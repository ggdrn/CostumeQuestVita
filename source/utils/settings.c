/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdio.h>
#include <string.h>
#include "settings.h"
#include "utils/filelog.h"
#include "vita_pad.h"
#include <stdlib.h>

#define CONFIG_FILE_PATH DATA_PATH"config.txt"

int setting_msaa;
int setting_fps_cap;
int setting_profiler;
int setting_fast_alloc;
int setting_lua_gcpause;
int setting_lua_gcstepmul;

void settings_reset() {
    setting_msaa = 2;
    setting_fps_cap = 60;
    setting_profiler = 1; // TODO: 0 for release 0.1
    setting_fast_alloc = 1;
    setting_lua_gcpause = 200;
    setting_lua_gcstepmul = 180; // the game's own value
}

static void settings_validate() {
    if (setting_msaa != 0 && setting_msaa != 2 && setting_msaa != 4)
        setting_msaa = 2;
    if (setting_fps_cap != 0 && setting_fps_cap != 30 && setting_fps_cap != 45 && setting_fps_cap != 60)
        setting_fps_cap = 60;
    if (setting_lua_gcpause < 50 || setting_lua_gcpause > 1000)
        setting_lua_gcpause = 200;
    if (setting_lua_gcstepmul < 50 || setting_lua_gcstepmul > 1000)
        setting_lua_gcstepmul = 180;
}

void settings_load() {
    settings_reset();

    char line[160], key[64], text[64];

    FILE *config = fopen(CONFIG_FILE_PATH, "r");

    if (config) {
        while (fgets(line, sizeof(line), config)) {
            if (line[0] == '#' || sscanf(line, "%63s %63s", key, text) != 2)
                continue;
            int value = atoi(text);
            if      (strcmp("msaa", key) == 0)    setting_msaa = value;
            else if (strcmp("fps_cap", key) == 0) setting_fps_cap = value;
            else if (strcmp("profiler", key) == 0) setting_profiler = value;
            else if (strcmp("lua_gcpause", key) == 0) setting_lua_gcpause = value;
            else if (strcmp("lua_gcstepmul", key) == 0) setting_lua_gcstepmul = value;
            else if (strcmp("allocator", key) == 0) setting_fast_alloc = strcmp(text, "newlib") != 0;
            else if (!vita_pad_set_mapping(key, text))
                filelog_printf("settings: unknown key \"%s\"\n", key);
        }
        fclose(config);
        settings_validate();
    }
    // Rewrite it: creates it on first boot and adds keys new versions know
    // about, keeping the user's values
    settings_save();

    filelog_printf("settings: msaa %d, fps_cap %d, profiler %d, allocator %s\n", setting_msaa, setting_fps_cap,
                   setting_profiler, setting_fast_alloc ? "fast" : "newlib");
}

void settings_save() {
    FILE *config = fopen(CONFIG_FILE_PATH, "w+");

    if (config) {
        fprintf(config, "%s %d\n", "msaa", setting_msaa);
        fprintf(config, "%s %d\n", "fps_cap", setting_fps_cap);
        // 1: log the game's stage timings in debug.log (small CPU cost); 2: also time every Lua call (slow)
        fprintf(config, "%s %d\n", "profiler", setting_profiler);
        // fast: small-block allocator for the game; newlib: plain malloc (next boot)
        fprintf(config, "%s %s\n", "allocator", setting_fast_alloc ? "fast" : "newlib");
        // Lua garbage collector: the game used pause 0 (never stops); Lua's default is 200. Next boot
        fprintf(config, "%s %d\n", "lua_gcpause", setting_lua_gcpause);
        fprintf(config, "%s %d\n", "lua_gcstepmul", setting_lua_gcstepmul);
        // Controls: game controller input <- Vita source (cross circle square
        // triangle start select l r up down left right rear_tl rear_tr
        // rear_bl rear_br none)
        vita_pad_save_mapping(config);
        fclose(config);
    }
}
