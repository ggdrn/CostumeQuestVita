/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  controls_ui.c
 * @brief Controller (PlayStation layout) UI for the game.
 *
 * The game keeps the console code path of every screen (also used by the
 * OUYA build); VirtualGamepadManager::m_bIsGamepad selects it. This file
 * makes that path usable on the Vita:
 *
 * - Flash log: Scaleform's messages (missing imports, ActionScript errors)
 *   went to an empty FlashLogger::LogMessageVarg; they now reach debug.log.
 * - Gamepad flag: with "controls gamepad" m_bIsGamepad is held at 1. The
 *   game clears it whenever touch (mouse) or keyboard input shows up, in
 *   three stores in VirtualGamepadManager::_ReadPhysicalInputs; those are
 *   removed, and the flag is set every frame (the game only sets it itself on
 *   the first controller input).
 * - Icons: HUDs import their icons from "Shared/ControllerIcons.swf", which
 *   DF_TranslateFlashFilePath maps to Mobile_InputIcons (touch symbols
 *   only). With gamepad it maps to Vita_ControllerIcons instead, written at
 *   boot by icons_movie.c (PlayStation buttons + touch symbols). The game
 *   finds it through its loose-file path (/sdcard/dfp/cq-droid/Data/UI/
 *   Vita_..., mapped in reimpl/io.c), and the PlayStation button images
 *   ("PS3__<name>") are redirected to their folder in RsRef<Texture>::BindFile.
 */

#include "controls_ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <so_util/so_util.h>

#include "utils/filelog.h"
#include "utils/init.h"
#include "utils/logger.h"
#include "utils/settings.h"

#include <kubridge.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include "icons_movie.h"

/*
 * Flash log
 */

#define FLASH_LOG_MAX 400 // messages per run: an error repeated every frame must not flood the log

static volatile int flash_log_count;

static void FlashLogger_LogMessageVarg(void *this, int type, const char *fmt, va_list ap) {
    if (!fmt || flash_log_count > FLASH_LOG_MAX)
        return;
    if (++flash_log_count > FLASH_LOG_MAX) {
        filelog_printf("flash: more messages suppressed\n");
        return;
    }
    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    size_t n = strlen(msg);
    while (n && (msg[n - 1] == '\n' || msg[n - 1] == '\r'))
        msg[--n] = 0;
    if (n)
        filelog_printf("flash[%d]: %s\n", type, msg);
}

static void hook_flash_log() {
    uintptr_t addr = so_symbol(&so_mod, "_ZN11FlashLogger14LogMessageVargEN15GFxLogConstants14LogMessageTypeEPKcSt9__va_list");
    if (!addr || *(uint32_t *)addr != 0xE12FFF1E) { // bx lr
        l_error("controls_ui: FlashLogger::LogMessageVarg missing or not empty");
        return;
    }
    if (hook_arm_short(&so_mod, addr, (uintptr_t)FlashLogger_LogMessageVarg) < 0)
        l_error("controls_ui: no veneer space for the Flash log");
}

/*
 * Gamepad flag
 */

#define RPI_FUNC 0x6e4b6c
static const uint32_t rpi_clear_gamepad[] = { 0x6e67c4, 0x6e691c, 0x6e6a20 };
static volatile uint8_t *is_gamepad;

static void patch_gamepad_flag() {
    is_gamepad = (uint8_t *)so_symbol(&so_mod, "_ZN21VirtualGamepadManager12m_bIsGamepadE");
    uintptr_t func = so_symbol(&so_mod, "_ZN21VirtualGamepadManager19_ReadPhysicalInputsERN14VirtualGamepad5StateERNS_7VGEntryEbj");
    if (!is_gamepad || !func) {
        l_error("controls_ui: VirtualGamepadManager symbols missing");
        return;
    }
    const uint32_t nop = 0xE320F000;
    for (int i = 0; i < sizeof(rpi_clear_gamepad) / sizeof(*rpi_clear_gamepad); i++) {
        uint32_t *insn = (uint32_t *)(func + rpi_clear_gamepad[i] - RPI_FUNC);
        // strb rX, [rY] (any condition): keep away from another build
        if ((*insn & 0x0FF00FFF) != 0x05C00000) {
            l_error("controls_ui: unexpected insn 0x%08x at _ReadPhysicalInputs+0x%x", *insn, rpi_clear_gamepad[i] - RPI_FUNC);
            continue;
        }
        kuKernelCpuUnrestrictedMemcpy(insn, &nop, sizeof(nop));
    }
    *is_gamepad = 1;
}

/*
 * Icons
 */

