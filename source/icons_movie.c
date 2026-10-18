/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  icons_movie.c
 * @brief Builds the controller icon library the HUDs import.
 *
 * Every HUD imports its button icons from "Shared/ControllerIcons.swf". The
 * Android battle HUD (BattleHUD_DLC) takes both the controller buttons
 * (BUTTON_A, BUTTON_DPadLeft, ...) and the touch symbols (SPECIAL_Ninja,
 * MINIGAME_Bat, BUTTON_SwipeLeft, ...) from it, but on Android that URL is
 * Mobile_InputIcons, which only has the touch symbols.
 *
 * This writes a movie in the same layout as the game's icon libraries that
 * exports both sets. It holds no artwork: only external image references to
 * textures already in the game data. Touch symbols resolve to
 * UI/Flash/Mobile_InputIcons/<name>; the PlayStation buttons are named
 * "PS3__<name>" and redirected to UI/Flash/PS3_ControllerIcons/<name> when
 * the texture is bound (controls_ui.c).
 *
 * Only depends on zlib, so it can be checked on the host against the
 * originals.
 */

#include "icons_movie.h"

#include <stdio.h>
#include <string.h>
#include <zlib.h>

typedef struct {
    const char *name;
    uint16_t size;
    uint8_t ps3;
} Icon;

// Same order as in the original libraries
static const Icon icons[] = {
    // Mobile_InputIcons
    { "BUTTON_Tap", 64 }, { "BUTTON_SwipeUp", 64 }, { "BUTTON_SwipeRight", 64 },
    { "BUTTON_SwipeLeft", 64 }, { "BUTTON_SwipeDown", 64 },
    { "STAMP_ToiletPaper", 64 }, { "STAMP_ScreamingSpider", 64 }, { "STAMP_RottenEgg", 64 },
    { "STAMP_Generic", 64 }, { "STAMP_Egg", 64 }, { "STAMP_DeviledEgg", 64 },
    { "STAMP_BlackWidow", 64 }, { "STAMP_TwoPly", 64 },
    { "SPECIAL_Yeti", 64 }, { "SPECIAL_Vampire", 64 }, { "SPECIAL_Unicorn", 64 },
    { "SPECIAL_SpaceMan", 64 }, { "SPECIAL_Robot", 64 }, { "SPECIAL_Pumpkin", 64 },
    { "SPECIAL_Pirate", 64 }, { "SPECIAL_Ninja", 64 }, { "SPECIAL_Liberty", 64 },
    { "SPECIAL_Knight", 64 }, { "SPECIAL_FrenchFries", 64 }, { "SPECIAL_EyeBall", 64 },
    { "SPECIAL_BlackCat", 64 },
    { "BUTTON_RunAway", 64 }, { "BUTTON_Notebook", 64 },
    { "MINIGAME_Spider", 64 }, { "MINIGAME_Pumpkin", 64 }, { "MINIGAME_Ghost", 64 },
    { "MINIGAME_Bat", 64 }, { "BUTTON_Attack", 64 }, { "BUTTON_Arrow", 128 },
    // PS3_ControllerIcons
    { "BUTTON_A", 32, 1 }, { "BUTTON_Y", 32, 1 }, { "BUTTON_Start", 32, 1 }, { "BUTTON_X", 32, 1 },
    { "BUTTON_Back", 32, 1 }, { "BUTTON_TriggerRight", 32, 1 }, { "BUTTON_ShoulderRight", 32, 1 },
    { "BUTTON_ClickRight", 32, 1 }, { "BUTTON_StickRight", 32, 1 }, { "BUTTON_B", 32, 1 },
    { "BUTTON_TriggerLeft", 32, 1 }, { "BUTTON_ShoulderLeft", 32, 1 }, { "BUTTON_ClickLeft", 32, 1 },
    { "BUTTON_StickLeft", 32, 1 }, { "BUTTON_DPadUp", 32, 1 }, { "BUTTON_DPadRight", 32, 1 },
    { "BUTTON_DPadLeft", 32, 1 }, { "BUTTON_DPadDown", 32, 1 }, { "BUTTON_DPad", 32, 1 },
};

