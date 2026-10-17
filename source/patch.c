/*
 * Copyright (C) 2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  patch.c
 * @brief Patching some of the .so internal functions or bridging them to native
 *        for better compatibility.
 *
 * libCq.so statically links an early SDL2 (2.0.x) Android backend. Instead of
 * emulating EGL/ANativeWindow/AssetManager through JNI, the SDL entry points
 * that touch them are hooked here and bridged to vitaGL and stdio.
 */

#include <kubridge.h>
#include <so_util/so_util.h>
#include <stdio.h>
#include <string.h>
#include <vitasdk.h>
#include <vitaGL.h>

#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/settings.h"
#include "vita_pad.h"
#include "port_menu.h"
#include "utils/markers.h"
#include "reimpl/controls.h"

#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX (0x0C20D050)

#include "utils/logger.h"
#include "utils/dialog.h"
#include "reimpl/sys.h"
#include <stdbool.h>

void __kuser_memory_barrier(void) {
	__sync_synchronize();
}

static void kuser_patch_module(so_module *mod) {
	uint32_t patched_addr;
	for (uint32_t addr = mod->text_base; addr < mod->text_base + mod->text_size; addr += 4) {
		uint32_t *a = (uint32_t *)addr;
		if (*a == 0xFFFF0FC0) {
			l_debug("Patching 0x%x -> __kuser_cmpxchg", a);
			patched_addr = 0x9A000FC0;
			kuKernelCpuUnrestrictedMemcpy((void *)(addr), &patched_addr, sizeof(uint32_t));
		}
		else if (*a == 0xFFFF0FA0) {
			l_debug("Patching 0x%x -> __kuser_memory_barrier", a);
			patched_addr = 0x9A000FA0;
			kuKernelCpuUnrestrictedMemcpy((void *)(addr), &patched_addr, sizeof(uint32_t));
		}
	}
}

void kuser_patch(void) {
	SceKernelAllocMemBlockKernelOpt opt;
	memset(&opt, 0, sizeof(SceKernelAllocMemBlockKernelOpt));
	opt.size = sizeof(SceKernelAllocMemBlockKernelOpt);
	opt.attr = 0x1;
	opt.field_C = (SceUInt32)0x9A000000;
	if (kuKernelAllocMemBlock("atomic", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, 0x1000, &opt) < 0)
		fatal_error("Error could not allocate atomic block.");
	kuKernelMemProtect((void *)0x9A000000, (SceSize)0x1000, KU_KERNEL_PROT_EXEC | KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE);

	hook_addr(0x9A000FA0, (uintptr_t)__kuser_memory_barrier);
	hook_addr(0x9A000FC0, (uintptr_t)__atomic_cmpxchg);

	kuser_patch_module(&fmodex_mod);
	kuser_patch_module(&fmodevent_mod);
	kuser_patch_module(&so_mod);
}

/*
 * C++ exception unwinding: the .so files look up their own .ARM.exidx tables
 * through this bionic helper.
 */

typedef uintptr_t _Unwind_Ptr;

_Unwind_Ptr __gnu_Unwind_Find_exidx_soloader(_Unwind_Ptr pc, int *pcount) {
	so_module *mods[] = { &so_mod, &fmodevent_mod, &fmodex_mod };
	for (int i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
		so_module *m = mods[i];
		if (pc >= m->text_base && pc < m->text_base + m->text_size) {
			*pcount = m->exidx_size / 8;
			return m->exidx_base;
		}
	}
	*pcount = 0;
	return 0;
}

/*
 * SDL EGL layer -> vitaGL.
 * Signatures follow SDL 2.0.x src/video/SDL_egl.c; `_this` is SDL_VideoDevice*.
 */

#define FAKE_EGL_SURFACE ((void *)0x45474C53) // 'EGLS'
#define FAKE_EGL_CONTEXT ((void *)0x45474C43) // 'EGLC'
#define FAKE_NATIVE_WINDOW ((void *)0x4E57494E) // 'NWIN'

// Set once vitaGL is fully up; other threads must not query it before
volatile int gl_initialized = 0;
static int contexts_created = 0;

static const char *current_thread_name() {
	static __thread char name[32];
	SceKernelThreadInfo info;
	info.size = sizeof(info);
	if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) >= 0)
		snprintf(name, sizeof(name), "%s", info.name);
	else
		snprintf(name, sizeof(name), "?");
	return name;
}

int SDL_EGL_LoadLibrary_hook(void *_this, const char *path, void *native_display) {
	l_info("SDL_EGL_LoadLibrary(%s)", path ? path : "NULL");
	return 0;
}