#define MOVIE_DIR   DATA_PATH "Data/UI/Vita_ControllerIcons/Opt/"
#define MOVIE_FILE  MOVIE_DIR "Vita_ControllerIcons.gfx"
#define MOVIE_URL   "Data/UI/Vita_ControllerIcons/Opt/Vita_ControllerIcons.gfx"
#define MOBILE_URL  "Data/UI/Mobile_InputIcons/Opt/Mobile_InputIcons.gfx"

static int write_movie() {
    static uint8_t movie[16384];
    size_t len = icons_movie_build(movie, sizeof(movie));
    if (!len) {
        l_error("controls_ui: icon movie build failed");
        return 0;
    }
    // Rewrite only when it changed
    SceUID fd = sceIoOpen(MOVIE_FILE, SCE_O_RDONLY, 0);
    if (fd >= 0) {
        static uint8_t old[sizeof(movie)];
        int n = sceIoRead(fd, old, sizeof(old));
        sceIoClose(fd);
        if (n == (int)len && memcmp(old, movie, len) == 0)
            return 1;
    }
    sceIoMkdir(DATA_PATH "Data", 0777);
    sceIoMkdir(DATA_PATH "Data/UI", 0777);
    sceIoMkdir(DATA_PATH "Data/UI/Vita_ControllerIcons", 0777);
    sceIoMkdir(MOVIE_DIR, 0777);
    fd = sceIoOpen(MOVIE_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) {
        l_error("controls_ui: can't write %s: 0x%08x", MOVIE_FILE, fd);
        return 0;
    }
    int ok = sceIoWrite(fd, movie, len) == (int)len;
    sceIoClose(fd);
    filelog_printf("controls_ui: wrote %s (%u bytes, %d symbols)\n", MOVIE_FILE, (unsigned)len, icons_movie_count());
    return ok;
}

static int use_vita_icons;
static volatile uint32_t icon_redirects;

// Same as the game's, except for the controller icon library
static void DF_TranslateFlashFilePath_hook(const char *in, char *out) {
    const char *shared = strstr(in, "/Shared/");
    if (!shared) {
        snprintf(out, 260, "%s", in);
        return;
    }
    const char *file = shared + 8;
    if (strcmp(file, "ControllerIcons.gfx") == 0) {
        snprintf(out, 260, "%s", use_vita_icons ? MOVIE_URL : MOBILE_URL);
        return;
    }
    char name[260];
    int n = 0;
    while (file[n] && file[n] != '.' && n < 259) {
        name[n] = file[n];
        n++;
    }
    name[n] = 0;
    snprintf(out, 260, "Data/UI/%s/Opt/%s", name, file);
}

// RsRef<Texture> RsRef<Texture>::BindFile(const char *path, bool)
#define TEX_BINDFILE            0x91aa0c
#define TEX_DESCRIPTOR_GOT_SLOT 0xc44b04

typedef void *(*RsBindFile)(const void *descriptor, const char *path, int flag);
static RsBindFile rs_bind_file;
static const void **tex_descriptor_slot;

static void *TextureBindFile_hook(const char *path, int flag) {
    char buf[260];
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (strncmp(base, ICONS_PS3_PREFIX, sizeof(ICONS_PS3_PREFIX) - 1) == 0) {
        snprintf(buf, sizeof(buf), "UI/Flash/PS3_ControllerIcons/%s", base + sizeof(ICONS_PS3_PREFIX) - 1);
        if (icon_redirects++ < 4)
            filelog_printf("controls_ui: texture %s -> %s\n", path, buf);
        path = buf;
    }
    return rs_bind_file(*tex_descriptor_slot, path, flag);
}

static void patch_icons() {
    uintptr_t translate = so_symbol(&so_mod, "_Z25DF_TranslateFlashFilePathPKcPc");
    uintptr_t bind = so_symbol(&so_mod, "_ZN5RsRefI7TextureE8BindFileEPKcb");
    rs_bind_file = (RsBindFile)so_symbol(&so_mod, "_ZN9RsRefBase9_BindFileEPK12RsDescriptorPKcb");
    if (!translate || !bind || !rs_bind_file) {
        l_error("controls_ui: icon symbols missing");
        return;
    }
    if (!write_movie())
        return; // keep the game's own mapping
    tex_descriptor_slot = (const void **)(bind - TEX_BINDFILE + TEX_DESCRIPTOR_GOT_SLOT);
    hook_addr(bind, (uintptr_t)TextureBindFile_hook);
    hook_addr(translate, (uintptr_t)DF_TranslateFlashFilePath_hook);
    use_vita_icons = 1;
}

void controls_ui_frame() {
    if (setting_controls_gamepad && is_gamepad)
        *is_gamepad = 1;
}

void controls_ui_patch() {
    hook_flash_log();
    if (setting_controls_gamepad) {
        patch_gamepad_flag();
        patch_icons();
    }
    filelog_printf("controls_ui: %s layout\n", setting_controls_gamepad ? "PlayStation (gamepad)" : "touch");
}
