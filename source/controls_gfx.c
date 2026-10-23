/* MIT license; see LICENSE. */
#include "controls_gfx.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* Preserve the game's original Quests.update and its method bindings.
   QuestHolder.getTargetY now returns dragging ? target_y : _y. This keeps
   the mobile drag target from undoing scrollbar/gamepad updates at rest.
   The adjacent init keeps its two assignments and omits its trace; the two
   function bodies together retain their original byte count. */
static const uint8_t quest_target_y[] = {
    0x96, 0x04, 0x00, 0x04, 0x01, 0x08, 0x0d, 0x4e, 0x12, 0x9d, 0x02, 0x00, 0x09, 0x00, 0x96, 0x04,
    0x00, 0x04, 0x01, 0x08, 0x14, 0x4e, 0x3e, 0x96, 0x04, 0x00, 0x04, 0x01, 0x08, 0x0b, 0x4e, 0x3e,
};

static uint32_t le32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

size_t controls_gfx_patch(int kind, const void *src, size_t size, void *out, size_t cap) {
    static const struct { uint32_t len, logical_len, crc, offset; } originals[] = {
        { 32776, 29059, 0xd9202aa3, 0x26e4 },
        { 24584, 23514, 0xd52f1215, 0x263b },
        { 102408, 101073, 0x63df6846, 0x127c2 },
    };
    if (kind < 0 || kind > CQ_GFX_NOTEBOOK_DLC || size < 8 || cap < size)
        return 0;
    const uint8_t *in = src;
    if (memcmp(in, "CFX", 3) || le32(in + 4) != originals[kind].logical_len)
        return 0;
    uint32_t len = originals[kind].len;
    uint8_t *raw = malloc(len);
    uint8_t *packed = malloc(size);
    if (!raw || !packed) { free(raw); free(packed); return 0; }
    memcpy(raw, in, 8);
    uLongf raw_len = len - 8;
    int ok = uncompress(raw + 8, &raw_len, in + 8, size - 8) == Z_OK && raw_len == len - 8;
    if (ok)
        ok = crc32(0, raw, len) == originals[kind].crc;
    if (ok) {
        uint32_t offset = originals[kind].offset;
        if (kind == CQ_GFX_NOTEBOOK_DLC) {
            /* 9+50 bytes become 32+27; the enclosing class/tag/jumps
               retain their sizes. Both functions keep their native registers. */
            uint8_t between[19], init[27];
            memcpy(between, raw + 0x112ca, sizeof(between));
            memcpy(init, raw + 0x112dd, 26);
            init[26] = 0; // End: return undefined after the original assignments.
            between[17] = 27; between[18] = 0; // init CodeSize
            raw[0x112bf] = sizeof(quest_target_y); raw[0x112c0] = 0;
            memcpy(raw + 0x112c1, quest_target_y, sizeof(quest_target_y));
            memcpy(raw + 0x112c1 + sizeof(quest_target_y), between, sizeof(between));
            memcpy(raw + 0x112c1 + sizeof(quest_target_y) + sizeof(between), init, sizeof(init));
        } else {
            /* The original A bitmap is 64px; the PS3 replacement is 32px.
               Double its bitmap fill matrix (10 -> 20 twips/pixel) so the
               glyph fills the same bounds, instead of drawing at half size.
               Both MATRIX encodings occupy seven bytes. */
            static const uint8_t matrix[] = { 0xd9, 0x40, 0x00, 0x05, 0x00, 0x00, 0x00 };
            memcpy(raw + offset, matrix, sizeof(matrix));
        }
        memcpy(packed, in, 8);
        uLongf compressed = size - 8;
        ok = compress2(packed + 8, &compressed, raw + 8, originals[kind].logical_len - 8, Z_BEST_COMPRESSION) == Z_OK;
        if (ok) {
            memset(packed + 8 + compressed, 0, size - 8 - compressed);
            memcpy(out, packed, size);
        }
    }
    free(packed);
    free(raw);
    return ok ? size : 0;
}
