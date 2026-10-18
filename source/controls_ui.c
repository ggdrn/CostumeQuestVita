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

void controls_ui_frame() {
    if (setting_controls_gamepad && is_gamepad)
        *is_gamepad = 1;
}

void controls_ui_patch() {
    hook_flash_log();
    if (setting_controls_gamepad)
        patch_gamepad_flag();
    filelog_printf("controls_ui: %s layout\n", setting_controls_gamepad ? "PlayStation (gamepad)" : "touch");
}
