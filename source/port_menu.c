/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  port_menu.c
 * @brief Port settings menu, opened with L + R + Start.
 *
 * Runs inside the game's buffer swap: the game thread is held there while the
 * menu is open, so the game is paused. The menu does not use OpenGL at all.
 * Once vitaGL's queued frames are on screen, it draws with the CPU straight
 * into the framebuffer being displayed (over a dimmed copy of the last game
 * frame). The game's next frame replaces it as usual.
 */

#include "port_menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>

#include "vita_pad.h"
#include "utils/filelog.h"
#include "utils/settings.h"

// Linux 6x10 console font, as bundled with vitaGL
#include "../lib/vitagl/source/utils/font_utils.h"

#define SCALE    2
#define CHAR_W   (FONT_WIDTH * SCALE)
#define LINE_H   22

// A8B8G8R8
#define COL_TEXT   0xFFFFFFFF
#define COL_DIM    0xFFA0A0A0
#define COL_TITLE  0xFF40C8FF
#define COL_SEL    0xFF40FFFF
#define COL_SELBG  0xC0503018

static uint32_t *fb;
static int fb_pitch, fb_w, fb_h;
static uint32_t *backdrop;

/*
 * Drawing
 */

static void fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    for (int yy = 0; yy < h; yy++) {
        uint32_t *p = fb + (y + yy) * fb_pitch + x;
        for (int xx = 0; xx < w; xx++)
            p[xx] = color;
    }
}

static void draw_char(int x, int y, unsigned char c, uint32_t color) {
    if (x < 0 || y < 0 || x + CHAR_W > fb_w || y + FONT_HEIGHT * SCALE > fb_h)
        return;
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = font[c * FONT_HEIGHT + row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (!((bits >> (7 - col)) & 1))
                continue;
            uint32_t *p = fb + (y + row * SCALE) * fb_pitch + x + col * SCALE;
            for (int sy = 0; sy < SCALE; sy++)
                for (int sx = 0; sx < SCALE; sx++)
                    p[sy * fb_pitch + sx] = color;
        }
    }
}

static void draw_text(int x, int y, uint32_t color, const char *text) {
    for (; *text; text++, x += CHAR_W)
        draw_char(x, y, (unsigned char)*text, color);
}

/*
 * Menu model
 */

typedef enum { ITEM_FPS, ITEM_MSAA, ITEM_PROFILER, ITEM_HEADER, ITEM_MAP, ITEM_RESET, ITEM_CLOSE } ItemType;

typedef struct {
    ItemType type;
    int map; // ITEM_MAP: mapping index
} Item;

static Item items[32];
static int item_count;

static const int fps_values[] = { 60, 45, 30, 0 };
static const int msaa_values[] = { 0, 2, 4 };

// Game controller input -> what it does in Costume Quest
static const struct { const char *key, *label; } map_labels[] = {
    { "a",     "Cross (confirm / jump)" },
    { "b",     "Circle (back)" },
    { "x",     "Square" },
    { "y",     "Triangle" },
    { "back",  "Select" },
    { "start", "Start (pause)" },
    { "lb",    "L1" },
    { "rb",    "R1" },
    { "lt",    "L2" },
    { "rt",    "R2" },
    { "l3",    "L3" },
    { "r3",    "R3" },
    { "guide", "PS" },
};

static const struct { const char *name, *label; } source_labels[] = {
    { "none",     "None" },
    { "cross",    "Cross" },
    { "circle",   "Circle" },
    { "square",   "Square" },
    { "triangle", "Triangle" },
    { "start",    "Start" },
    { "select",   "Select" },
    { "l",        "L" },
    { "r",        "R" },
    { "up",       "Up" },
    { "down",     "Down" },
    { "left",     "Left" },
    { "right",    "Right" },
    { "rear_tl",  "Rear upper left" },
    { "rear_tr",  "Rear upper right" },
    { "rear_bl",  "Rear lower left" },
    { "rear_br",  "Rear lower right" },
};

