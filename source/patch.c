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
#include "utils/luaprof.h"
#include "controls_ui.h"
#include "particle_compat.h"
#include "particle_trace.h"
#include "particle_progressive.h"
#include "reimpl/controls.h"

#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX (0x0C20D050)

#include "utils/logger.h"
#include "utils/filelog.h"
#include "reimpl/fdcache.h"
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
static void particle_trace_frame(uint32_t wall_us);

void SDL_EGL_SwapBuffers_hook(void *_this, void *egl_surface) {
	static uint64_t last_swap_end = 0;
	static int applied_cap = -1;
	static uint32_t previous_reads;
	static uint64_t previous_card_us;
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

	controls_ui_frame();
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
		particle_trace_frame(frame);
		if (setting_profiler && frame >= 250000) {
			filelog_printf("hitch: frame %u ms (game %u + swap %u), process card reads +%u, card IO +%u ms\n",
			               frame / 1000, work / 1000, (unsigned)((end - start) / 1000),
			               fdcache_stats.refills - previous_reads,
			               (unsigned)((fdcache_stats.refill_us - previous_card_us) / 1000));
		}
	}
	previous_reads = fdcache_stats.refills;
	previous_card_us = fdcache_stats.refill_us;
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

typedef void (*ParticleWarmup)(void *, const void *, void *, const void *, void *);
static ParticleWarmup orig_warmup[2];
static void (*orig_particle_reset)(void *);
/* ARM C++ destructors return this, unlike an ordinary void C function. */
static void *(*orig_particle_destruct)(void *);
static ParticleTrace particle_trace;
static SceKernelLwMutexWork particle_trace_lock;
static int particle_trace_ready;
static uint32_t particle_frame = 1;
static ParticleProgressive particle_progressive;
static int particle_progressive_ready;
static unsigned particle_progressive_records;
static void (*orig_particle_step)(void *, const void *, float, void *, const void *);
static const char *(*particle_asset_name)(const void *);
static uint32_t particle_named_resources[64];
static unsigned particle_named_count;

static void particle_trace_name(void *instance, uint32_t resource) {
    if (!particle_asset_name || !(resource & 1)) return;
    sceKernelLockLwMutex(&particle_trace_lock, 1, NULL);
    for (unsigned i = 0; i < particle_named_count; ++i) {
        if (particle_named_resources[i] == resource) {
            sceKernelUnlockLwMutex(&particle_trace_lock, 1);
            return;
        }
    }
    if (particle_named_count >= 64) {
        sceKernelUnlockLwMutex(&particle_trace_lock, 1);
        return;
    }
    particle_named_resources[particle_named_count++] = resource;
    sceKernelUnlockLwMutex(&particle_trace_lock, 1);
    /* The current native task still owns the resource reference. Resolve now,
     * outside our mutex, and copy into the async logger before it can expire.
     * Never retain the returned native string or resolve it on another thread. */
    const char *name = particle_asset_name(instance);
    filelog_printf("particle asset: resource=%08X name=%.384s\n",
                   resource, name ? name : "(unavailable)");
}

static void particle_reset_hook(void *instance) {
    orig_particle_reset(instance);
    if (!setting_profiler && !setting_particle_leaves) return;
    uint64_t now = sceKernelGetProcessTimeWide();
    sceKernelLockLwMutex(&particle_trace_lock, 1, NULL);
    particle_progressive_cancel(&particle_progressive, (uintptr_t)instance);
    particle_trace_reset(&particle_trace, (uintptr_t)instance, now);
    sceKernelUnlockLwMutex(&particle_trace_lock, 1);
}

static void *particle_destruct_hook(void *instance) {
    sceKernelLockLwMutex(&particle_trace_lock, 1, NULL);
    particle_progressive_cancel(&particle_progressive, (uintptr_t)instance);
    particle_trace_retire(&particle_trace, (uintptr_t)instance);
    sceKernelUnlockLwMutex(&particle_trace_lock, 1);
    return orig_particle_destruct(instance);
}

