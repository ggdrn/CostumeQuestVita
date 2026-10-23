/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  translation.h
 * @brief Fan translations from ux0:data/costumequest/translations/<name>/.
 */

#ifndef SOLOADER_TRANSLATION_H
#define SOLOADER_TRANSLATION_H

#ifdef __cplusplus
extern "C" {
#endif

// Serves translations/<language>/*.STbl in place of the game's tables (after
// settings_load and apk_index_init, before the game starts)
void translation_init();

// The language code the game must run in for the translation, or NULL
const char *translation_game_language();

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_TRANSLATION_H