static const char *map_label(int i) {
    const char *key = vita_pad_map_key(i);
    for (int k = 0; k < sizeof(map_labels) / sizeof(*map_labels); k++)
        if (strcmp(map_labels[k].key, key) == 0)
            return map_labels[k].label;
    return key;
}

static const char *source_label(int src) {
    const char *name = vita_pad_source_name(src);
    for (int k = 0; k < sizeof(source_labels) / sizeof(*source_labels); k++)
        if (strcmp(source_labels[k].name, name) == 0)
            return source_labels[k].label;
    return name;
}

static void build_items() {
    item_count = 0;
    items[item_count++] = (Item){ ITEM_FPS };
    items[item_count++] = (Item){ ITEM_MSAA };
    items[item_count++] = (Item){ ITEM_PROFILER };
    items[item_count++] = (Item){ ITEM_HEADER };
    for (int i = 0; i < vita_pad_map_count() && item_count < 29; i++)
        items[item_count++] = (Item){ ITEM_MAP, i };
    items[item_count++] = (Item){ ITEM_RESET };
    items[item_count++] = (Item){ ITEM_CLOSE };
}

static int cycle(const int *values, int count, int current, int dir) {
    int idx = 0;
    for (int i = 0; i < count; i++)
        if (values[i] == current)
            idx = i;
    return values[(idx + dir + count) % count];
}

static int msaa_at_boot;

static void change(Item *it, int dir) {
    switch (it->type) {
        case ITEM_FPS:
            setting_fps_cap = cycle(fps_values, 4, setting_fps_cap, dir);
            break;
        case ITEM_MSAA:
            setting_msaa = cycle(msaa_values, 3, setting_msaa, dir);
            break;
        case ITEM_PROFILER:
            setting_profiler = !setting_profiler;
            break;
        case ITEM_MAP: {
            int n = vita_pad_source_count();
            vita_pad_map_set(it->map, (vita_pad_map_get(it->map) + dir + n) % n);
            break;
        }
        default:
            break;
    }
}

static void render(int sel) {
    char value[64];
    memcpy(fb, backdrop, fb_pitch * fb_h * 4);

    int x = 120, w = fb_w - 240;
    int top = 14;
    fill_rect(x - 16, top - 8, w + 32, fb_h - 2 * (top - 8), 0xD0101010);

    draw_text(x, top, COL_TITLE, "COSTUME QUEST - PORT MENU");
    int y = top + LINE_H + 8;

    for (int i = 0; i < item_count; i++) {
        Item *it = &items[i];
        const char *label = "";
        value[0] = 0;
        switch (it->type) {
            case ITEM_FPS:
                label = "FPS limit";
                if (setting_fps_cap)
                    snprintf(value, sizeof(value), "< %d >", setting_fps_cap);
                else
                    snprintf(value, sizeof(value), "< Unlimited >");
                break;
            case ITEM_MSAA:
                label = "Antialiasing (MSAA)";
                snprintf(value, sizeof(value), "< %s >%s",
                         setting_msaa == 0 ? "Off" : setting_msaa == 2 ? "2x" : "4x",
                         setting_msaa != msaa_at_boot ? " *" : "");
                break;
            case ITEM_PROFILER:
                label = "Profiler (debug.log)";
                snprintf(value, sizeof(value), "< %s >", setting_profiler ? "On" : "Off");
                break;
            case ITEM_HEADER:
                draw_text(x, y + 4, COL_TITLE, "Controls (game input <- Vita button)");
                y += LINE_H + 4;
                continue;
            case ITEM_MAP:
                label = map_label(it->map);
                snprintf(value, sizeof(value), "< %s >", source_label(vita_pad_map_get(it->map)));
                break;
            case ITEM_RESET:
                label = "Restore default controls";
                break;
            case ITEM_CLOSE:
                label = "Save and resume";
                break;
        }
        if (i == sel)
            fill_rect(x - 8, y - 2, w + 16, LINE_H - 2, COL_SELBG);
        uint32_t col = i == sel ? COL_SEL : COL_TEXT;
        draw_text(x, y, col, label);
        if (value[0])
            draw_text(x + 30 * CHAR_W, y, col, value);
        y += LINE_H;
    }

    const char *hint = setting_msaa != msaa_at_boot
        ? "* Restart the game to apply MSAA changes"
        : "Up/Down: select  Left/Right: change  X: OK  O: back";
    draw_text(x, fb_h - top - LINE_H, COL_DIM, hint);
}

