/*
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  settings.h
 * @brief Loader settings that can be set via a configurator app.
 */

#ifndef SOLOADER_SETTINGS_H
#define SOLOADER_SETTINGS_H

#include "stdbool.h"

#ifdef __cplusplus
extern "C" {
#endif

// ux0:data/costumequest/config.txt, "key value" per line
extern int setting_msaa;     // 0, 2 or 4 (applied at boot)
extern int setting_fps_cap;  // 60, 45, 30 or 0 (unlimited)
extern int setting_profiler; // game stage timings in debug.log
extern int setting_lua_gcpause;   // LUA_GCSETPAUSE for every state (200 = Lua default)
extern int setting_lua_gcstepmul; // LUA_GCSETSTEPMUL
extern char setting_language[32];   // "auto" or a folder of translations/
extern int setting_controls_gamepad; // "controls gamepad" (1) or "touch" (0), at boot
extern int setting_fast_alloc; // "allocator fast" (1) or "newlib" (0), at boot

void settings_load();
void settings_save();
void settings_reset();

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_SETTINGS_H
