/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  markers.c
 * @brief The game's own profiler markers, timed.
 *
 * The engine wraps its frame stages in ThreadMarker scopes ("SessionManager
 * Update", "Bullet Step", "Animation Tick", "Flash Update", "Create Render
 * Lists", ...: ~100 names). In this build the constructors and the destructor
 * are empty (bx lr); here they push / pop a per-thread stack and add up each
 * marker's inclusive and self time. The heartbeat prints the main thread's
 * costliest stages per frame and the other threads' per second.
 */

#include "utils/markers.h"

#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <so_util/so_util.h>

#include "utils/filelog.h"
#include "utils/init.h"
#include "utils/logger.h"
#include "utils/settings.h"
#include "utils/waitstats.h"

#define MAX_THREADS 24
#define MAX_DEPTH   32
#define MAX_STATS   256

typedef struct {
    const char *name;
    uint64_t t0;
    uint64_t child_us;
} Scope;

typedef struct {
    volatile SceUID thid;
    int depth;
    Scope stack[MAX_DEPTH];
} ThreadStack;

typedef struct {
    const char *name;
    uint8_t main;
    uint32_t calls;
    uint64_t incl_us;
    uint64_t self_us;
} MarkerStat;

static ThreadStack threads[MAX_THREADS];
static MarkerStat stats[MAX_STATS];
static SceKernelLwMutexWork lock;
static int ready = 0;

static ThreadStack *my_stack() {
    SceUID me = sceKernelGetThreadId();
    for (int i = 0; i < MAX_THREADS; i++)
        if (threads[i].thid == me)
            return &threads[i];
    // New thread: claim a free slot
    sceKernelLockLwMutex(&lock, 1, NULL);
    ThreadStack *t = NULL;
    for (int i = 0; i < MAX_THREADS && !t; i++) {
        if (threads[i].thid == 0) {
            threads[i].depth = 0;
            threads[i].thid = me;
            t = &threads[i];
        }
    }
    sceKernelUnlockLwMutex(&lock, 1);
    return t;
}

static void add_stat(const char *name, int main, uint64_t incl, uint64_t self) {
    sceKernelLockLwMutex(&lock, 1, NULL);
    for (int i = 0; i < MAX_STATS; i++) {
        MarkerStat *s = &stats[i];
        if (s->name == name && s->main == main) {
            s->calls++;
            s->incl_us += incl;
            s->self_us += self;
            break;
        }
        if (!s->name) {
            *s = (MarkerStat){ name, (uint8_t)main, 1, incl, self };
            break;
        }
    }
    sceKernelUnlockLwMutex(&lock, 1);
}

static void push(const char *name) {
    if (!setting_profiler)
        return;
    ThreadStack *t = my_stack();
    if (!t)
        return;
    if (t->depth < MAX_DEPTH)
        t->stack[t->depth] = (Scope){ name ? name : "?", sceKernelGetProcessTimeWide(), 0 };
    t->depth++;
}

static void pop() {
    if (!setting_profiler)
        return;
    ThreadStack *t = my_stack();
    if (!t || t->depth == 0)
        return;
    t->depth--;
    if (t->depth >= MAX_DEPTH)
        return;
    Scope *s = &t->stack[t->depth];
    uint64_t incl = sceKernelGetProcessTimeWide() - s->t0;
    if (t->depth > 0 && t->depth - 1 < MAX_DEPTH)
        t->stack[t->depth - 1].child_us += incl;
    add_stat(s->name, waitstats_is_main(), incl, incl > s->child_us ? incl - s->child_us : 0);
}

// ARM C++ ABI: constructors and destructors return `this`
static void *ThreadMarker_ctor(void *this, const char *name) { push(name); return this; }
static void *ThreadMarker_ctor_s(void *this, const char *name, const char *sub) { push(name); return this; }
static void *ThreadMarker_ctor_u(void *this, const char *name, unsigned n) { push(name); return this; }
static void *ThreadMarker_dtor(void *this) { pop(); return this; }

void markers_patch() {
    sceKernelCreateLwMutex(&lock, "markers", 0, 0, NULL);
    ready = 1;

    static const struct { const char *sym; void *fn; } hooks[] = {
        { "_ZN12ThreadMarkerC1EPKc",    ThreadMarker_ctor },
        { "_ZN12ThreadMarkerC1EPKcS1_", ThreadMarker_ctor_s },
        { "_ZN12ThreadMarkerC1EPKcj",   ThreadMarker_ctor_u },
        { "_ZN12ThreadMarkerD1Ev",      ThreadMarker_dtor },
    };
    for (int i = 0; i < sizeof(hooks) / sizeof(*hooks); i++) {
        uintptr_t addr = so_symbol(&so_mod, hooks[i].sym);
        // Only the empty (bx lr) stubs of this build
        if (!addr || *(uint32_t *)addr != 0xE12FFF1E) {
            l_error("markers: %s missing or not empty", hooks[i].sym);
            continue;
        }
        if (hook_arm_short(&so_mod, addr, (uintptr_t)hooks[i].fn) < 0)
            l_error("markers: no veneer space for %s", hooks[i].sym);
    }
}

static int cmp_self(const MarkerStat *a, const MarkerStat *b) {
    return a->self_us < b->self_us ? 1 : a->self_us > b->self_us ? -1 : 0;
}

void markers_log(uint32_t frames, uint32_t window_us) {
    if (!ready || !setting_profiler)
        return;
    static MarkerStat snap[MAX_STATS];
    sceKernelLockLwMutex(&lock, 1, NULL);
    memcpy(snap, stats, sizeof(snap));
    memset(stats, 0, sizeof(stats));
    sceKernelUnlockLwMutex(&lock, 1);

    int n = 0;
    while (n < MAX_STATS && snap[n].name)
        n++;
    // Small n: selection sort by self time
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (cmp_self(&snap[i], &snap[j]) > 0) {
                MarkerStat tmp = snap[i]; snap[i] = snap[j]; snap[j] = tmp;
            }

    if (frames == 0)
        frames = 1;
    int shown = 0;
    for (int i = 0; i < n && shown < 10; i++) {
        if (!snap[i].main || snap[i].self_us < window_us / 1000) // under 0.1%
            continue;
        filelog_printf("heartbeat: stage %-34s self %2u.%u ms/frame, incl %2u.%u ms, %u calls/frame\n",
                       snap[i].name,
                       (unsigned)(snap[i].self_us / frames / 1000), (unsigned)(snap[i].self_us / frames / 100 % 10),
                       (unsigned)(snap[i].incl_us / frames / 1000), (unsigned)(snap[i].incl_us / frames / 100 % 10),
                       (snap[i].calls + frames / 2) / frames);
        shown++;
    }
    shown = 0;
    for (int i = 0; i < n && shown < 6; i++) {
        if (snap[i].main || snap[i].self_us < window_us / 100) // under 1% of a core
            continue;
        filelog_printf("heartbeat: other  %-34s self %3u%% of a core, %u calls/s\n", snap[i].name,
                       (unsigned)(snap[i].self_us * 100 / window_us),
                       (unsigned)((uint64_t)snap[i].calls * 1000000 / window_us));
        shown++;
    }
}
