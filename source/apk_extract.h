/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  apk_extract.h
 * @brief Extracting single files from the APK (a zip archive).
 *
 * Only depends on stdio and zlib, so it can be tested on the host as well.
 */

#ifndef SOLOADER_APK_EXTRACT_H
#define SOLOADER_APK_EXTRACT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APK_OK = 0,
    APK_ERR_OPEN_APK = -1,     // can't open the APK
    APK_ERR_NOT_ZIP = -2,      // end of central directory not found
    APK_ERR_NOT_FOUND = -3,    // entry is not in the APK
    APK_ERR_UNSUPPORTED = -4,  // compression method other than stored/deflate
    APK_ERR_READ = -5,         // truncated or corrupted APK
    APK_ERR_WRITE = -6,        // can't write the output file
    APK_ERR_INFLATE = -7,      // zlib error
    APK_ERR_CRC = -8,          // extracted data doesn't match the CRC32
} ApkResult;

/**
 * @brief Extracts @p entry_name from the zip at @p apk_path into @p out_path.
 *
 * Data is written to `<out_path>.tmp` first and renamed when it was fully
 * extracted and its CRC32 matched, so an interrupted extraction never leaves
 * a broken file at @p out_path.
 */
ApkResult apk_extract_file(const char *apk_path, const char *entry_name, const char *out_path);

/**
 * @brief Finds where @p entry_name's data starts inside the APK.
 *
 * @param data_offset  File offset of the first data byte.
 * @param size         Stored (compressed) size.
 * @param stored       1 if the entry is not compressed.
 */
ApkResult apk_locate_entry(const char *apk_path, const char *entry_name,
                           uint32_t *data_offset, uint32_t *size, int *stored);

const char *apk_result_str(ApkResult res);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_APK_EXTRACT_H
