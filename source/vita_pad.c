/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  vita_pad.c
 * @brief The Vita controls exposed to the game as an SDL game controller.
 *
 * The game reads gamepads through SDL's GameController API
 * (SDLGamepad::UpdateDeviceState_GameController: 6 axes, 15 buttons). The
 * SDL build in libCq.so only knows the Android accelerometer, so the
 * functions the game uses are replaced: there is one controller, "PS Vita",
 * at device index 0, backed by sceCtrl and the rear touch panel.
 */

#include "vita_pad.h"

#include <stdio.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/touch.h>

#include <so_util/so_util.h>

#include "utils/init.h"
#include "utils/logger.h"

// SDL_GameControllerButton / SDL_GameControllerAxis (SDL 2.0)
enum {
    GC_A, GC_B, GC_X, GC_Y, GC_BACK, GC_GUIDE, GC_START, GC_LEFTSTICK, GC_RIGHTSTICK,
    GC_LEFTSHOULDER, GC_RIGHTSHOULDER, GC_DPAD_UP, GC_DPAD_DOWN, GC_DPAD_LEFT, GC_DPAD_RIGHT,
    GC_BUTTON_MAX
};
enum {
    GC_AXIS_LEFTX, GC_AXIS_LEFTY, GC_AXIS_RIGHTX, GC_AXIS_RIGHTY,
    GC_AXIS_TRIGGERLEFT, GC_AXIS_TRIGGERRIGHT, GC_AXIS_MAX
};

// Physical sources a game controller button can be mapped to
typedef enum {
    SRC_NONE, SRC_CROSS, SRC_CIRCLE, SRC_SQUARE, SRC_TRIANGLE, SRC_START, SRC_SELECT,
    SRC_L, SRC_R, SRC_UP, SRC_DOWN, SRC_LEFT, SRC_RIGHT,
    SRC_REAR_TL, SRC_REAR_TR, SRC_REAR_BL, SRC_REAR_BR,
    SRC_NUM
} PadSource;

static const char *src_names[SRC_NUM] = {
    "none", "cross", "circle", "square", "triangle", "start", "select",
    "l", "r", "up", "down", "left", "right",
    "rear_tl", "rear_tr", "rear_bl", "rear_br",
};

static const uint32_t src_ctrl[SRC_NUM] = {
    0, SCE_CTRL_CROSS, SCE_CTRL_CIRCLE, SCE_CTRL_SQUARE, SCE_CTRL_TRIANGLE, SCE_CTRL_START, SCE_CTRL_SELECT,
    SCE_CTRL_LTRIGGER, SCE_CTRL_RTRIGGER, SCE_CTRL_UP, SCE_CTRL_DOWN, SCE_CTRL_LEFT, SCE_CTRL_RIGHT,
};

// Game controller inputs that can be remapped, as named in config.txt
typedef struct {
    const char *key;
    PadSource src;
} PadMapping;

enum { MAP_A, MAP_B, MAP_X, MAP_Y, MAP_BACK, MAP_START, MAP_LB, MAP_RB, MAP_LT, MAP_RT,
       MAP_L3, MAP_R3, MAP_GUIDE, MAP_NUM };

static PadMapping mapping[MAP_NUM] = {
    [MAP_A]     = { "map_a",     SRC_CROSS },
    [MAP_B]     = { "map_b",     SRC_CIRCLE },
    [MAP_X]     = { "map_x",     SRC_SQUARE },
    [MAP_Y]     = { "map_y",     SRC_TRIANGLE },
    [MAP_BACK]  = { "map_back",  SRC_SELECT },
    [MAP_START] = { "map_start", SRC_START },
    [MAP_LB]    = { "map_lb",    SRC_L },
    [MAP_RB]    = { "map_rb",    SRC_R },
    [MAP_LT]    = { "map_lt",    SRC_REAR_TL },
    [MAP_RT]    = { "map_rt",    SRC_REAR_TR },
    [MAP_L3]    = { "map_l3",    SRC_REAR_BL },
    [MAP_R3]    = { "map_r3",    SRC_REAR_BR },
    [MAP_GUIDE] = { "map_guide", SRC_NONE },
};

