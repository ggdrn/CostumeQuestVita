/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  vita_pad.h
 * @brief The Vita controls exposed to the game as an SDL game controller.
 */

#ifndef SOLOADER_VITA_PAD_H
#define SOLOADER_VITA_PAD_H

#ifdef __cplusplus
extern "C" {
#endif

// Hooks the SDL joystick / game controller functions used by the game
void vita_pad_patch();

// Applies a "map_<button> <source>" line from config.txt; returns 0 if unknown
int vita_pad_set_mapping(const char *key, const char *value);

// Writes the current mapping as config.txt lines
void vita_pad_save_mapping(void *file);

// Mapping access for the port menu: game controller inputs (0..count-1) and
// the Vita sources they read from
int vita_pad_map_count();
const char *vita_pad_map_key(int i);
int vita_pad_map_get(int i);
void vita_pad_map_set(int i, int src);
int vita_pad_source_count();
const char *vita_pad_source_name(int src);
void vita_pad_reset_mapping();

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_VITA_PAD_H
