/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  waitstats.c
 * @brief Where the game's main thread spends time not running.
 *
 * The sleep / condition variable / semaphore / join wrappers report every
 * wait made by the main thread, with the game code that asked for it. The
 * heartbeat prints the totals and the top callers, which tells a frame
 * limiter (sleep) apart from waiting on worker threads (cond / sem).
 */

#include "utils/waitstats.h"

#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "utils/filelog.h"

static volatile SceUID main_thid = -1;

static const char *kind_names[WAIT_NUM] = { "sleep", "cond", "sem", "join" };

typedef struct {
    uintptr_t caller;
    uint8_t kind;
    uint32_t calls;
    uint64_t us;
} CallerStat;

#define MAX_CALLERS 32
static CallerStat callers[MAX_CALLERS];
static uint64_t kind_us[WAIT_NUM];
static uint32_t kind_calls[WAIT_NUM];
static uint32_t yields;
static SceKernelLwMutexWork lock;
static int lock_ready = 0;

void waitstats_set_main_thread(SceUID thid) {
    if (!lock_ready) {
        sceKernelCreateLwMutex(&lock, "waitstats", 0, 0, NULL);
        lock_ready = 1;
    }
    main_thid = thid;
}

int waitstats_is_main(void) {
    return main_thid >= 0 && sceKernelGetThreadId() == main_thid;
}

void waitstats_add(WaitKind kind, uint64_t us, void *caller) {
    if (!lock_ready)
        return;
    sceKernelLockLwMutex(&lock, 1, NULL);
    kind_us[kind] += us;
    kind_calls[kind]++;
    int free_slot = -1;
    for (int i = 0; i < MAX_CALLERS; i++) {
        if (callers[i].calls && callers[i].caller == (uintptr_t)caller && callers[i].kind == kind) {
            callers[i].calls++;
            callers[i].us += us;
            free_slot = -2;
            break;
        }
        if (!callers[i].calls && free_slot == -1)
            free_slot = i;
    }
    if (free_slot >= 0)
        callers[free_slot] = (CallerStat){ (uintptr_t)caller, kind, 1, us };
    sceKernelUnlockLwMutex(&lock, 1);
}

void waitstats_log(uint32_t window_us) {
    if (!lock_ready)
        return;
    CallerStat snap[MAX_CALLERS];
    uint64_t snap_us[WAIT_NUM];
    uint32_t snap_calls[WAIT_NUM], snap_yields;

    sceKernelLockLwMutex(&lock, 1, NULL);
    memcpy(snap, callers, sizeof(snap));
    memcpy(snap_us, kind_us, sizeof(snap_us));
    memcpy(snap_calls, kind_calls, sizeof(snap_calls));
    snap_yields = yields;
    memset(callers, 0, sizeof(callers));
    memset(kind_us, 0, sizeof(kind_us));
    memset(kind_calls, 0, sizeof(kind_calls));
    yields = 0;
    sceKernelUnlockLwMutex(&lock, 1);

    uint64_t total = 0;
    for (int k = 0; k < WAIT_NUM; k++)
        total += snap_us[k];
    filelog_printf("heartbeat: main waits %u%% | sleep %u ms (%u) | cond %u ms (%u) | sem %u ms (%u) | join %u ms | yield %u\n",
                   (unsigned)(total * 100 / window_us),
                   (unsigned)(snap_us[WAIT_SLEEP] / 1000), snap_calls[WAIT_SLEEP],
                   (unsigned)(snap_us[WAIT_COND] / 1000), snap_calls[WAIT_COND],
                   (unsigned)(snap_us[WAIT_SEM] / 1000), snap_calls[WAIT_SEM],
                   (unsigned)(snap_us[WAIT_JOIN] / 1000), snap_yields);

    // Top 3 callers by time
    for (int n = 0; n < 3; n++) {
        int best = -1;
        for (int i = 0; i < MAX_CALLERS; i++)
            if (snap[i].calls && (best < 0 || snap[i].us > snap[best].us))
                best = i;
        if (best < 0 || snap[best].us < window_us / 100) // under 1%
            break;
        char where[160];
        filelog_describe_addr(snap[best].caller, where, sizeof(where));
        filelog_printf("heartbeat:   %s %u ms in %u calls from %s\n", kind_names[snap[best].kind],
                       (unsigned)(snap[best].us / 1000), snap[best].calls, where);
        snap[best].calls = 0;
    }
}

/*
 * Sleep wrappers (the cond / sem / join ones are in reimpl/pthr.c)
 */

int usleep_soloader(useconds_t usec) {
    return WAITSTATS_TIMED(WAIT_SLEEP, usleep(usec));
}

int nanosleep_soloader(const struct timespec *req, struct timespec *rem) {
    return WAITSTATS_TIMED(WAIT_SLEEP, nanosleep(req, rem));
}

unsigned int sleep_soloader(unsigned int seconds) {
    return WAITSTATS_TIMED(WAIT_SLEEP, sleep(seconds));
}

int sched_yield_soloader(void) {
    if (waitstats_is_main())
        yields++;
    return sched_yield();
}
