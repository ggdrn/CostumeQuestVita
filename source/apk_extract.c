/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  apk_extract.c
 * @brief Extracting single files from the APK (a zip archive).
 *
 * Minimal zip reader: finds the End Of Central Directory record, walks the
 * central directory for the entry, then inflates its data from the local
 * file header. No zip64 (the APK is well under 4GB).
 */

#include "apk_extract.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define SIG_EOCD          0x06054b50
#define SIG_CENTRAL_DIR   0x02014b50
#define SIG_LOCAL_HEADER  0x04034b50

#define METHOD_STORED  0
#define METHOD_DEFLATE 8

#define EOCD_SIZE        22
#define EOCD_MAX_COMMENT 0xFFFF
#define CHUNK_SIZE       (256 * 1024)

static uint16_t rd16(const uint8_t *p) {
    return p[0] | (p[1] << 8);
}

static uint32_t rd32(const uint8_t *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Locates the EOCD record (it is followed by an up to 64KB comment)
static int find_eocd(FILE *f, uint32_t *cd_offset, uint32_t *cd_entries) {
    if (fseek(f, 0, SEEK_END) != 0)
        return -1;
    long file_size = ftell(f);
    if (file_size < EOCD_SIZE)
        return -1;

    long search = EOCD_SIZE + EOCD_MAX_COMMENT;
    if (search > file_size)
        search = file_size;

    uint8_t *buf = malloc(search);
    if (!buf)
        return -1;

    int ret = -1;
    if (fseek(f, file_size - search, SEEK_SET) == 0 && fread(buf, 1, search, f) == (size_t)search) {
        for (long i = search - EOCD_SIZE; i >= 0; i--) {
            if (rd32(buf + i) == SIG_EOCD) {
                *cd_entries = rd16(buf + i + 10);
                *cd_offset = rd32(buf + i + 16);
                ret = 0;
                break;
            }
        }
    }

    free(buf);
    return ret;
}

typedef struct {
    uint16_t method;
    uint32_t crc;
    uint32_t comp_size;
    uint32_t uncomp_size;
    uint32_t local_offset;
} ZipEntry;

static ApkResult find_entry(FILE *f, const char *name, ZipEntry *entry) {
    uint32_t cd_offset, cd_entries;
    if (find_eocd(f, &cd_offset, &cd_entries) != 0)
        return APK_ERR_NOT_ZIP;

    if (fseek(f, cd_offset, SEEK_SET) != 0)
        return APK_ERR_READ;

    size_t name_len = strlen(name);
    uint8_t hdr[46];
    char fname[512];

    for (uint32_t i = 0; i < cd_entries; i++) {
        if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) || rd32(hdr) != SIG_CENTRAL_DIR)
            return APK_ERR_READ;

        uint16_t fname_len = rd16(hdr + 28);
        uint16_t extra_len = rd16(hdr + 30);
        uint16_t comment_len = rd16(hdr + 32);

        if (fname_len == name_len && fname_len < sizeof(fname)) {
            if (fread(fname, 1, fname_len, f) != fname_len)
                return APK_ERR_READ;
            if (memcmp(fname, name, name_len) == 0) {
                entry->method = rd16(hdr + 10);
                entry->crc = rd32(hdr + 16);
                entry->comp_size = rd32(hdr + 20);
                entry->uncomp_size = rd32(hdr + 24);
                entry->local_offset = rd32(hdr + 42);
                return APK_OK;
            }
            fname_len = 0; // already consumed
        }

        if (fseek(f, fname_len + extra_len + comment_len, SEEK_CUR) != 0)
            return APK_ERR_READ;
    }

    return APK_ERR_NOT_FOUND;
}

// Moves to the entry data: local header sizes may differ from the central ones
static ApkResult seek_to_data(FILE *f, const ZipEntry *entry) {
    uint8_t hdr[30];
    if (fseek(f, entry->local_offset, SEEK_SET) != 0
        || fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)
        || rd32(hdr) != SIG_LOCAL_HEADER)
        return APK_ERR_READ;

    if (fseek(f, rd16(hdr + 26) + rd16(hdr + 28), SEEK_CUR) != 0)
        return APK_ERR_READ;

    return APK_OK;
}

static ApkResult copy_stored(FILE *in, FILE *out, const ZipEntry *entry, uint8_t *buf, uLong *crc) {
    uint32_t left = entry->comp_size;
    while (left > 0) {
        size_t n = left < CHUNK_SIZE ? left : CHUNK_SIZE;
        if (fread(buf, 1, n, in) != n)
            return APK_ERR_READ;
        if (fwrite(buf, 1, n, out) != n)
            return APK_ERR_WRITE;
        *crc = crc32(*crc, buf, n);
        left -= n;
    }
    return APK_OK;
}

