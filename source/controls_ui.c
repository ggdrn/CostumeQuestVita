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
 *   only). The game only opens files inside the APK, so the reads of that
 *   file's bytes return the combined library from icons_movie.c instead
 *   (PlayStation buttons + touch symbols; reimpl/fdcache.c), and the
 *   PlayStation button images ("PS3__<name>") are redirected to their folder
 *   in RsRef<Texture>::BindFile.
 * - Battle HUD: BattleHUD_DLC shows its console frames (action labels next
 *   to the face buttons, button prompts in minigames) when
 *   _global.ShowPCButtons is false; its init() sets it to true and nothing
 *   else touches it, so Android always shows the touch frames. With gamepad
 *   it is set to false with the other battle HUD variables every frame
 *   (after Battle_PushFlashVars).
 * - Battle minigames: the Android build judges them only from touch input
 *   (Battle_GetInputType: a tap, or a swipe in one of 4 directions, in a
 *   1280x720 space; Battle_GetButtonOverlap: is the tap on the slot where
 *   the prompt's icon is drawn). During a minigame a face button becomes a
 *   tap on the slot of that button's icon (asked to the game, so a wrong
 *   button misses, as a wrong tap would) and the D-pad becomes a swipe.
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
#include <psp2/ctrl.h>

#include "apk_index.h"
#include "icons_movie.h"
#include "reimpl/fdcache.h"

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

#define ICONS_ENTRY "Data/UI/Mobile_InputIcons/Opt/Mobile_InputIcons.gfx"

static uint8_t icons_data[4096]; // the entry's bytes as served to the game
static volatile uint32_t icon_redirects;

// The game keeps reading its own Mobile_InputIcons.gfx (where every HUD's
// "Shared/ControllerIcons.swf" import leads); reads of that file's bytes in
// the APK return the combined library, zero-padded to the original size.
static int install_icons() {
    uint32_t offset, size;
    if (apk_index_find(APK_PATH, ICONS_ENTRY, &offset, &size) != 0) {
        l_error("controls_ui: %s not found (or compressed) in the game data", ICONS_ENTRY);
        return 0;
    }
    size_t len = icons_movie_build(icons_data, sizeof(icons_data));
    if (!len || len > size || size > sizeof(icons_data)) {
        l_error("controls_ui: icon library doesn't fit (%u bytes, entry %u)", (unsigned)len, size);
        return 0;
    }
    memset(icons_data + len, 0, size - len);
    if (fdcache_set_apk_overlay(offset, icons_data, size) != 0) {
        l_error("controls_ui: can't serve the icon library");
        return 0;
    }
    filelog_printf("controls_ui: icon library (%d symbols, %u of %u bytes) served at APK offset %u\n",
                   icons_movie_count(), (unsigned)len, size, offset);
    return 1;
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
    uintptr_t bind = so_symbol(&so_mod, "_ZN5RsRefI7TextureE8BindFileEPKcb");
    rs_bind_file = (RsBindFile)so_symbol(&so_mod, "_ZN9RsRefBase9_BindFileEPK12RsDescriptorPKcb");
    if (!bind || !rs_bind_file) {
        l_error("controls_ui: texture symbols missing");
        return;
    }
    if (!install_icons())
        return; // the game keeps its touch icons
    tex_descriptor_slot = (const void **)(bind - TEX_BINDFILE + TEX_DESCRIPTOR_GOT_SLOT);
    hook_addr(bind, (uintptr_t)TextureBindFile_hook);
}

/*
 * Battle input diagnostics: what the battle code sees when buttons are
 * pressed (VirtualGamepad state bytes), its input state and minigame.
 */

#define BATTLE_LOG_MAX 3000
#define VG_FIRST 170 // 160-169 change every frame (counters)
#define VG_LAST  300

static void (*orig_battle_input)(const uint8_t *pad);
static volatile int32_t *battle_input_state, *battle_minigame;
static volatile uint8_t *battle_mg_buttons, *battle_mg_success, *battle_mg_failed;
static int battle_log_count;