void SDL_EGL_UnloadLibrary_hook(void *_this) {}

void *SDL_EGL_GetProcAddress_hook(void *_this, const char *proc) {
	void *ret = vglGetProcAddress(proc);
	if (!ret)
		l_warn("SDL_EGL_GetProcAddress(%s): not found", proc);
	return ret;
}

int SDL_EGL_ChooseConfig_hook(void *_this) {
	return 0;
}

void *SDL_EGL_CreateContext_hook(void *_this, void *egl_surface) {
	// Initialize vitaGL on the thread that is going to render (SDL thread)
	if (!gl_initialized) {
		sceIoMkdir("ux0:data/shader_cache", 0777); // vitaGL only creates the per-title folder
		gl_init();
		gl_initialized = 1;
		l_success("vitaGL initialized.");
	}
	// Distinct fake handles, to tell the game's contexts apart in the log
	void *ctx = (uint8_t *)FAKE_EGL_CONTEXT + 0x10 * contexts_created++;
	l_info("SDL_EGL_CreateContext() on thread 0x%08X \"%s\": %p", sceKernelGetThreadId(), current_thread_name(), ctx);
	return ctx;
}

int SDL_EGL_MakeCurrent_hook(void *_this, void *egl_surface, void *context) {
	return 0;
}

int SDL_EGL_SetSwapInterval_hook(void *_this, int interval) {
	return 0;
}

int SDL_EGL_GetSwapInterval_hook(void *_this) {
	return 1;
}

// Frame timing, read and reset by the heartbeat (main.c)
volatile uint32_t frame_count = 0;
volatile uint64_t frame_work_us = 0;  // game logic + GL submission
volatile uint64_t frame_swap_us = 0;  // waiting in vglSwapBuffers (GPU / vsync)
volatile uint32_t frame_max_us = 0;   // slowest whole frame

void SDL_EGL_SwapBuffers_hook(void *_this, void *egl_surface) {
	static uint64_t last_swap_end = 0;
	static int applied_cap = -1;
	uint64_t start = sceKernelGetProcessTimeWide();

	// Frame cap: 60 and 30 are vsync intervals 1 and 2 (each frame held for
	// one / two 60Hz refreshes). 45 can't be a multiple of the display rate,
	// so it is paced by sleeping, without vsync, like 0 (unlimited).
	if (applied_cap != setting_fps_cap) {
		extern uint32_t vsync_interval; // vitaGL
		applied_cap = setting_fps_cap;
		vsync_interval = setting_fps_cap == 60 ? 1 : setting_fps_cap == 30 ? 2 : 0;
	}
	if (last_swap_end && setting_fps_cap == 45) {
		uint64_t interval = 1000000 / 45;
		uint64_t elapsed = start - last_swap_end;
		if (elapsed < interval)
			sceKernelDelayThread((SceUInt)(interval - elapsed));
	}

	controls_poll();
	gl_swap();

	// The game is paused while the menu is open; that time is not a frame
	if (port_menu_check()) {
		port_menu_run();
		last_swap_end = sceKernelGetProcessTimeWide();
		return;
	}

	uint64_t end = sceKernelGetProcessTimeWide();
	if (last_swap_end) {
		uint32_t work = (uint32_t)(start - last_swap_end);
		uint32_t frame = (uint32_t)(end - last_swap_end);
		frame_work_us += work;
		frame_swap_us += end - start;
		if (frame > frame_max_us)
			frame_max_us = frame;
		frame_count++;
	}
	last_swap_end = end;
}

void SDL_EGL_DeleteContext_hook(void *_this, void *context) {}

void *SDL_EGL_CreateSurface_hook(void *_this, void *native_window) {
	return FAKE_EGL_SURFACE;
}

void SDL_EGL_DestroySurface_hook(void *_this, void *egl_surface) {}

// The original also calls eglWaitNative/eglWaitGL through _this->egl_data,
// which we never allocate.
void Android_GLES_SwapWindow_hook(void *_this, void *window) {
	SDL_EGL_SwapBuffers_hook(_this, NULL);
}

void *Android_JNI_GetNativeWindow_hook(void) {
	return FAKE_NATIVE_WINDOW;
}

/*
 * SDL Android file IO (SDL_RWFromFile with a relative path) -> stdio.
 * SDL_RWops on 32-bit SDL 2.0.x: 5 function pointers, Uint32 type, then the
 * `hidden` union at offset 24. We keep our FILE* in hidden.unknown.data1.
 */

typedef struct {
	void *size, *seek, *read, *write, *close;
	uint32_t type;
	FILE *fp;
} RWopsAndroid;