static void particle_trace_frame(uint32_t wall_us) {
    static uint64_t last_summary;
    static unsigned frame_records;
    if (!particle_trace_ready || (!setting_profiler && !setting_particle_leaves)) return;
    uint64_t now = sceKernelGetProcessTimeWide();
    sceKernelLockLwMutex(&particle_trace_lock, 1, NULL);
    uint32_t frame = particle_frame++;
    particle_progressive_frame(&particle_progressive, particle_frame);
    uint32_t calls = particle_trace.frame_calls;
    uint64_t sum = particle_trace.frame_sum_us;
    particle_trace.frame_calls = 0;
    particle_trace.frame_sum_us = 0;
    int summary = now - last_summary >= 5000000;
    /* Snapshot counters only; never copy the complete identity table. */
    uint64_t all_calls = particle_trace.calls, slow = particle_trace.slow;
    uint64_t total = particle_trace.total_us, max = particle_trace.max_us;
    unsigned active = particle_trace.active, lost = particle_trace.untracked;
    unsigned records = particle_trace.records;
    unsigned pending = particle_progressive_pending(&particle_progressive);
    unsigned started = particle_progressive.started, completed = particle_progressive.completed;
    unsigned cancelled = particle_progressive.cancelled, fallback = particle_progressive.fallback;
    unsigned steps = particle_progressive.steps, max_step = particle_progressive.max_step_us;
    unsigned overruns = particle_progressive.overruns;
    sceKernelUnlockLwMutex(&particle_trace_lock, 1);
    if (!setting_profiler) return;
    if (calls && wall_us >= 50000 && frame_records++ < PARTICLE_TRACE_RECORDS)
        filelog_printf("particle frame: id=%u wall_us=%u finished=%u sum_call_us=%llu\n",
                       frame, wall_us, calls, (unsigned long long)sum);
    if (summary) {
        last_summary = now;
        filelog_printf("particle trace totals: calls=%llu slow=%llu sum_us=%llu max_us=%llu active=%u untracked_resets=%u detail_records=%u\n",
                       (unsigned long long)all_calls, (unsigned long long)slow,
                       (unsigned long long)total, (unsigned long long)max,
                       active, lost, records);
        if (particle_progressive_ready)
            filelog_printf("particle leaves totals: started=%u pending=%u completed=%u cancelled=%u fallback=%u steps=%u max_step_us=%u budget_overruns=%u\n",
                           started, pending, completed, cancelled, fallback, steps, max_step, overruns);
    }
}

static void particle_step_hook(void *instance, const void *data, float dt,
                               void *rng, const void *noise) {
    ParticleProgressiveTicket ticket;
    int advance = 0;
    int32_t state;
    memcpy(&state, (char *)instance + 8, sizeof(state));
    if (setting_particle_leaves && data && state <= 2) {
        sceKernelLockLwMutex(&particle_trace_lock, 1, NULL);
        /* Only tracked jobs need a generation lookup on this hot path. */
        int pending = 0;
        for (unsigned i = 0; i < PARTICLE_PROGRESSIVE_JOBS; ++i)
            pending |= particle_progressive.jobs[i].instance == (uintptr_t)instance;
        if (pending) {
            ParticleTraceEntry identity = particle_trace_find(&particle_trace, (uintptr_t)instance);
            advance = particle_progressive_reserve(&particle_progressive,
                (uintptr_t)instance, identity.generation, dt, &ticket);
        }
        sceKernelUnlockLwMutex(&particle_trace_lock, 1);
    }
    if (!advance) {
        orig_particle_step(instance, data, dt, rng, noise);
        return;
    }
    uint64_t start = sceKernelGetProcessTimeWide();
    orig_particle_step(instance, data, dt + ticket.extra, rng, noise);
    uint64_t elapsed = sceKernelGetProcessTimeWide() - start;
    sceKernelLockLwMutex(&particle_trace_lock, 1, NULL);
    int completed = particle_progressive_finish(&particle_progressive, ticket,
        elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed);
    int log = completed && setting_profiler && particle_progressive_records < 256;
    if (log) ++particle_progressive_records;
    sceKernelUnlockLwMutex(&particle_trace_lock, 1);
    if (log)
        filelog_printf("particle leaves: complete instance=%08X generation=%u\n",
                       (unsigned)(uintptr_t)instance, ticket.generation);
}

static int particle_leaves_start(int kind, void *instance) {
    if (!particle_progressive_ready || !setting_particle_leaves || kind != 1)
        return 0;
    float requested;
    uint16_t capacity;
    memcpy(&requested, (char *)instance + 308, sizeof(requested));
    memcpy(&capacity, (char *)instance + 16, sizeof(capacity));
    /* Limit progressive settling to the exact asset and parameters tested on Vita. */
    if (requested != 20.0f || capacity != 661) return 0;
    const char *name = particle_asset_name(instance);
    if (!particle_progressive_matches(name, kind, requested, capacity)) return 0;
    sceKernelLockLwMutex(&particle_trace_lock, 1, NULL);
    ParticleTraceEntry identity = particle_trace_find(&particle_trace, (uintptr_t)instance);
    int queued = particle_progressive_start(&particle_progressive,
        (uintptr_t)instance, identity.generation, requested);
    int log = queued && setting_profiler && particle_progressive_records < 256;
    if (log) ++particle_progressive_records;
    sceKernelUnlockLwMutex(&particle_trace_lock, 1);
    if (!queued) return 0; // bounded queue full/unknown lifetime: use native warmup
    float zero = 0;
    memcpy((char *)instance + 308, &zero, sizeof(zero));
    if (log)
        filelog_printf("particle leaves: start instance=%08X generation=%u extra_sim_seconds=20\n",
                       (unsigned)(uintptr_t)instance, identity.generation);
    return 1;
}

