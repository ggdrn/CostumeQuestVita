/* Host regression checks: cc -Isource tests/controls_compat_test.c
   source/controls_compat.c -o /tmp/cq-controls-test && /tmp/cq-controls-test */
#include "controls_compat.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void texture(const char *path, const char *button) {
    char buf[260], expected[260];
    snprintf(expected, sizeof(expected), "UI/Flash/PS3_ControllerIcons/%s.dds", button);
    assert(strcmp(controls_ps3_texture(path, buf, sizeof(buf)), expected) == 0);
    char tiny[8];
    assert(controls_ps3_texture(path, tiny, sizeof(tiny)) == path);
}

int main(void) {
    texture("UI/Flash/HUDInteractions/HUDInteractions_I12.dds", "BUTTON_A");
    texture("UI/Flash/HUDInteractions_DLC/HUDInteractions_DLC_IF.dds", "BUTTON_A");
    texture("UI/Flash/MessageBox/MessageBox_IA.dds", "BUTTON_A");
    texture("ui/flash/messagebox/messagebox_i7.dds", "BUTTON_B");
    texture("UI/Flash/Notebook/notebook_IE7.dds", "BUTTON_StickRight");
    texture("UI/Flash/Notebook_DLC/Notebook_DLC_IEF.dds", "BUTTON_StickRight");
    texture("UI/Flash/Mobile_InputIcons/PS3__BUTTON_X.dds", "BUTTON_X");
    texture("UI/Flash/Xbox360_ControllerIcons/BUTTON_Aorig.dds", "BUTTON_A");
    texture("UI/Flash/Xbox360_ControllerIcons/BUTTON_ShoulderLeft.dds", "BUTTON_ShoulderLeft");
    texture("UI/Flash/PC_Keys/BUTTON_A.dds", "BUTTON_A");
    texture("UI/Flash/PC_Keys/BUTTON_TriggerLeft.dds", "BUTTON_TriggerLeft");
    const char *key = "UI/Flash/PC_Keys/BUTTON_W.dds";
    char keyboard_buf[260];
    assert(controls_ps3_texture(key, keyboard_buf, sizeof(keyboard_buf)) == key);
    const char *speech = "UI/Flash/HUDInteractions/speech.dds";
    char buf[260];
    assert(controls_ps3_texture(speech, buf, sizeof(buf)) == speech);

    uint8_t pad[724] = {0};
    int latch = 0;
    /* Holding the flee chord opens one modal. While it is open, the
       battle handler must not see the held chord or touches on Yes/No. */
    pad[278] = pad[279] = 1;
    assert(!controls_battle_block(0, pad, &latch));
    for (int frame = 0; frame < 120; frame++)
        assert(controls_battle_block(1, pad, &latch));
    pad[184] = 1; // Touching No closes the native dialog.
    assert(controls_battle_block(1, pad, &latch));
    assert(controls_battle_block(0, pad, &latch)); // Held chord cannot reopen it.
    pad[278] = pad[279] = pad[184] = 0;
    assert(!controls_battle_block(0, pad, &latch));
    /* Touch-only opening and cancelling also block combat underneath. */
    assert(controls_battle_block(1, pad, &latch));
    assert(!controls_battle_block(0, pad, &latch));
    pad[297] = 1;
    assert(controls_battle_block(1, pad, &latch));
    assert(controls_battle_block(0, pad, &latch));
    pad[297] = 0;
    assert(!controls_battle_block(0, pad, &latch));

    assert(!controls_modal_result(4, pad));
    pad[270] = 1; // Held A, without a new press, must not repeat.
    assert(!controls_modal_result(4, pad));
    pad[170] = 1;
    assert(controls_modal_result(4, pad) == 1);
    pad[171] = 1;
    assert(controls_modal_result(4, pad) == 2);
    pad[170] = 0;
    assert(controls_modal_result(4, pad) == 2);
    for (int type = 0; type < 4; type++)
        assert(!controls_modal_result(type, pad));
    puts("PlayStation prompts and flee modal regression checks passed");
}
