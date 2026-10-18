/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  translation.c
 * @brief Fan translations from ux0:data/costumequest/translations/<name>/.
 *
 * The game's text lives in string tables, one per language and part
 * (OGLES/Munged/stringtable/<table>_<lang>.STbl~c in the game data): a text
 * "StringTable{LineCodeData={KEY=LineCodeData{Text=...};...}}" in a "dfcf"
 * container (64-byte header, zlib).
 *
 * With "language <name>" in config.txt (or "auto" and a translation folder
 * present: the first one with costumequest.STbl is used), the tables found in
 * translations/<name>/ (<table>.STbl, plain text, e.g. from
 * extras/scripts/translation_from_pc.py) are compressed into that container
 * at boot and served in place of the French tables (reimpl/fdcache.c), and
 * the game is started in French. Each one must fit in the French table's
 * space in the APK.
 */

#include "translation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>

#include "apk_index.h"
#include "reimpl/fdcache.h"
#include "utils/filelog.h"
#include "utils/settings.h"

#define TRANSLATIONS_DIR DATA_PATH "translations/"
#define HOST_LANG        "frfr"  // the language whose tables carry the translation
#define HOST_GAME_LANG   "frFR"
#define DFCF_HEADER      0x40

static const char *const tables[] = { "costumequest", "costumequestdlc1", "cq1patch1", "cqmobile", "cqpc" };
#define N_TABLES (sizeof(tables) / sizeof(*tables))

static int active;

static uint8_t *read_file(const char *path, size_t *len) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0)
        return NULL;
    SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    uint8_t *buf = size > 0 && size < (16 << 20) ? malloc(size + 1) : NULL;
    if (buf && sceIoRead(fd, buf, size) != size) {
        free(buf);
        buf = NULL;
    }
    sceIoClose(fd);
    if (buf) {
        // The game's tables end with a NUL
        while (size > 0 && buf[size - 1] == 0)
            size--;
        buf[size++] = 0;
        *len = size;
    }
    return buf;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

// dfcf container as the game's compressed string tables have it, zero-padded to size
static uint8_t *build_table(const uint8_t *text, size_t len, uint32_t size, uint32_t *out_len) {
    uint8_t *out = calloc(1, size);
    if (!out || size <= DFCF_HEADER)
        return free(out), NULL;
    uLongf zlen = size - DFCF_HEADER;
    if (compress2(out + DFCF_HEADER, &zlen, text, len, Z_BEST_COMPRESSION) != Z_OK)
        return free(out), NULL; // doesn't fit
    static const uint8_t header[0x16] = {
        'd', 'f', 'c', 'f', 0x00, 0x06, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00,
        0x83, 0xda, 0x4c, 0x0d, 0x45, 0xfd, 0x46, 0xc6, 0x00, 0x00, // STbl type id
    };
    memcpy(out, header, sizeof(header));
    out[0x16] = 0x01; out[0x17] = 0x02; // zlib
    put_be32(out + 0x18, len);
    put_be32(out + 0x1c, zlen);
    put_be32(out + 0x28, len);
    *out_len = DFCF_HEADER + zlen;
    return out;
}

// "auto": the first folder of translations/ holding a main game table
static int find_translation(char *name, size_t size) {
    SceUID d = sceIoDopen(TRANSLATIONS_DIR);
    if (d < 0)
        return 0;
    SceIoDirent ent;
    int found = 0;
    while (!found && sceIoDread(d, &ent) > 0) {
        if (!SCE_S_ISDIR(ent.d_stat.st_mode) || ent.d_name[0] == '.')
            continue;
        char path[320];
        SceIoStat st;
        snprintf(path, sizeof(path), TRANSLATIONS_DIR "%s/costumequest.STbl", ent.d_name);
        if (sceIoGetstat(path, &st) >= 0) {
            snprintf(name, size, "%s", ent.d_name);
            found = 1;
        }
    }
    sceIoDclose(d);
    return found;
}

void translation_init() {
    char name[64];
    if (!setting_language[0] || strcmp(setting_language, "auto") == 0) {
        if (!find_translation(name, sizeof(name))) {
            filelog_printf("translation: none in " TRANSLATIONS_DIR ", using the system language\n");
            return;
        }
        filelog_printf("translation: language auto, found translations/%s\n", name);
    } else {
        snprintf(name, sizeof(name), "%s", setting_language);
    }
    char dir[256];
    snprintf(dir, sizeof(dir), TRANSLATIONS_DIR "%s/", name);
    SceIoStat st;
    if (sceIoGetstat(dir, &st) < 0) {
        filelog_printf("translation: %s not found, using the system language\n", dir);
        return;
    }
    uint64_t t0 = sceKernelGetProcessTimeWide();

    const char *entries[N_TABLES];
    char names[N_TABLES][96];
    uint32_t offsets[N_TABLES], sizes[N_TABLES];
    for (int i = 0; i < N_TABLES; i++) {
        snprintf(names[i], sizeof(names[i]), "OGLES/Munged/stringtable/%s_" HOST_LANG ".STbl~c", tables[i]);
        entries[i] = names[i];
    }
    apk_index_find_many(APK_PATH, N_TABLES, entries, offsets, sizes);

    int served = 0;
    for (int i = 0; i < N_TABLES; i++) {
        char path[320];
        snprintf(path, sizeof(path), "%s%s.STbl", dir, tables[i]);
        size_t len;
        uint8_t *text = read_file(path, &len);
        if (!text)
            continue;
        if (memcmp(text, "StringTable{", 12) != 0) {
            filelog_printf("translation: %s is not a string table\n", path);
            free(text);
            continue;
        }
        if (!sizes[i]) {
            filelog_printf("translation: no %s table in the game data\n", tables[i]);
            free(text);
            continue;
        }
        uint32_t used;
        uint8_t *table = build_table(text, len, sizes[i], &used);
        free(text);
        if (!table) {
            filelog_printf("translation: %s.STbl is too big for the game's table (%u bytes)\n", tables[i], sizes[i]);
            continue;
        }
        if (fdcache_set_apk_overlay(offsets[i], table, sizes[i]) != 0) {
            free(table);
            continue;
        }
        filelog_printf("translation: %s.STbl (%u bytes of text) -> %u of %u bytes\n",
                       tables[i], (unsigned)len, used, sizes[i]);
        served++;
    }
    active = served > 0;
    filelog_printf("translation: \"%s\", %d tables in %u ms%s\n", name, served,
                   (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000),
                   active ? ", game language " HOST_GAME_LANG : "");
}

const char *translation_game_language() {
    return active ? HOST_GAME_LANG : NULL;
}