static void particle_warmup_timed(int kind, void *instance, const void *data,
                                void *rng, const void *noise, void *particles) {
    if (particle_leaves_start(kind, instance)) return;
    if (!setting_profiler) {
        orig_warmup[kind](instance, data, rng, noise, particles);
        return;
    }
    float requested;
    memcpy(&requested, (char *)instance + 308, sizeof(requested));
    uint16_t before, after, capacity;
    uint32_t resource;
    memcpy(&before, (char *)instance + 18, sizeof(before));
    memcpy(&capacity, (char *)instance + 16, sizeof(capacity));
    memcpy(&resource, instance, sizeof(resource));
    uint64_t start = sceKernelGetProcessTimeWide();
    ParticleTraceEntry identity = {0};
    uint32_t frame = 0;
    if (particle_trace_ready) {
        sceKernelLockLwMutex(&particle_trace_lock, 1, NULL);
        identity = particle_trace_find(&particle_trace, (uintptr_t)instance);
        frame = particle_frame;
        sceKernelUnlockLwMutex(&particle_trace_lock, 1);
    }
    /* Exclude bookkeeping from the native routine's measured time. */
    uint64_t native_start = sceKernelGetProcessTimeWide();
    orig_warmup[kind](instance, data, rng, noise, particles);
    uint64_t elapsed = sceKernelGetProcessTimeWide() - native_start;
    memcpy(&after, (char *)instance + 18, sizeof(after));
    int detail = 0;
    uint32_t end_frame = 0;
    if (particle_trace_ready) {
        sceKernelLockLwMutex(&particle_trace_lock, 1, NULL);
        detail = particle_trace_finish(&particle_trace, elapsed);
        end_frame = particle_frame;
        sceKernelUnlockLwMutex(&particle_trace_lock, 1);
    }
    if (detail) {
        particle_trace_name(instance, resource);
        filelog_printf("particle trace: kind=%s instance=%08X generation=%u resource=%08X data=%08X frame=%u end_frame=%u thread=%08X reset_age_ms=%lld requested=%.3f before=%u after=%u capacity=%u rng=%08X noise=%08X buffer=%08X us=%llu\n",
                       kind ? "particle" : "ribbon", (unsigned)(uintptr_t)instance,
                       identity.generation, resource, (unsigned)(uintptr_t)data,
                       frame, end_frame, (unsigned)sceKernelGetThreadId(),
                       identity.generation ? (long long)((start - identity.reset_us) / 1000) : -1LL,
                       (double)requested, before, after, capacity,
                       (unsigned)(uintptr_t)rng, (unsigned)(uintptr_t)noise,
                       (unsigned)(uintptr_t)particles, (unsigned long long)elapsed);
    } else if (!particle_trace_ready && elapsed >= 50000)
        filelog_printf("particle warmup: %s requested %.3f s, took %llu ms\n",
                       kind ? "particle" : "ribbon", (double)requested,
                       (unsigned long long)(elapsed / 1000));
}

static void ribbon_warmup_hook(void *i, const void *d, void *r, const void *n, void *p) {
    particle_warmup_timed(0, i, d, r, n, p);
}
static void particle_warmup_hook(void *i, const void *d, void *r, const void *n, void *p) {
    particle_warmup_timed(1, i, d, r, n, p);
}