// Same as the initial mapping table
static const PadSource default_src[MAP_NUM] = {
    SRC_CROSS, SRC_CIRCLE, SRC_SQUARE, SRC_TRIANGLE, SRC_SELECT, SRC_START,
    SRC_L, SRC_R, SRC_REAR_TL, SRC_REAR_TR, SRC_REAR_BL, SRC_REAR_BR, SRC_NONE,
};

static const int button_to_map[GC_BUTTON_MAX] = {
    [GC_A] = MAP_A, [GC_B] = MAP_B, [GC_X] = MAP_X, [GC_Y] = MAP_Y,
    [GC_BACK] = MAP_BACK, [GC_GUIDE] = MAP_GUIDE, [GC_START] = MAP_START,
    [GC_LEFTSTICK] = MAP_L3, [GC_RIGHTSTICK] = MAP_R3,
    [GC_LEFTSHOULDER] = MAP_LB, [GC_RIGHTSHOULDER] = MAP_RB,
    [GC_DPAD_UP] = -1, [GC_DPAD_DOWN] = -1, [GC_DPAD_LEFT] = -1, [GC_DPAD_RIGHT] = -1,
};

int vita_pad_set_mapping(const char *key, const char *value) {
    for (int i = 0; i < MAP_NUM; i++) {
        if (strcmp(mapping[i].key, key) != 0)
            continue;
        for (int s = 0; s < SRC_NUM; s++) {
            if (strcmp(src_names[s], value) == 0) {
                mapping[i].src = s;
                return 1;
            }
        }
        l_warn("config: unknown source \"%s\" for %s", value, key);
        return 1;
    }
    return 0;
}

void vita_pad_save_mapping(void *file) {
    for (int i = 0; i < MAP_NUM; i++)
        fprintf((FILE *)file, "%s %s\n", mapping[i].key, src_names[mapping[i].src]);
}

int vita_pad_map_count() { return MAP_NUM; }
const char *vita_pad_map_key(int i) { return mapping[i].key + 4; } // without "map_"
int vita_pad_map_get(int i) { return mapping[i].src; }
void vita_pad_map_set(int i, int src) { mapping[i].src = (PadSource)src; }
int vita_pad_source_count() { return SRC_NUM; }
const char *vita_pad_source_name(int src) { return src_names[src]; }

void vita_pad_reset_mapping() {
    for (int i = 0; i < MAP_NUM; i++)
        mapping[i].src = default_src[i];
}

/*
 * State, refreshed at most every 2ms (the game reads every button and axis
 * separately each frame)
 */

static SceCtrlData pad;
static uint8_t rear[4]; // TL, TR, BL, BR quadrants touched
static uint64_t last_read = 0;
static int rear_enabled = 0;

static void read_state() {
    uint64_t now = sceKernelGetProcessTimeWide();
    if (now - last_read < 2000)
        return;
    last_read = now;

    sceCtrlPeekBufferPositiveExt2(0, &pad, 1);

    if (!rear_enabled) {
        sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
        rear_enabled = 1;
    }
    SceTouchData touch;
    memset(rear, 0, sizeof(rear));
    if (sceTouchPeek(SCE_TOUCH_PORT_BACK, &touch, 1) > 0) {
        // Rear panel: 1920 x 890 units
        for (int i = 0; i < touch.reportNum; i++) {
            int right = touch.report[i].x >= 960;
            int bottom = touch.report[i].y >= 445;
            rear[bottom * 2 + right] = 1;
        }
    }
}

static int source_pressed(PadSource src) {
    if (src == SRC_NONE)
        return 0;
    if (src >= SRC_REAR_TL)
        return rear[src - SRC_REAR_TL];
    // L+R+Start opens the port menu: keep that Start away from the game
    if (src == SRC_START && (pad.buttons & SCE_CTRL_LTRIGGER) && (pad.buttons & SCE_CTRL_RTRIGGER))
        return 0;
    return (pad.buttons & src_ctrl[src]) != 0;
}

