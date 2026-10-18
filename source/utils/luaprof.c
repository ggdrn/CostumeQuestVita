/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  luaprof.c
 * @brief Lua (5.1.2, inside libCq.so) profiling and GC settings.
 *
 * "Mission lua" is the main thread's costliest stage, but a sampling hook
 * found almost no time in the Lua VM itself: it goes to the game's C
 * functions the scripts call. Every Lua state the game creates gets a call /
 * return hook that times each Lua and C function called through Lua on the
 * main thread; the heartbeat lists the costliest by self time.
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

/*
 * Call / return hook: every Lua or C function called through Lua on the main
 * thread is timed (inclusive and self), keyed by "script:line" for Lua
 * functions and "C name" for the game's C functions.
 */

#define LUA_HOOKCALL    0
#define LUA_HOOKRET     1
#define LUA_HOOKTAILRET 4
#define LUA_MASKCALL    (1 << 0)
#define LUA_MASKRET     (1 << 1)

typedef struct {
    char key[56];
    uint32_t hash;
    uint32_t calls;
    uint64_t incl_us, self_us;
} FuncStat;

typedef struct {
    int16_t stat;      // index in funcs, -1 = untracked
    uint64_t t0, child_us;
} Frame;

typedef struct {
    lua_State *L;      // a state or coroutine
    int depth;
    Frame stack[64];
} CallStack;

#define MAX_FUNCS  256
#define MAX_STACKS 16
static FuncStat funcs[MAX_FUNCS];
static CallStack stacks[MAX_STACKS];
static uint64_t top_level_us; // time inside outermost Lua calls
static volatile uint32_t lua_kb;

static uint32_t hash_str(const char *s) {
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static int func_slot(const char *key) {
    uint32_t h = hash_str(key);
    for (int n = 0; n < MAX_FUNCS; n++) {
        int i = (h + n) % MAX_FUNCS;
        if (!funcs[i].key[0]) {
            snprintf(funcs[i].key, sizeof(funcs[i].key), "%s", key);
            funcs[i].hash = h;
            return i;
        }
        if (funcs[i].hash == h && strcmp(funcs[i].key, key) == 0)
            return i;
    }
    return -1;
}

static CallStack *stack_of(lua_State *L) {
    CallStack *free_slot = NULL;
    for (int i = 0; i < MAX_STACKS; i++) {
        if (stacks[i].L == L)
            return &stacks[i];
        if (!free_slot && (!stacks[i].L || stacks[i].depth == 0))
            free_slot = &stacks[i];
    }
    if (free_slot) {
        free_slot->L = L;
        free_slot->depth = 0;
    }
    return free_slot;
}

static void pop_frame(CallStack *cs, uint64_t now) {
    cs->depth--;
    if (cs->depth >= 64)
        return;
    Frame *f = &cs->stack[cs->depth];
    uint64_t incl = now - f->t0;
    if (cs->depth > 0 && cs->depth - 1 < 64)
        cs->stack[cs->depth - 1].child_us += incl;
    else
        top_level_us += incl;
    if (f->stat >= 0) {
        FuncStat *st = &funcs[f->stat];
        st->calls++;
        st->incl_us += incl;
        st->self_us += incl > f->child_us ? incl - f->child_us : 0;
    }
}

static void call_hook(lua_State *L, lua_Debug *ar) {
    if (!setting_profiler || !waitstats_is_main())
        return;
    uint64_t now = sceKernelGetProcessTimeWide();
    CallStack *cs = stack_of(L);
    if (!cs)
        return;
    if (ar->event == LUA_HOOKCALL) {
        int stat = -1;
        if (cs->depth < 64 && p_getinfo(L, "Sn", ar)) {
            char key[64];
            if (ar->what && ar->what[0] == 'C')
                snprintf(key, sizeof(key), "C %s", ar->name ? ar->name : "?");
            else
                snprintf(key, sizeof(key), "%s:%d", ar->short_src, ar->linedefined);
            stat = func_slot(key);
        }
        if (cs->depth < 64)
            cs->stack[cs->depth] = (Frame){ (int16_t)stat, sceKernelGetProcessTimeWide(), 0 };
        cs->depth++;
    } else if (ar->event == LUA_HOOKRET || ar->event == LUA_HOOKTAILRET) {
        if (cs->depth > 0)
            pop_frame(cs, now);
    }
    if (cs->depth == 0)
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
    p_sethook(L, call_hook, LUA_MASKCALL | LUA_MASKRET, 0);
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
    // Racy with the main thread, but only ever loses a call or two
    static FuncStat snap[MAX_FUNCS];
    memcpy(snap, funcs, sizeof(snap));
    memset(funcs, 0, sizeof(funcs));
    uint64_t total = top_level_us;
    top_level_us = 0;
    if (frames == 0)
        frames = 1;
    if (!total)
        return;
    filelog_printf("heartbeat: lua calls %u.%u ms/frame in total, heap %u KB\n",
                   (unsigned)(total / frames / 1000), (unsigned)(total / frames / 100 % 10), lua_kb);
    for (int n = 0; n < 12; n++) {
        int best = -1;
        for (int i = 0; i < MAX_FUNCS; i++)
            if (snap[i].calls && (best < 0 || snap[i].self_us > snap[best].self_us))
                best = i;
        if (best < 0 || snap[best].self_us * 50 < total) // under 2%
            break;
        filelog_printf("heartbeat:   %-44s self %2u.%u incl %2u.%u ms/frame, %u calls/frame\n", snap[best].key,
                       (unsigned)(snap[best].self_us / frames / 1000), (unsigned)(snap[best].self_us / frames / 100 % 10),
                       (unsigned)(snap[best].incl_us / frames / 1000), (unsigned)(snap[best].incl_us / frames / 100 % 10),
                       (snap[best].calls + frames / 2) / frames);
        snap[best].calls = 0;
    }
}