static void patch_particle_warmup(void) {
    const char *symbols[] = {
        "_ZN22ParticleSystemInstance16_WarmupParticlesI11RibbonStateEEvPK18ParticleSystemDataR9FastFRandRK12NoiseMachinePT_",
        "_ZN22ParticleSystemInstance16_WarmupParticlesI13ParticleStateEEvPK18ParticleSystemDataR9FastFRandRK12NoiseMachinePT_"
    };
    uintptr_t addresses[2];
    uint32_t code[0x208 / 4];
    /* Validate both before modifying either; do not patch unknown APK code. */
    for (int i = 0; i < 2; ++i) {
        addresses[i] = so_symbol(&so_mod, symbols[i]);
        if (!addresses[i]) {
            filelog_printf("particle warmup: skipped (missing symbol)\n");
            return;
        }
        memcpy(code, (void *)addresses[i], sizeof(code));
        if (!particle_warmup_prepare(code, sizeof(code))) {
            filelog_printf("particle warmup: skipped (unsupported instructions)\n");
            return;
        }
    }
    uintptr_t reset = so_symbol(&so_mod, "_ZN22ParticleSystemInstance5ResetEv");
    uintptr_t destruct = so_symbol(&so_mod, "_ZN22ParticleSystemInstanceD1Ev");
    const uint32_t reset_prefix[] = {0xe92d4010, 0xe1a04000};
    const uint32_t destruct_prefix[] = {0xe92d4830, 0xe24dd010};
    if (reset && destruct && !memcmp((void *)reset, reset_prefix, sizeof(reset_prefix)) &&
        !memcmp((void *)destruct, destruct_prefix, sizeof(destruct_prefix)) &&
        sceKernelCreateLwMutex(&particle_trace_lock, "particle_trace", 0, 0, NULL) == 0) {
        orig_particle_reset = (void *)hook_arm_trampoline(&so_mod, reset, (uintptr_t)particle_reset_hook);
        orig_particle_destruct = (void *)hook_arm_trampoline(&so_mod, destruct, (uintptr_t)particle_destruct_hook);
        particle_trace_ready = orig_particle_reset && orig_particle_destruct;
    }
    uintptr_t get_name = so_symbol(&so_mod, "_ZNK9RsRefBase12GetAssetNameEv");
    const uint32_t name_prefix[] = {0xe92d4800, 0xe5901000, 0xe3a00000, 0xe3110001};
    if (particle_trace_ready && get_name &&
        !memcmp((void *)get_name, name_prefix, sizeof(name_prefix)))
        particle_asset_name = (void *)get_name;
    for (int i = 0; i < 2; ++i) {
        kuKernelCpuUnrestrictedMemcpy((void *)(addresses[i] + 0x2c),
                                     &code[0x2c / 4], sizeof(uint32_t));
        orig_warmup[i] = (ParticleWarmup)hook_arm_trampoline(&so_mod, addresses[i],
            (uintptr_t)(i ? particle_warmup_hook : ribbon_warmup_hook));
        if (!orig_warmup[i])
            filelog_printf("particle warmup: timing hook %d unavailable\n", i);
    }
    uintptr_t step = so_symbol(&so_mod, "_ZN22ParticleSystemInstance13StepParticlesEPK18ParticleSystemDatafR9FastFRandRK12NoiseMachine");
    const uint32_t step_prefix[] = {0xe92d4ff0, 0xed2d8b08, 0xe24dd01c, 0xe1a04000};
    if (orig_warmup[1] && particle_asset_name && step && !memcmp((void *)step, step_prefix, sizeof(step_prefix))) {
        orig_particle_step = (void *)hook_arm_trampoline(&so_mod, step, (uintptr_t)particle_step_hook);
        particle_progressive_ready = orig_particle_step != NULL;
    }
    particle_progressive_frame(&particle_progressive, particle_frame);
    filelog_printf("particle leaves progressive v1: %s; %u us shared target, normal-step cap %.6f s; gradual initial density\n",
                   particle_progressive_ready && setting_particle_leaves ? "enabled" : "disabled",
                   PARTICLE_PROGRESSIVE_BUDGET_US, (double)PARTICLE_PROGRESSIVE_MAX_DT);
    filelog_printf("particle trace v2: %s, %u identity slots, %u detail records; native asset names %s\n",
                   particle_trace_ready ? "enabled with profiler" : "unavailable",
                   PARTICLE_TRACE_SLOTS, PARTICLE_TRACE_RECORDS,
                   particle_asset_name ? "enabled" : "unavailable");
    filelog_printf("particle warmup: requested duration, capped at 90 s (both routines)\n");
}

void so_patch(void) {
	kuser_patch();

	SDL_FreeRW = (void *)so_symbol(&so_mod, "SDL_FreeRW");

	// Vita controls as an SDL game controller (analog sticks included)
	vita_pad_patch();

	// The engine's own (empty) profiler markers, timed
	markers_patch();
	luaprof_patch();

	// PlayStation-layout controller UI
	controls_ui_patch();
	patch_particle_warmup();

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