/*
 * Entry points
 */

int port_menu_check() {
    SceCtrlData pad;
    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 1)
        return 0;
    uint32_t combo = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START;
    return (pad.buttons & combo) == combo;
}

static uint32_t read_buttons() {
    SceCtrlData pad;
    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 1)
        return 0;
    return pad.buttons;
}

static void wait_release() {
    while (read_buttons())
        sceDisplayWaitVblankStart();
}

void port_menu_run() {
    static int first = 1;
    if (first) {
        msaa_at_boot = setting_msaa;
        first = 0;
    }

    // Let every queued frame reach the screen, then take the one shown
    sceGxmDisplayQueueFinish();
    SceDisplayFrameBuf dfb;
    memset(&dfb, 0, sizeof(dfb));
    dfb.size = sizeof(dfb);
    if (sceDisplayGetFrameBuf(&dfb, SCE_DISPLAY_SETBUF_NEXTFRAME) < 0 || !dfb.base) {
        filelog_printf("port menu: no framebuffer");
        wait_release();
        return;
    }
    fb = dfb.base;
    fb_pitch = dfb.pitch;
    fb_w = dfb.width;
    fb_h = dfb.height;

    backdrop = malloc(fb_pitch * fb_h * 4);
    if (!backdrop) {
        filelog_printf("port menu: out of memory");
        wait_release();
        return;
    }
    // Dimmed copy of the game frame
    for (int i = 0; i < fb_pitch * fb_h; i++)
        backdrop[i] = (fb[i] >> 2) & 0x3F3F3F3F;

    filelog_printf("port menu: opened");
    build_items();
    int sel = 0;
    while (items[sel].type == ITEM_HEADER)
        sel++;

    wait_release();
    uint32_t old = 0;
    int repeat = 0;
    int done = 0;
    render(sel);

    while (!done) {
        sceDisplayWaitVblankStart();
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);

        uint32_t now = read_buttons();
        uint32_t pressed = now & ~old;
        // Auto-repeat for held directions
        uint32_t dirs = now & (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT);
        if (dirs && dirs == (old & dirs)) {
            if (++repeat > 24 && repeat % 6 == 0)
                pressed |= dirs;
        } else {
            repeat = 0;
        }
        old = now;
        if (!pressed)
            continue;

        if (pressed & SCE_CTRL_UP) {
            do sel = (sel + item_count - 1) % item_count; while (items[sel].type == ITEM_HEADER);
        }
        if (pressed & SCE_CTRL_DOWN) {
            do sel = (sel + 1) % item_count; while (items[sel].type == ITEM_HEADER);
        }
        if (pressed & SCE_CTRL_LEFT)
            change(&items[sel], -1);
        if (pressed & (SCE_CTRL_RIGHT | SCE_CTRL_CROSS)) {
            if (items[sel].type == ITEM_RESET)
                vita_pad_reset_mapping();
            else if (items[sel].type == ITEM_CLOSE && (pressed & SCE_CTRL_CROSS))
                done = 1;
            else
                change(&items[sel], +1);
        }
        if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_START))
            done = 1;
        render(sel);
    }

    settings_save();
    filelog_printf("port menu: closed (fps_cap %d, msaa %d)", setting_fps_cap, setting_msaa);

    // Leave the game the screen it drew; the next frame replaces it anyway
    for (int i = 0; i < fb_pitch * fb_h; i++)
        fb[i] = (backdrop[i] << 2) | 0xFF000000;
    free(backdrop);
    backdrop = NULL;
    wait_release();
}