/*
 * SDL replacements. The controller / joystick handles only need to be
 * distinct non-NULL pointers the game hands back to us; real memory is used
 * in case something reads through them.
 */

static uint8_t fake_controller[256];
static uint8_t fake_joystick[256];
#define PAD_INSTANCE_ID 0

static int NumJoysticks(void) {
    return 1;
}

static int IsGameController(int index) {
    return index == 0;
}

static void *GameControllerOpen(int index) {
    if (index != 0)
        return NULL;
    l_info("SDL_GameControllerOpen(0): PS Vita controls");
    return fake_controller;
}

static void GameControllerClose(void *gc) {}

static int GetAttached(void *handle) {
    return handle == fake_controller || handle == fake_joystick;
}

static void *GameControllerGetJoystick(void *gc) {
    return gc == fake_controller ? fake_joystick : NULL;
}

static int JoystickInstanceID(void *joy) {
    return joy == fake_joystick ? PAD_INSTANCE_ID : -1;
}

static const char *ControllerName(void *handle) {
    return "PS Vita";
}

static void JoystickClose(void *joy) {}

static int16_t stick_axis(uint8_t v) {
    int a = ((int)v - 128) * 256;
    if (a > 32767)
        a = 32767;
    return (int16_t)a;
}

static int16_t GameControllerGetAxis(void *gc, int axis) {
    if (gc != fake_controller)
        return 0;
    read_state();
    switch (axis) {
        case GC_AXIS_LEFTX:        return stick_axis(pad.lx);
        case GC_AXIS_LEFTY:        return stick_axis(pad.ly);
        case GC_AXIS_RIGHTX:       return stick_axis(pad.rx);
        case GC_AXIS_RIGHTY:       return stick_axis(pad.ry);
        case GC_AXIS_TRIGGERLEFT:  return source_pressed(mapping[MAP_LT].src) ? 32767 : 0;
        case GC_AXIS_TRIGGERRIGHT: return source_pressed(mapping[MAP_RT].src) ? 32767 : 0;
        default:                   return 0;
    }
}

static uint8_t GameControllerGetButton(void *gc, int button) {
    if (gc != fake_controller || button < 0 || button >= GC_BUTTON_MAX)
        return 0;
    read_state();
    switch (button) {
        case GC_DPAD_UP:    return (pad.buttons & SCE_CTRL_UP) != 0;
        case GC_DPAD_DOWN:  return (pad.buttons & SCE_CTRL_DOWN) != 0;
        case GC_DPAD_LEFT:  return (pad.buttons & SCE_CTRL_LEFT) != 0;
        case GC_DPAD_RIGHT: return (pad.buttons & SCE_CTRL_RIGHT) != 0;
        default:            return source_pressed(mapping[button_to_map[button]].src);
    }
}

static void hook(const char *symbol, void *dst) {
    uintptr_t addr = so_symbol(&so_mod, symbol);
    if (!addr) {
        l_error("vita_pad: %s not found", symbol);
        return;
    }
    hook_addr(addr, (uintptr_t)dst);
}

void vita_pad_patch() {
    sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);

    hook("SDL_NumJoysticks", NumJoysticks);
    hook("SDL_IsGameController", IsGameController);
    hook("SDL_GameControllerOpen", GameControllerOpen);
    hook("SDL_GameControllerClose", GameControllerClose);
    hook("SDL_GameControllerGetAttached", GetAttached);
    hook("SDL_JoystickGetAttached", GetAttached);
    hook("SDL_GameControllerGetJoystick", GameControllerGetJoystick);
    hook("SDL_JoystickInstanceID", JoystickInstanceID);
    hook("SDL_GameControllerName", ControllerName);
    hook("SDL_JoystickName", ControllerName);
    hook("SDL_JoystickClose", JoystickClose);
    hook("SDL_GameControllerGetAxis", GameControllerGetAxis);
    hook("SDL_GameControllerGetButton", GameControllerGetButton);
}
