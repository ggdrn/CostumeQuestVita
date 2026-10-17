/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  waitstats.h
 * @brief Where the game's main thread spends time not running.
 */

#ifndef SOLOADER_WAITSTATS_H
#define SOLOADER_WAITSTATS_H

#include <stdint.h>
#include <time.h>
#include <sys/types.h>
#include <psp2/types.h>
#include <psp2/kernel/processmgr.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { WAIT_SLEEP, WAIT_COND, WAIT_SEM, WAIT_JOIN, WAIT_NUM } WaitKind;

void waitstats_set_main_thread(SceUID thid);
int waitstats_is_main(void);
void waitstats_add(WaitKind kind, uint64_t us, void *caller);

// Prints and resets the stats gathered over the last window_us
void waitstats_log(uint32_t window_us);

// Times a call made by the main thread; use directly in the function the game
// calls, so the return address is the game's code
#define WAITSTATS_TIMED(kind, call) ({ \
    void *caller_ = __builtin_return_address(0); \
    int is_main_ = waitstats_is_main(); \
    uint64_t t0_ = is_main_ ? sceKernelGetProcessTimeWide() : 0; \
    __typeof__(call) r_ = (call); \
    if (is_main_) waitstats_add(kind, sceKernelGetProcessTimeWide() - t0_, caller_); \
    r_; })

int usleep_soloader(useconds_t usec);
int nanosleep_soloader(const struct timespec *req, struct timespec *rem);
unsigned int sleep_soloader(unsigned int seconds);
int sched_yield_soloader(void);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_WAITSTATS_H
