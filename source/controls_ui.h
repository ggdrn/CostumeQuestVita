/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  controls_ui.h
 * @brief Controller (PlayStation layout) UI for the game.
 */

#ifndef SOLOADER_CONTROLS_UI_H
#define SOLOADER_CONTROLS_UI_H

#ifdef __cplusplus
extern "C" {
#endif

// Installs the controller UI patches (needs settings loaded)
void controls_ui_patch();

// Once per frame, on the game thread (buffer swap)
void controls_ui_frame();

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_CONTROLS_UI_H