static void Battle_HandleInput_hook(const uint8_t *pad) {
    static uint8_t last[VG_LAST];
    static int32_t last_state = -100, last_mg = -100, last_gp = -1;
    static uint8_t last_btn[12], last_ok = 0xFF, last_fail = 0xFF;

    if (battle_log_count < BATTLE_LOG_MAX) {
        int32_t st = *battle_input_state, mg = *battle_minigame, gp = is_gamepad ? *is_gamepad : -1;
        if (st != last_state || mg != last_mg || gp != last_gp || memcmp(last_btn, (const void *)battle_mg_buttons, 12)
            || *battle_mg_success != last_ok || *battle_mg_failed != last_fail) {
            filelog_printf("battle: input state %d, minigame %d, gamepad flag %d, mg buttons %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x, success %d failed %d\n",
                           st, mg, gp,
                           battle_mg_buttons[0], battle_mg_buttons[1], battle_mg_buttons[2], battle_mg_buttons[3],
                           battle_mg_buttons[4], battle_mg_buttons[5], battle_mg_buttons[6], battle_mg_buttons[7],
                           battle_mg_buttons[8], battle_mg_buttons[9], battle_mg_buttons[10], battle_mg_buttons[11],
                           *battle_mg_success, *battle_mg_failed);
            last_state = st; last_mg = mg; last_gp = gp;
            memcpy(last_btn, (const void *)battle_mg_buttons, 12);
            last_ok = *battle_mg_success; last_fail = *battle_mg_failed;
            battle_log_count++;
        }
        // Controller state bytes that changed since the last call
        char diff[400];
        int n = 0;
        for (int i = VG_FIRST; i < VG_LAST && n < (int)sizeof(diff) - 16; i++)
            if (pad[i] != last[i])
                n += snprintf(diff + n, sizeof(diff) - n, " %d:%d", i, pad[i]);
        if (n) {
            SceCtrlData ctrl;
            sceCtrlPeekBufferPositive(0, &ctrl, 1);
            filelog_printf("battle: pad%s (vita buttons 0x%05x)\n", diff, (unsigned)ctrl.buttons);
            battle_log_count++;
        }
        memcpy(last + VG_FIRST, pad + VG_FIRST, VG_LAST - VG_FIRST);
    }
    orig_battle_input(pad);
}

/*
 * Battle HUD console frames
 */

typedef struct { void *entry; } Name;
static void *(*name_lookup)(const char *s, unsigned len, int tag);
static void (*snapshot_set_bool)(void *snapshot, const Name *name, int value);
static void (*orig_push_flash_vars)(void *snapshot);
static Name show_pc_buttons;

static void Battle_PushFlashVars_hook(void *snapshot) {
    orig_push_flash_vars(snapshot);
    if (!show_pc_buttons.entry)
        show_pc_buttons.entry = name_lookup("ShowPCButtons", 13, 2);
    snapshot_set_bool(snapshot, &show_pc_buttons, 0);
}

static void patch_battle_hud() {
    uintptr_t fn = so_symbol(&so_mod, "_Z20Battle_PushFlashVarsP18FlashValueSnapshot");
    name_lookup = (void *)so_symbol(&so_mod, "_ZN4Name13_LookupStringEPKcjNS_14ConstructorTagE");
    snapshot_set_bool = (void *)so_symbol(&so_mod, "_ZN18FlashValueSnapshot17SetGlobalVariableERK4Nameb");
    if (!fn || !name_lookup || !snapshot_set_bool) {
        l_error("controls_ui: battle HUD symbols missing");
        return;
    }
    orig_push_flash_vars = (void *)hook_arm_trampoline(&so_mod, fn, (uintptr_t)Battle_PushFlashVars_hook);
    if (!orig_push_flash_vars)
        l_error("controls_ui: can't hook Battle_PushFlashVars");
}

/*
 * Battle minigames: buttons as the touch input the game judges
 */

#define BATTLE_STATE_MINIGAME 6
#define INPUT_NONE  -1
#define INPUT_TAP    0
// Swipe directions as Battle_GetInputType returns them (|dx| or |dy| > 80)
#define SWIPE_LEFT   1
#define SWIPE_RIGHT  2
#define SWIPE_UP     3
#define SWIPE_DOWN   4

typedef struct { float x, y; } Vec2;
static int (*orig_get_input_type)(const uint8_t *pad, Vec2 *pos, float *dist);
static int (*button_overlap)(int prompt, const Vec2 *pos);
static volatile uint32_t *battle_mg_array; // Array<BattlePrompt>: count << 6, capacity, data

static int find_slot(int prompt, Vec2 *out) {
    // The icons are ~100 units wide: a coarse grid finds them; then centre it
    float x0 = 0, x1 = -1, y0 = 0, y1 = -1;
    for (int y = 8; y < 720; y += 16) {
        for (int x = 8; x < 1280; x += 16) {
            Vec2 p = { (float)x, (float)y };
            if (button_overlap(prompt, &p)) {
                if (x1 < 0) { x0 = x1 = x; y0 = y1 = y; }
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                y1 = y;
            }
        }
    }
    if (x1 < 0)
        return 0;
    out->x = (x0 + x1) / 2;
    out->y = (y0 + y1) / 2;
    return 1;
}

static void log_prompts(const char *what) {
    uint32_t count = battle_mg_array[0] >> 6;
    const int32_t *data = (const int32_t *)(uintptr_t)battle_mg_array[2];
    char buf[96];
    int n = 0;
    for (uint32_t i = 0; i < count && i < 8 && data; i++)
        n += snprintf(buf + n, sizeof(buf) - n, " %d", data[i]);
    buf[n] = 0;
    if (battle_log_count++ < BATTLE_LOG_MAX)
        filelog_printf("battle: %s (minigame %d, prompts%s)\n", what, *battle_minigame, buf);
}