static ApkResult copy_deflated(FILE *in, FILE *out, const ZipEntry *entry, uint8_t *buf, uLong *crc) {
    uint8_t *obuf = malloc(CHUNK_SIZE);
    if (!obuf)
        return APK_ERR_INFLATE;

    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    // Negative window bits: raw deflate stream, no zlib header (zip format)
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
        free(obuf);
        return APK_ERR_INFLATE;
    }

    ApkResult res = APK_OK;
    uint32_t left = entry->comp_size;
    int zret = Z_OK;

    while (zret != Z_STREAM_END) {
        if (zs.avail_in == 0) {
            if (left == 0) {
                res = APK_ERR_READ;
                break;
            }
            size_t n = left < CHUNK_SIZE ? left : CHUNK_SIZE;
            if (fread(buf, 1, n, in) != n) {
                res = APK_ERR_READ;
                break;
            }
            left -= n;
            zs.next_in = buf;
            zs.avail_in = n;
        }

        zs.next_out = obuf;
        zs.avail_out = CHUNK_SIZE;
        zret = inflate(&zs, Z_NO_FLUSH);
        if (zret != Z_OK && zret != Z_STREAM_END) {
            res = APK_ERR_INFLATE;
            break;
        }

        size_t produced = CHUNK_SIZE - zs.avail_out;
        if (fwrite(obuf, 1, produced, out) != produced) {
            res = APK_ERR_WRITE;
            break;
        }
        *crc = crc32(*crc, obuf, produced);
    }

    inflateEnd(&zs);
    free(obuf);
    return res;
}

ApkResult apk_extract_file(const char *apk_path, const char *entry_name, const char *out_path) {
    FILE *in = fopen(apk_path, "rb");
    if (!in)
        return APK_ERR_OPEN_APK;

    ZipEntry entry;
    ApkResult res = find_entry(in, entry_name, &entry);
    if (res == APK_OK && entry.method != METHOD_STORED && entry.method != METHOD_DEFLATE)
        res = APK_ERR_UNSUPPORTED;
    if (res == APK_OK)
        res = seek_to_data(in, &entry);
    if (res != APK_OK) {
        fclose(in);
        return res;
    }

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", out_path);
    FILE *out = fopen(tmp_path, "wb");
    if (!out) {
        fclose(in);
        return APK_ERR_WRITE;
    }

    uint8_t *buf = malloc(CHUNK_SIZE);
    uLong crc = crc32(0L, Z_NULL, 0);
    if (!buf)
        res = APK_ERR_READ;
    else if (entry.method == METHOD_STORED)
        res = copy_stored(in, out, &entry, buf, &crc);
    else
        res = copy_deflated(in, out, &entry, buf, &crc);

    free(buf);
    fclose(in);
    if (fclose(out) != 0 && res == APK_OK)
        res = APK_ERR_WRITE;

    if (res == APK_OK && crc != entry.crc)
        res = APK_ERR_CRC;

    if (res == APK_OK) {
        remove(out_path);
        if (rename(tmp_path, out_path) != 0)
            res = APK_ERR_WRITE;
    }
    if (res != APK_OK)
        remove(tmp_path);

    return res;
}

ApkResult apk_locate_entry(const char *apk_path, const char *entry_name,
                           uint32_t *data_offset, uint32_t *size, int *stored) {
    FILE *f = fopen(apk_path, "rb");
    if (!f)
        return APK_ERR_OPEN_APK;
    ZipEntry entry;
    ApkResult res = find_entry(f, entry_name, &entry);
    if (res == APK_OK)
        res = seek_to_data(f, &entry);
    if (res == APK_OK) {
        *data_offset = (uint32_t)ftell(f);
        *size = entry.comp_size;
        *stored = entry.method == METHOD_STORED;
    }
    fclose(f);
    return res;
}

const char *apk_result_str(ApkResult res) {
    switch (res) {
        case APK_OK:              return "OK";
        case APK_ERR_OPEN_APK:    return "can't open the APK";
        case APK_ERR_NOT_ZIP:     return "the APK is not a valid zip file";
        case APK_ERR_NOT_FOUND:   return "file not found in the APK";
        case APK_ERR_UNSUPPORTED: return "unsupported compression method";
        case APK_ERR_READ:        return "the APK is truncated or corrupted";
        case APK_ERR_WRITE:       return "can't write the extracted file (is the memory card full?)";
        case APK_ERR_INFLATE:     return "decompression error";
        case APK_ERR_CRC:         return "checksum mismatch (corrupted APK?)";
        default:                  return "unknown error";
    }
}