static void (*SDL_FreeRW)(void *ctx);

int Android_JNI_FileOpen_hook(RWopsAndroid *ctx, const char *fileName, const char *mode) {
	char path[512];
	if (strchr(fileName, ':'))
		snprintf(path, sizeof(path), "%s", fileName);
	else
		snprintf(path, sizeof(path), DATA_PATH "assets/%s", fileName);

	ctx->fp = fopen(path, mode);
	if (!ctx->fp) {
		l_warn("Android_JNI_FileOpen(%s): not found", path);
		return -1;
	}
	l_debug("Android_JNI_FileOpen(%s): %p", path, ctx->fp);
	return 0;
}

size_t Android_JNI_FileRead_hook(RWopsAndroid *ctx, void *buffer, size_t size, size_t maxnum) {
	return fread(buffer, size, maxnum, ctx->fp);
}

size_t Android_JNI_FileWrite_hook(RWopsAndroid *ctx, const void *buffer, size_t size, size_t num) {
	return fwrite(buffer, size, num, ctx->fp);
}

int64_t Android_JNI_FileSize_hook(RWopsAndroid *ctx) {
	long pos = ftell(ctx->fp);
	fseek(ctx->fp, 0, SEEK_END);
	long size = ftell(ctx->fp);
	fseek(ctx->fp, pos, SEEK_SET);
	return size;
}

int64_t Android_JNI_FileSeek_hook(RWopsAndroid *ctx, int64_t offset, int whence) {
	if (fseek(ctx->fp, (long)offset, whence) != 0)
		return -1;
	return ftell(ctx->fp);
}

int Android_JNI_FileClose_hook(RWopsAndroid *ctx) {
	if (ctx->fp)
		fclose(ctx->fp);
	ctx->fp = NULL;
	SDL_FreeRW(ctx);
	return 0;
}

static void hook_cq(const char *symbol, void *dst) {
	uintptr_t addr = so_symbol(&so_mod, symbol);
	if (!addr) {
		l_error("Can't hook %s: symbol not found", symbol);
		return;
	}
	hook_addr(addr, (uintptr_t)dst);
}

void so_patch(void) {
	kuser_patch();

	SDL_FreeRW = (void *)so_symbol(&so_mod, "SDL_FreeRW");

	// Vita controls as an SDL game controller (analog sticks included)
	vita_pad_patch();

	// The engine's own (empty) profiler markers, timed
	markers_patch();

	// EGL / window
	hook_cq("SDL_EGL_LoadLibrary", SDL_EGL_LoadLibrary_hook);
	hook_cq("SDL_EGL_UnloadLibrary", SDL_EGL_UnloadLibrary_hook);
	hook_cq("SDL_EGL_GetProcAddress", SDL_EGL_GetProcAddress_hook);
	hook_cq("SDL_EGL_ChooseConfig", SDL_EGL_ChooseConfig_hook);
	hook_cq("SDL_EGL_CreateContext", SDL_EGL_CreateContext_hook);
	hook_cq("SDL_EGL_MakeCurrent", SDL_EGL_MakeCurrent_hook);
	hook_cq("SDL_EGL_SetSwapInterval", SDL_EGL_SetSwapInterval_hook);
	hook_cq("SDL_EGL_GetSwapInterval", SDL_EGL_GetSwapInterval_hook);
	hook_cq("SDL_EGL_SwapBuffers", SDL_EGL_SwapBuffers_hook);
	hook_cq("SDL_EGL_DeleteContext", SDL_EGL_DeleteContext_hook);
	hook_cq("SDL_EGL_CreateSurface", SDL_EGL_CreateSurface_hook);
	hook_cq("SDL_EGL_DestroySurface", SDL_EGL_DestroySurface_hook);
	hook_cq("Android_GLES_SwapWindow", Android_GLES_SwapWindow_hook);
	hook_cq("Android_JNI_GetNativeWindow", Android_JNI_GetNativeWindow_hook);

	// Asset IO
	hook_cq("Android_JNI_FileOpen", Android_JNI_FileOpen_hook);
	hook_cq("Android_JNI_FileRead", Android_JNI_FileRead_hook);
	hook_cq("Android_JNI_FileWrite", Android_JNI_FileWrite_hook);
	hook_cq("Android_JNI_FileSize", Android_JNI_FileSize_hook);
	hook_cq("Android_JNI_FileSeek", Android_JNI_FileSeek_hook);
	hook_cq("Android_JNI_FileClose", Android_JNI_FileClose_hook);
}