static int Battle_GetInputType_hook(const uint8_t *pad, Vec2 *pos, float *dist) {
    static uint32_t last_ctrl;
    SceCtrlData ctrl;
    sceCtrlPeekBufferPositive(0, &ctrl, 1);
    uint32_t pressed = ctrl.buttons & ~last_ctrl;
    last_ctrl = ctrl.buttons;

    int type = orig_get_input_type(pad, pos, dist);
    if (*battle_input_state != BATTLE_STATE_MINIGAME)
        return type;
    if (type != INPUT_NONE) {
        char what[64];
        snprintf(what, sizeof(what), "touch input %d at %.0f,%.0f", type, pos->x, pos->y);
        log_prompts(what);
        return type;
    }

    // Face buttons: the controller's A/B/X/Y "pressed this frame" bytes (after remapping)
    for (int b = 0; b < 4; b++) {
        if (!pad[170 + b])
            continue;
        Vec2 p = { -1000, -1000 }; // off every slot: a miss
        int found = find_slot(b, &p);
        *pos = p;
        *dist = 0;
        char what[64];
        snprintf(what, sizeof(what), "button %c -> tap %s %.0f,%.0f", "ABXY"[b], found ? "on its icon" : "(no icon: miss)", p.x, p.y);
        log_prompts(what);
        return INPUT_TAP;
    }

    // D-pad: swipes
    static const struct { uint32_t button; int swipe; const char *name; } dirs[] = {
        { SCE_CTRL_LEFT, SWIPE_LEFT, "left" }, { SCE_CTRL_RIGHT, SWIPE_RIGHT, "right" },
        { SCE_CTRL_UP, SWIPE_UP, "up" }, { SCE_CTRL_DOWN, SWIPE_DOWN, "down" },
    };
    for (int i = 0; i < 4; i++) {
        if (!(pressed & dirs[i].button))
            continue;
        pos->x = 640;
        pos->y = 360;
        *dist = 200;
        char what[64];
        snprintf(what, sizeof(what), "d-pad %s -> swipe %d", dirs[i].name, dirs[i].swipe);
        log_prompts(what);
        return dirs[i].swipe;
    }
    return type;
}

static void patch_battle_minigames() {
    uintptr_t fn = so_symbol(&so_mod, "_Z19Battle_GetInputTypeRK14VirtualGamepadR4vec2Rf");
    button_overlap = (void *)so_symbol(&so_mod, "_Z23Battle_GetButtonOverlap12BattlePromptRK4vec2");
    battle_mg_array = (void *)so_symbol(&so_mod, "g_BattleMiniGameButtons");
    if (!fn || !button_overlap || !battle_mg_array || !battle_input_state || !battle_minigame) {
        l_error("controls_ui: minigame symbols missing");
        return;
    }
    orig_get_input_type = (void *)hook_arm_trampoline(&so_mod, fn, (uintptr_t)Battle_GetInputType_hook);
    if (!orig_get_input_type)
        l_error("controls_ui: can't hook Battle_GetInputType");
}

static void hook_battle_diagnostics() {
    uintptr_t fn = so_symbol(&so_mod, "_Z18Battle_HandleInputRK14VirtualGamepad");
    battle_input_state = (void *)so_symbol(&so_mod, "g_BattleInputState");
    battle_minigame = (void *)so_symbol(&so_mod, "g_BattleMiniGame");
    battle_mg_buttons = (void *)so_symbol(&so_mod, "g_BattleMiniGameButtons");
    battle_mg_success = (void *)so_symbol(&so_mod, "g_BattleMiniGameSuccess");
    battle_mg_failed = (void *)so_symbol(&so_mod, "g_BattleMiniGameFailed");
    if (!fn || !battle_input_state || !battle_minigame || !battle_mg_buttons || !battle_mg_success || !battle_mg_failed) {
        l_error("controls_ui: battle symbols missing");
        return;
    }
    orig_battle_input = (void *)hook_arm_trampoline(&so_mod, fn, (uintptr_t)Battle_HandleInput_hook);
    if (!orig_battle_input)
        l_error("controls_ui: can't hook Battle_HandleInput");
}

void controls_ui_frame() {
    if (setting_controls_gamepad && is_gamepad)
        *is_gamepad = 1;
}

void controls_ui_patch() {
    hook_flash_log();
    hook_battle_diagnostics();
    if (setting_controls_gamepad) {
        patch_gamepad_flag();
        patch_icons();
        patch_battle_hud();
        patch_battle_minigames();
    }
    filelog_printf("controls_ui: %s layout\n", setting_controls_gamepad ? "PlayStation (gamepad)" : "touch");
}
