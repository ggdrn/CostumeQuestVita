/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  luaprof.c
 * @brief Lua (5.1.2, inside libCq.so) profiling and GC settings.
 *
 * "Mission lua" is the main thread's costliest stage. Every Lua state the
 * game creates gets a count hook: every LUA_SAMPLE_INSNS VM instructions on
 * the main thread, the time since the previous sample is charged to the Lua
 * function running now (C functions it called in between included). The
 * heartbeat lists the costliest functions as script:line.
 *
 * lua_gcpause / lua_gcstepmul in config.txt are applied to every state
 * (Lua's defaults are 200 / 200; a higher pause runs the collector less).
 */

#include "utils/luaprof.h"

#include <stdio.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>

#include <so_util/so_util.h>

#include "utils/filelog.h"
#include "utils/init.h"
#include "utils/logger.h"
#include "utils/settings.h"
#include "utils/waitstats.h"

#define LUA_SAMPLE_INSNS 2000
#define LUA_MASKCOUNT    (1 << 3)
#define LUA_GCSETPAUSE   6
#define LUA_GCSETSTEPMUL 7
#define LUA_GCCOUNT      3
#define LUA_IDSIZE       60

typedef struct lua_State lua_State;

// Lua 5.1 lua_Debug
typedef struct {
    int event;
    const char *name, *namewhat, *what, *source;
    int currentline, nups, linedefined, lastlinedefined;
    char short_src[LUA_IDSIZE];
    int i_ci;
} lua_Debug;

typedef void (*lua_Hook)(lua_State *L, lua_Debug *ar);
typedef void *(*lua_Alloc)(void *ud, void *ptr, size_t osize, size_t nsize);

static lua_State *(*orig_newstate)(lua_Alloc f, void *ud);
static int (*p_sethook)(lua_State *L, lua_Hook f, int mask, int count);
static int (*p_getinfo)(lua_State *L, const char *what, lua_Debug *ar);
static int (*p_gc)(lua_State *L, int what, int data);

typedef struct {
    char src[48];
    int line;          // linedefined
    uint32_t samples;
    uint64_t us;
} FuncStat;

#define MAX_FUNCS 128
static FuncStat funcs[MAX_FUNCS];
static uint64_t last_sample;
static volatile uint32_t lua_kb;

static FuncStat *func_slot(const char *src, int line) {
    for (int i = 0; i < MAX_FUNCS; i++) {
        if (!funcs[i].samples && !funcs[i].src[0]) {
            snprintf(funcs[i].src, sizeof(funcs[i].src), "%s", src);
            funcs[i].line = line;
            return &funcs[i];
        }
        if (funcs[i].line == line && strcmp(funcs[i].src, src) == 0)
            return &funcs[i];
    }
    return NULL;
}

static void count_hook(lua_State *L, lua_Debug *ar) {
    if (!setting_profiler || !waitstats_is_main())
        return;
    uint64_t now = sceKernelGetProcessTimeWide();
    uint64_t dt = now - last_sample;
    last_sample = now;
    if (dt > 20000)
        dt = 0; // a gap between frames / Lua calls, not this function's time
    if (!p_getinfo(L, "S", ar))
        return;
    // short_src keeps the end of long chunk names
    FuncStat *f = func_slot(ar->short_src, ar->linedefined);
    if (f) {
        f->samples++;
        f->us += dt;
    }
    lua_kb = p_gc(L, LUA_GCCOUNT, 0);
}

static lua_State *newstate_hook(lua_Alloc f, void *ud) {
    lua_State *L = orig_newstate(f, ud);
    if (!L)
        return L;
    if (setting_lua_gcpause != 200)
        p_gc(L, LUA_GCSETPAUSE, setting_lua_gcpause);
    if (setting_lua_gcstepmul != 200)
        p_gc(L, LUA_GCSETSTEPMUL, setting_lua_gcstepmul);
    // Coroutines created later inherit the hook
    p_sethook(L, count_hook, LUA_MASKCOUNT, LUA_SAMPLE_INSNS);
    filelog_printf("luaprof: lua_State %p (gc pause %d, stepmul %d)\n", L,
                   setting_lua_gcpause, setting_lua_gcstepmul);
    return L;
}

void luaprof_patch() {
    uintptr_t newstate = so_symbol(&so_mod, "lua_newstate");
    p_sethook = (void *)so_symbol(&so_mod, "lua_sethook");
    p_getinfo = (void *)so_symbol(&so_mod, "lua_getinfo");
    p_gc = (void *)so_symbol(&so_mod, "lua_gc");
    if (!newstate || !p_sethook || !p_getinfo || !p_gc) {
        l_error("luaprof: Lua symbols missing");
        return;
    }
    orig_newstate = (void *)hook_arm_trampoline(&so_mod, newstate, (uintptr_t)newstate_hook);
    if (!orig_newstate)
        l_error("luaprof: can't hook lua_newstate");
}

void luaprof_log(uint32_t frames) {
    if (!setting_profiler)
        return;
    // Racy with the main thread, but only ever loses a sample or two
    static FuncStat snap[MAX_FUNCS];
    memcpy(snap, funcs, sizeof(snap));
    memset(funcs, 0, sizeof(funcs));
    if (frames == 0)
        frames = 1;

    uint64_t total = 0;
    for (int i = 0; i < MAX_FUNCS; i++)
        total += snap[i].us;
    if (!total)
        return;
    filelog_printf("heartbeat: lua %u.%u ms/frame sampled, heap %u KB\n",
                   (unsigned)(total / frames / 1000), (unsigned)(total / frames / 100 % 10), lua_kb);
    for (int n = 0; n < 8; n++) {
        int best = -1;
        for (int i = 0; i < MAX_FUNCS; i++)
            if (snap[i].samples && (best < 0 || snap[i].us > snap[best].us))
                best = i;
        if (best < 0 || snap[best].us * 50 < total) // under 2%
            break;
        filelog_printf("heartbeat:   lua %s:%d  %u.%u ms/frame (%u%%)\n", snap[best].src, snap[best].line,
                       (unsigned)(snap[best].us / frames / 1000), (unsigned)(snap[best].us / frames / 100 % 10),
                       (unsigned)(snap[best].us * 100 / total));
        snap[best].samples = 0;
    }
}
