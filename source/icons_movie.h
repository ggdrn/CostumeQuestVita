/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  icons_movie.h
 * @brief Builds the controller icon library the HUDs import.
 */

#ifndef SOLOADER_ICONS_MOVIE_H
#define SOLOADER_ICONS_MOVIE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Image file prefix of the PlayStation buttons (their textures are elsewhere)
#define ICONS_PS3_PREFIX "PS3__"

// Writes the compressed movie (CFX) to out; returns its size, 0 on failure
size_t icons_movie_build(uint8_t *out, size_t cap);

// Number of symbols it exports
int icons_movie_count();

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_ICONS_MOVIE_H
