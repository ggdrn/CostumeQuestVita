/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  apk_index.c
 * @brief Keeps the game data's zip local headers in memory.
 *
 * The game data is a zip (assets/ant.mp3, 35k files) stored inside the APK.
 * At boot the game indexes it entry by entry, and for each one jumps to the
 * entry's 30-byte local header to read a few fields: thousands of tiny reads
 * scattered over 670MB, ~2.5ms each on the memory card.
 *
 * The first boot collects all local headers once (in offset order, sharing
 * reads between neighbours) and saves them to apk_index.bin; later boots
 * load that file in one read. read() on the APK serves any read that falls
 * inside a known local header from memory (reimpl/fdcache.c).
 *
 * Only depends on stdio, so it can be tested on the host.
 */

#include "apk_index.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "apk_extract.h"

#ifdef __vita__
#include "utils/filelog.h"
#define index_log(...) filelog_printf(__VA_ARGS__)
#else
#define index_log(...) printf(__VA_ARGS__)
#endif

#define DATA_ENTRY  "assets/ant.mp3"
#define HDR_SIZE    30
#define MAGIC       0x58495143 // "CQIX"
#define VERSION     1
#define WINDOW      (16 * 1024)

typedef struct {
    uint32_t offset;           // in the APK
    uint8_t hdr[HDR_SIZE];
} __attribute__((packed)) IndexEntry;

typedef struct {
    uint32_t magic, version;
    uint64_t apk_size, apk_mtime;
    uint32_t count, reserved;
} IndexHeader;

static IndexEntry *entries;
static uint32_t entry_count;
volatile uint32_t apk_index_hits;

static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static int read_at(FILE *f, uint32_t off, void *buf, size_t n) {
    return fseek(f, off, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}

// Reads every local header of the data zip; 0 on success
static int build(const char *apk_path) {
    uint32_t base, size;
    int stored;
    ApkResult res = apk_locate_entry(apk_path, DATA_ENTRY, &base, &size, &stored);
    if (res != APK_OK || !stored) {
        index_log("apk_index: %s not found or compressed (%s)\n", DATA_ENTRY, apk_result_str(res));
        return -1;
    }

    FILE *f = fopen(apk_path, "rb");
    if (!f)
        return -1;
    setvbuf(f, NULL, _IONBF, 0);

    // End of central directory of the inner zip (no comment expected, allow 64KB)
    uint32_t tail = size < 22 + 0xFFFF ? size : 22 + 0xFFFF;
    uint8_t *buf = malloc(tail > WINDOW ? tail : WINDOW);
    uint32_t *offsets = NULL;
    uint8_t *cd = NULL;
    int ret = -1;
    if (!buf || !read_at(f, base + size - tail, buf, tail))
        goto out;
    uint32_t cd_offset = 0, cd_size = 0, count = 0;
    for (int i = tail - 22; i >= 0; i--) {
        if (rd32(buf + i) == 0x06054b50) {
            count = rd16(buf + i + 10);
            cd_size = rd32(buf + i + 12);
            cd_offset = rd32(buf + i + 16);
            break;
        }
    }
    if (!count || cd_offset + cd_size > size)
        goto out;

    // Central directory: one sequential read
    cd = malloc(cd_size);
    offsets = malloc(count * sizeof(uint32_t));
    if (!cd || !offsets || !read_at(f, base + cd_offset, cd, cd_size))
        goto out;
    uint32_t n = 0;
    for (uint32_t p = 0; n < count && p + 46 <= cd_size; n++) {
        if (rd32(cd + p) != 0x02014b50)
            break;
        offsets[n] = base + rd32(cd + p + 42);
        p += 46 + rd16(cd + p + 28) + rd16(cd + p + 30) + rd16(cd + p + 32);
    }
    qsort(offsets, n, sizeof(uint32_t), cmp_u32);

    // Local headers in file order: neighbours come from the same read
    entries = malloc(n * sizeof(IndexEntry));
    if (!entries)
        goto out;
    uint32_t win_start = 0, win_len = 0, reads = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t off = offsets[i];
        if (off < win_start || off + HDR_SIZE > win_start + win_len) {
            win_start = off;
            win_len = WINDOW;
            if (win_start + win_len > base + size)
                win_len = base + size - win_start;
            if (win_len < HDR_SIZE || !read_at(f, win_start, buf, win_len))
                goto out;
            reads++;
        }
        entries[i].offset = off;
        memcpy(entries[i].hdr, buf + (off - win_start), HDR_SIZE);
        if (rd32(entries[i].hdr) != 0x04034b50)
            goto out;
    }
    entry_count = n;
    index_log("apk_index: built %u local headers with %u reads\n", n, reads);
    ret = 0;
out:
    if (ret != 0) {
        free(entries);
        entries = NULL;
        entry_count = 0;
    }
    free(cd);
    free(offsets);
    free(buf);
    fclose(f);
    return ret;
}

static int load(const char *index_path, const IndexHeader *want) {
    FILE *f = fopen(index_path, "rb");
    if (!f)
        return -1;
    IndexHeader h;
    int ok = fread(&h, sizeof(h), 1, f) == 1 && h.magic == want->magic && h.version == want->version
             && h.apk_size == want->apk_size && h.apk_mtime == want->apk_mtime && h.count;
    if (ok) {
        entries = malloc(h.count * sizeof(IndexEntry));
        ok = entries && fread(entries, sizeof(IndexEntry), h.count, f) == h.count;
    }
    fclose(f);
    if (!ok) {
        free(entries);
        entries = NULL;
        return -1;
    }
    entry_count = h.count;
    return 0;
}

static void save(const char *index_path, IndexHeader h) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", index_path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return;
    h.count = entry_count;
    int ok = fwrite(&h, sizeof(h), 1, f) == 1
             && fwrite(entries, sizeof(IndexEntry), entry_count, f) == entry_count;
    ok = fclose(f) == 0 && ok;
    remove(index_path);
    if (ok)
        rename(tmp, index_path);
}

void apk_index_init(const char *apk_path, const char *index_path) {
    struct stat st;
    if (stat(apk_path, &st) != 0)
        return;
    IndexHeader h = { MAGIC, VERSION, (uint64_t)st.st_size, (uint64_t)st.st_mtime, 0, 0 };
    if (load(index_path, &h) == 0) {
        index_log("apk_index: loaded %u local headers from %s\n", entry_count, index_path);
        return;
    }
    if (build(apk_path) == 0)
        save(index_path, h);
}

int apk_index_read(uint32_t pos, void *dst, size_t n) {
    if (!entry_count || n == 0 || n > HDR_SIZE)
        return 0;
    // Last entry starting at or before pos
    uint32_t lo = 0, hi = entry_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (entries[mid].offset <= pos)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return 0;
    const IndexEntry *e = &entries[lo - 1];
    if (pos + n > e->offset + HDR_SIZE)
        return 0;
    memcpy(dst, e->hdr + (pos - e->offset), n);
    apk_index_hits++;
    return 1;
}
