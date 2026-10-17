/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  apk_index.h
 * @brief Keeps the game data's zip local headers in memory.
 */

#ifndef SOLOADER_APK_INDEX_H
#define SOLOADER_APK_INDEX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Loads index_path, or builds it from the APK (first boot) and saves it
void apk_index_init(const char *apk_path, const char *index_path);

// Copies n bytes at APK offset pos if they lie in a known local header: 1, else 0
int apk_index_read(uint32_t pos, void *dst, size_t n);

extern volatile uint32_t apk_index_hits;

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_APK_INDEX_H
