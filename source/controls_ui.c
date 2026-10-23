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
 *   Timed and mash prompts use press edges; the movement minigame uses the left
 *   stick as a local cursor, without feeding Android touch events.
 */

#include "controls_ui.h"
#include "controls_compat.h"
#include "controls_gfx.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
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
static const struct { uint32_t offset, expected; } rpi_clear_gamepad[] = {
    { 0x6e67c4, 0xe5c02000 }, { 0x6e691c, 0xe5c40000 }, { 0x6e6a20, 0x15ca4000 }
};
static volatile uint8_t *is_gamepad;

static void patch_gamepad_flag() {
    is_gamepad = (uint8_t *)so_symbol(&so_mod, "_ZN21VirtualGamepadManager12m_bIsGamepadE");
    uintptr_t func = so_symbol(&so_mod, "_ZN21VirtualGamepadManager19_ReadPhysicalInputsERN14VirtualGamepad5StateERNS_7VGEntryEbj");
    if (!is_gamepad || !func) {
        l_error("controls_ui: VirtualGamepadManager symbols missing");
        return;
    }
    const uint32_t nop = 0xE320F000;
    // Validate the whole patch before writing any instruction.
    for (unsigned i = 0; i < sizeof(rpi_clear_gamepad) / sizeof(*rpi_clear_gamepad); i++) {
        uint32_t *insn = (uint32_t *)(func + rpi_clear_gamepad[i].offset - RPI_FUNC);
        if (*insn != rpi_clear_gamepad[i].expected) {
            l_error("controls_ui: unsupported _ReadPhysicalInputs instruction at +0x%x",
                    rpi_clear_gamepad[i].offset - RPI_FUNC);
            return;
        }
    }
    for (unsigned i = 0; i < sizeof(rpi_clear_gamepad) / sizeof(*rpi_clear_gamepad); i++) {
        void *insn = (void *)(func + rpi_clear_gamepad[i].offset - RPI_FUNC);
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

/* Install compressed movie overrides before any Flash movie is loaded.
   The APK and its original data are never changed on disk. */
static void patch_ui_movies(void) {
    static const char *entries[] = {
        "Data/UI/HUDInteractions/Opt/HUDInteractions.gfx",
        "Data/UI/HUDInteractions_DLC/Opt/HUDInteractions_DLC.gfx",
        "Data/UI/Notebook_DLC/Opt/Notebook_DLC.gfx",
    };
    FILE *apk = fopen(APK_PATH, "rb");
    if (!apk) { l_error("controls_ui: can't open APK for movie patches"); return; }
    for (unsigned kind = 0; kind < sizeof(entries) / sizeof(*entries); kind++) {
        uint32_t offset, size;
        if (apk_index_find(APK_PATH, entries[kind], &offset, &size) || !size)
            continue;
        uint8_t *original = malloc(size), *patched = malloc(size);
        int ok = original && patched && fseek(apk, offset, SEEK_SET) == 0 &&
                 fread(original, 1, size, apk) == size &&
                 controls_gfx_patch(kind, original, size, patched, size) &&
                 fdcache_set_apk_overlay(offset, patched, size) == 0;
        free(original);
        if (ok)
            filelog_printf("controls_ui: %s -> %s\n", entries[kind],
                           kind == CQ_GFX_NOTEBOOK_DLC ? "scroll v2 (native update + drag-aware target)" : "full-size PS3 interaction button");
        else {
            free(patched);
            l_error("controls_ui: unsupported movie or failed override: %s", entries[kind]);
        }
        // The successful overlay owns patched for the lifetime of the game.
    }
    fclose(apk);
}

// RsRef<Texture> RsRef<Texture>::BindFile(const char *path, bool)
#define TEX_BINDFILE            0x91aa0c
#define TEX_DESCRIPTOR_GOT_SLOT 0xc44b04

typedef void *(*RsBindFile)(const void *descriptor, const char *path, int flag);
static RsBindFile rs_bind_file;
static const void **tex_descriptor_slot;

static void *TextureBindFile_hook(const char *path, int flag) {
    char buf[260];
    const char *resolved = controls_ps3_texture(path, buf, sizeof(buf));
    if (resolved != path && icon_redirects++ < 16)
        filelog_printf("controls_ui: texture %s -> %s\n", path, resolved);
    path = resolved;
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
 * Type-4 (Yes/No) modal input. Let the native Close pop the dialog and run
 * its callback; the next FlashFrontEnd::Update unloads the movie and restores
 * exclusive input. Never unload a movie from its own Flash input callback.
 */
static void *modal_dialog;
static void **flash_frontend;
static int (*modal_active)(const void *dialog);
static int (*modal_type)(const void *dialog);
static void (*modal_close)(void *dialog, int result);
static void (*orig_flash_controller_input)(void *manager, void *movie);

static int is_modal_active(void) {
    return modal_dialog && modal_active && modal_active(modal_dialog);
}

static void FlashControllerInput_hook(void *manager, void *movie) {
    void *frontend = flash_frontend ? *flash_frontend : NULL;
    if (setting_controls_gamepad && frontend && is_modal_active() &&
        movie == *(void **)((uint8_t *)frontend + 12) && modal_type(modal_dialog) == 4) {
        /* FlashMovie's first word is PlayType, not a player index. The
           modal uses frontend PlayType=1, whose VirtualGamepad starts at
           FlashManager+2924+724 (HandleControllerInput/Update). */
        const uint8_t *pad = (const uint8_t *)manager + 2924 + 724;
        int result = controls_modal_result(4, pad);
        if (result) {
            filelog_printf("controls_ui: Yes/No modal -> %s\n", result == 1 ? "yes" : "no");
            modal_close(modal_dialog, result);
        }
        return; // Do not also deliver the same press to ActionScript.
    }
    orig_flash_controller_input(manager, movie);
}

static void patch_modal_input(void) {
    modal_dialog = (void *)so_symbol(&so_mod, "tModalDialog");
    modal_active = (void *)so_symbol(&so_mod, "_ZNK11ModalDialog9GetActiveEv");
    modal_type = (void *)so_symbol(&so_mod, "_ZNK11ModalDialog7GetTypeEv");
    modal_close = (void *)so_symbol(&so_mod, "_ZN11ModalDialog5CloseE15ModalDialogType");
    flash_frontend = (void *)so_symbol(&so_mod, "g_pFlashFrontEnd");
    uintptr_t fn = so_symbol(&so_mod, "_ZN12FlashManager21HandleControllerInputEP10FlashMovie");
    if (!modal_dialog || !modal_active || !modal_type || !modal_close || !flash_frontend || !fn) {
        l_error("controls_ui: modal input symbols missing");
        return;
    }
    orig_flash_controller_input = (void *)hook_arm_trampoline(&so_mod, fn, (uintptr_t)FlashControllerInput_hook);
    if (!orig_flash_controller_input)
        l_error("controls_ui: can't hook modal controller input");
    else
        filelog_printf("controls_ui: Yes/No modal bridge (single dialog)\n");
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

static void battle_console_input(const uint8_t *pad);

static void Battle_HandleInput_hook(const uint8_t *pad) {
    static uint8_t last[VG_LAST];
    static int32_t last_state = -100, last_mg = -100, last_gp = -1;
    static uint8_t last_btn[12], last_ok = 0xFF, last_fail = 0xFF;

    if (setting_profiler && battle_log_count < BATTLE_LOG_MAX) {
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
    static int wait_flee_release;
    if (controls_battle_block(is_modal_active(), pad, &wait_flee_release))
        return;
    battle_console_input(pad);
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
typedef struct { float x0, y0, x1, y1; } HitBox;
// C++ aggregate return uses an explicit output pointer in this ARM ABI.
static void (*get_hit_box)(HitBox *out, int slot);
static void (*orig_mouse_position)(Vec2 *out, const void *rules, int pixels);
static int battle_cursor_active;
static Vec2 battle_cursor;
static Vec2 battle_previous_stick;
static int battle_motion_initialized;
static int battle_motion_touch;
static float battle_previous_timer;
static volatile float *battle_mg_timer;
static int battle_stick_direction = INPUT_NONE;
static int battle_stick_swipe = INPUT_NONE;

static Vec2 battle_left_stick(const uint8_t *pad) {
    Vec2 v;
    memcpy(&v.x, pad + 108, sizeof(float));
    memcpy(&v.y, pad + 112, sizeof(float));
    float length = sqrtf(v.x * v.x + v.y * v.y);
    if (!isfinite(length) || length < 0.20f)
        return (Vec2){0, 0};
    if (length > 1.0f) {
        v.x /= length;
        v.y /= length;
    }
    return v;
}

static void battle_update_stick_direction(Vec2 v) {
    int direction = battle_stick_direction;
    float ax = fabsf(v.x), ay = fabsf(v.y);
    // A held direction never repeats a QTE. Hysteresis suppresses drift
    // around the threshold; the next flick can follow a return to centre.
    if (ax < 0.35f && ay < 0.35f)
        direction = INPUT_NONE;
    else if (ax >= 0.60f || ay >= 0.60f)
        direction = ax > ay ? (v.x < 0 ? SWIPE_LEFT : SWIPE_RIGHT)
                            : (v.y < 0 ? SWIPE_DOWN : SWIPE_UP);
    battle_stick_swipe = direction != battle_stick_direction ? direction : INPUT_NONE;
    battle_stick_direction = direction;
}

static Vec2 battle_motion_cursor(Vec2 v) {
    // The native routine expands normalized coordinates into 1280x720 and
    // corrects Y for the Vita's 960x544 viewport. Feed ONE stick unit, not
    // hundreds of pixels: the gamepad gain is 0.12 (touch gain is 0.0007).
    return (Vec2){0.5f + v.x / 1280.0f,
                  0.5f - v.y * (960.0f / 544.0f) / 1280.0f};
}

static void mouse_position_hook(Vec2 *out, const void *rules, int pixels) {
    if (battle_cursor_active == 2 && !pixels) {
        orig_mouse_position(out, rules, pixels);
        // Real gestures retain the original touch gain while console mode
        // stays active (native handler otherwise applies the 0.12 pad gain).
        const float scale = 0.0007f / 0.12f;
        out->x = 0.5f + (out->x - 0.5f) * scale;
        out->y = 0.5f + (out->y - 0.5f) * scale;
        return;
    }
    if (battle_cursor_active == 1 && !pixels) {
        *out = battle_cursor;
        return;
    }
    orig_mouse_position(out, rules, pixels);
}

static void battle_console_input(const uint8_t *pad) {
    Vec2 stick = battle_left_stick(pad);
    // Sample even outside QTEs: a stick held during target selection must
    // not become a new directional press when the minigame starts.
    battle_update_stick_direction(stick);
    if (setting_controls_gamepad && orig_mouse_position && orig_get_input_type &&
        battle_mg_timer && *battle_input_state == BATTLE_STATE_MINIGAME &&
        *battle_minigame == 20) {
        if (pad[184] || pad[234] || pad[284]) {
            battle_cursor_active = 2;
            if (!battle_motion_touch || pad[184]) {
                uint8_t seed[724];
                Vec2 unused_pos;
                float unused_dist = 0;
                memcpy(seed, pad, sizeof(seed));
                seed[184] = 1; seed[234] = seed[284] = 0;
                orig_get_input_type(seed, &unused_pos, &unused_dist);
            }
            battle_motion_touch = !pad[234];
            battle_motion_initialized = 0;
            orig_battle_input(pad);
            battle_cursor_active = 0;
            return;
        }
        battle_motion_touch = 0;
        uint32_t copy[181]; // VirtualGamepad: 724 bytes (Clear/Update)
        uint8_t *input = (uint8_t *)copy;
        float timer = *battle_mg_timer;
        memcpy(copy, pad, sizeof(copy));
        // _UpdateMinigame subtracts dt: only an increase denotes a restart.
        if (!battle_motion_initialized || timer > battle_previous_timer) {
            // No entry impulse, including a stick already held at QTE start.
            battle_previous_stick = stick;
            battle_motion_initialized = 1;
        }
        battle_previous_timer = timer;
        float dx = stick.x - battle_previous_stick.x;
        float dy = stick.y - battle_previous_stick.y;
        int moved = dx * dx + dy * dy >= 0.0001f;
        input[184] = input[234] = input[284] = 0;
        battle_cursor_active = 1;
        if (moved) {
            // Seed the engine's previous position with its own tap routine.
            // It may have been reset or left at another QTE's screen position.
            // This synthetic tap is never passed to Battle_HandleInput/SDL.
            Vec2 unused_pos;
            float unused_dist = 0;
            battle_cursor = battle_motion_cursor(battle_previous_stick);
            input[184] = 1;
            orig_get_input_type(input, &unused_pos, &unused_dist);
            input[184] = 0;
            input[284] = 1;
            battle_previous_stick = stick;
        }
        battle_cursor = battle_motion_cursor(stick);
        orig_battle_input(input);
        battle_cursor_active = 0;
        return;
    }
    battle_motion_initialized = 0;
    battle_motion_touch = 0;
    orig_battle_input(pad);
}

typedef struct {
    uint32_t count_flags, capacity;
    const int32_t *data;
} BattlePromptArray;
static volatile BattlePromptArray *battle_mg_array;

static int find_slot(int prompt, Vec2 *out) {
    uint32_t count = battle_mg_array->count_flags >> 6;
    const int32_t *data = battle_mg_array->data;
    // The original overlap routine searches the same array for the base
    // A/B/X/Y prompt and passes its index to Battle_GetHitBox (slots 0..3).
    for (uint32_t i = 0; data && i < count && i < 4; i++) {
        if (data[i] != prompt)
            continue;
        HitBox box;
        get_hit_box(&box, i);
        out->x = (box.x0 + box.x1) * 0.5f;
        out->y = (box.y0 + box.y1) * 0.5f;
        return 1;
    }
    return 0;
}

static void log_prompts(const char *what) {
    if (!setting_profiler)
        return;
    uint32_t count = battle_mg_array->count_flags >> 6;
    const int32_t *data = battle_mg_array->data;
    char buf[96];
    int n = 0;
    for (uint32_t i = 0; i < count && i < 8 && data; i++)
        n += snprintf(buf + n, sizeof(buf) - n, " %d", data[i]);
    buf[n] = 0;
    if (battle_log_count++ < BATTLE_LOG_MAX)
        filelog_printf("battle: %s (minigame %d, prompts%s)\n", what, *battle_minigame, buf);
}

static int Battle_GetInputType_hook(const uint8_t *pad, Vec2 *pos, float *dist) {
    if (*battle_input_state != BATTLE_STATE_MINIGAME)
        return orig_get_input_type(pad, pos, dist);
    // Preserve physical touchscreen gestures; mapped buttons win when pressed
    // in the same frame. The synthetic motion cursor remains private.
    if (!setting_controls_gamepad || battle_cursor_active)
        return orig_get_input_type(pad, pos, dist);
    int type = INPUT_NONE;

    // Press, mash and meter prompts all require a fresh mapped press.
    // Holding a meter button must not automatically hit its timing window.
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

    // Use the game's edge state, not a separately sampled physical D-pad.
    // VirtualGamepad BUTTON_DPadUp/Right/Down/Left are indices 12..15.
    static const int swipes[] = { SWIPE_UP, SWIPE_RIGHT, SWIPE_DOWN, SWIPE_LEFT };
    for (int i = 0; i < 4; i++) {
        if (!pad[180 + i])
            continue;
        pos->x = 640;
        pos->y = 360;
        *dist = 200;
        return swipes[i];
    }
    if (battle_stick_swipe != INPUT_NONE) {
        pos->x = 640;
        pos->y = 360;
        *dist = 200;
        return battle_stick_swipe;
    }
    if (pad[184] || pad[234] || pad[284])
        return orig_get_input_type(pad, pos, dist);
    return type;
}

static void patch_battle_minigames() {
    uintptr_t fn = so_symbol(&so_mod, "_Z19Battle_GetInputTypeRK14VirtualGamepadR4vec2Rf");
    get_hit_box = (void *)so_symbol(&so_mod, "_Z16Battle_GetHitBox21BattleButtonPositions");
    battle_mg_array = (void *)so_symbol(&so_mod, "g_BattleMiniGameButtons");
    battle_mg_timer = (void *)so_symbol(&so_mod, "g_BattleMiniGameTimer");
    if (!fn || !get_hit_box || !battle_mg_array || !battle_input_state || !battle_minigame || !battle_mg_timer) {
        l_error("controls_ui: minigame symbols missing");
        return;
    }
    uintptr_t mouse = so_symbol(&so_mod, "_ZNK9SeedRules22GetMouseScreenPositionEb");
    if (mouse)
        orig_mouse_position = (void *)hook_arm_trampoline(&so_mod, mouse, (uintptr_t)mouse_position_hook);
    if (!orig_mouse_position)
        l_error("controls_ui: continuous movement minigame bridge unavailable");
    orig_get_input_type = (void *)hook_arm_trampoline(&so_mod, fn, (uintptr_t)Battle_GetInputType_hook);
    if (!orig_get_input_type)
        l_error("controls_ui: can't hook Battle_GetInputType");
    else
        filelog_printf("controls_ui: motion bridge v2 (countdown timer)\n");
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
    patch_modal_input();
    hook_battle_diagnostics();
    if (setting_controls_gamepad) {
        patch_gamepad_flag();
        patch_icons();
        patch_ui_movies();
        patch_battle_hud();
        patch_battle_minigames();
    }
    filelog_printf("controls_ui: %s layout\n", setting_controls_gamepad ? "PlayStation (gamepad)" : "touch");
}