typedef struct {
    uint8_t *buf;
    size_t len, cap;
    int overflow;
} Writer;

static void put8(Writer *w, uint8_t v) {
    if (w->len < w->cap)
        w->buf[w->len] = v;
    else
        w->overflow = 1;
    w->len++;
}
static void put16(Writer *w, uint16_t v) { put8(w, v & 0xFF); put8(w, v >> 8); }
static void put32(Writer *w, uint32_t v) { put16(w, v & 0xFFFF); put16(w, v >> 16); }
static void putbytes(Writer *w, const void *p, size_t n) { for (size_t i = 0; i < n; i++) put8(w, ((const uint8_t *)p)[i]); }
static void putstr8(Writer *w, const char *s) { put8(w, strlen(s)); putbytes(w, s, strlen(s)); }  // u8 length + bytes
static void putcstr(Writer *w, const char *s) { putbytes(w, s, strlen(s) + 1); }

// Short record header when it fits, like the game's files
static void tag(Writer *w, uint16_t code, const uint8_t *body, size_t len) {
    if (len < 0x3F) {
        put16(w, (code << 6) | len);
    } else {
        put16(w, (code << 6) | 0x3F);
        put32(w, len);
    }
    putbytes(w, body, len);
}

size_t icons_movie_build(uint8_t *out, size_t cap) {
    static uint8_t raw[16384];
    uint8_t body[256];
    Writer w = { raw, 0, sizeof(raw), 0 };

    // SWF 8 header (signature and length are written last), 960x544, 12fps, 1 frame
    static const uint8_t frame_rect[] = { 0x80, 0x00, 0x03, 0x20, 0x00, 0x00, 0x01, 0xC2, 0x00 };
    putbytes(&w, "CFX", 3);
    put8(&w, 8);
    put32(&w, 0);
    putbytes(&w, frame_rect, sizeof(frame_rect));
    put16(&w, 12 << 8);
    put16(&w, 1);

    // ExporterInfo: version 0x020F, flags 0, bitmap format 14, no prefix, movie name
    Writer b = { body, 0, sizeof(body), 0 };
    put16(&b, 0x020F); put32(&b, 0); put16(&b, 0x0E); put8(&b, 0);
    putstr8(&b, "Mobile_InputIcons");
    tag(&w, 1000, body, b.len);

    static const uint8_t file_attributes[4] = { 0 };
    tag(&w, 69, file_attributes, sizeof(file_attributes));
    static const uint8_t background[3] = { 0x33, 0x33, 0x33 };
    tag(&w, 9, background, sizeof(background));

    char file[64];
    for (size_t i = 0; i < sizeof(icons) / sizeof(*icons); i++) {
        uint16_t id = i + 1;
        snprintf(file, sizeof(file), "%s%s.dds", icons[i].ps3 ? ICONS_PS3_PREFIX : "", icons[i].name);

        // DefineExternalImage2: id, format, width, height, export name, file name
        b = (Writer){ body, 0, sizeof(body), 0 };
        put16(&b, id); put16(&b, 0x0E); put16(&b, icons[i].size); put16(&b, icons[i].size);
        putstr8(&b, icons[i].name);
        putstr8(&b, file);
        tag(&w, 1001, body, b.len);

        // ExportAssets: one symbol
        b = (Writer){ body, 0, sizeof(body), 0 };
        put16(&b, 1); put16(&b, id); putcstr(&b, icons[i].name);
        tag(&w, 56, body, b.len);
    }
    tag(&w, 1, NULL, 0); // ShowFrame
    tag(&w, 0, NULL, 0); // End
    if (w.overflow)
        return 0;

    // Uncompressed length, then zlib after the 8-byte header ("CFX")
    uint32_t total = w.len;
    memcpy(raw + 4, &total, 4);
    if (cap < 8)
        return 0;
    memcpy(out, raw, 8);
    uLongf zlen = cap - 8;
    if (compress2(out + 8, &zlen, raw + 8, total - 8, Z_BEST_COMPRESSION) != Z_OK)
        return 0;
    return 8 + zlen;
}

int icons_movie_count() {
    return sizeof(icons) / sizeof(*icons);
}
