/* MIT license; see LICENSE. */
#include "controls_compat.h"
#include "icons_movie.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

const char *controls_ps3_texture(const char *path, char *out, size_t size) {
    static const struct { const char *path, *button; } embedded[] = {
        { "UI/Flash/HUDInteractions/HUDInteractions_I12.dds", "BUTTON_A" },
        { "UI/Flash/HUDInteractions_DLC/HUDInteractions_DLC_IF.dds", "BUTTON_A" },
        { "UI/Flash/MessageBox/MessageBox_IA.dds", "BUTTON_A" },
        { "UI/Flash/MessageBox/MessageBox_I7.dds", "BUTTON_B" },
        { "UI/Flash/Notebook/notebook_IE7.dds", "BUTTON_StickRight" },
        { "UI/Flash/Notebook_DLC/Notebook_DLC_IEF.dds", "BUTTON_StickRight" },
    };
    // CostumeChooser imports PC_Keys, including console-shaped A/B/LT icons.
    static const char *console_buttons[] = {
        "BUTTON_A", "BUTTON_B", "BUTTON_X", "BUTTON_Y", "BUTTON_Start", "BUTTON_Back",
        "BUTTON_TriggerLeft", "BUTTON_TriggerRight", "BUTTON_ShoulderLeft", "BUTTON_ShoulderRight",
        "BUTTON_StickLeft", "BUTTON_StickRight", "BUTTON_ClickLeft", "BUTTON_ClickRight",
        "BUTTON_DPad", "BUTTON_DPadUp", "BUTTON_DPadDown", "BUTTON_DPadLeft", "BUTTON_DPadRight",
    };
    const char *button = NULL;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (strncmp(base, ICONS_PS3_PREFIX, sizeof(ICONS_PS3_PREFIX) - 1) == 0)
        button = base + sizeof(ICONS_PS3_PREFIX) - 1;
    else if (strncasecmp(path, "UI/Flash/Xbox360_ControllerIcons/", sizeof("UI/Flash/Xbox360_ControllerIcons/") - 1) == 0 &&
             strncasecmp(base, "BUTTON_", 7) == 0)
        button = strcasecmp(base, "BUTTON_Aorig.dds") == 0 ? "BUTTON_A.dds" : base;
    if (!button && strncasecmp(path, "UI/Flash/PC_Keys/", sizeof("UI/Flash/PC_Keys/") - 1) == 0) {
        for (size_t i = 0; i < sizeof(console_buttons) / sizeof(*console_buttons); i++) {
            size_t n = strlen(console_buttons[i]);
            if (strncasecmp(base, console_buttons[i], n) == 0 && strcasecmp(base + n, ".dds") == 0) {
                button = base;
                break;
            }
        }
    }
    if (button) {
        int n = snprintf(out, size, "UI/Flash/PS3_ControllerIcons/%s", button);
        return n >= 0 && (size_t)n < size ? out : path;
    }
    for (size_t i = 0; i < sizeof(embedded) / sizeof(*embedded); i++) {
        if (strcasecmp(path, embedded[i].path) != 0)
            continue;
        int n = snprintf(out, size, "UI/Flash/PS3_ControllerIcons/%s.dds", embedded[i].button);
        return n >= 0 && (size_t)n < size ? out : path;
    }
    return path;
}

int controls_modal_result(int type, const uint8_t *pad) {
    /* MessageBox.as only handles types 1/3 for A and 2/3 for B. The battle
       asks for type 4 (Yes/No), so neither console handler closes it. */
    if (type != 4)
        return 0;
    if (pad[171]) return 2; // Cancel wins if both are pressed.
    if (pad[170]) return 1;
    return 0;
}

int controls_battle_block(int modal_active, const uint8_t *pad, int *wait_release) {
    /* Native flee checks the HELD LT+RT chord or button 297 each frame.
       Without this guard it keeps pushing dialogs onto tModalDialog. */
    int flee_held = (pad[278] && pad[279]) || pad[297];
    if (!flee_held)
        *wait_release = 0;
    if (modal_active) {
        *wait_release = flee_held;
        return 1;
    }
    return *wait_release;
}
