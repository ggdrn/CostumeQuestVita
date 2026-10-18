/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  luaprof.h
 * @brief Lua (5.1.2, inside libCq.so) profiling and GC settings.
 */

#ifndef SOLOADER_LUAPROF_H
#define SOLOADER_LUAPROF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Hooks lua_newstate: GC settings and the sampling hook on every state
void luaprof_patch();

// Prints and resets the Lua function times (frames rendered since last call)
void luaprof_log(uint32_t frames);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_LUAPROF_H
