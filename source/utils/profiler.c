/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  profiler.c
 * @brief Per-thread CPU usage, logged by the heartbeat.
 *
 * Threads are registered when created (game threads through
 * pthread_create_soloader, plus the loader's own); the heartbeat reads each
 * one's runClocks and reports the busiest ones as % of a core.
 */

#include "utils/profiler.h"

#include <stdio.h>
#include <string.h>

#include <psp2/kernel/threadmgr.h>

#include "utils/filelog.h"

#define MAX_THREADS 48

typedef struct {
    SceUID thid;
    char label[48];
    uint64_t last_clocks;
} ProfThread;

static ProfThread threads[MAX_THREADS];
static volatile int threads_num = 0;
static SceKernelLwMutexWork lock;
static volatile int lock_ready = 0;

static void lock_get() {
    if (!lock_ready) {
        sceKernelCreateLwMutex(&lock, "profiler", 0, 0, NULL);
        lock_ready = 1;
    }
    sceKernelLockLwMutex(&lock, 1, NULL);
}

void profiler_register_thread(SceUID thid, const char *label) {
    lock_get();
    if (threads_num < MAX_THREADS) {
        ProfThread *t = &threads[threads_num];
        t->thid = thid;
        strncpy(t->label, label, sizeof(t->label) - 1);
        t->label[sizeof(t->label) - 1] = '\0';
        t->last_clocks = 0;
        threads_num++;
    }
    sceKernelUnlockLwMutex(&lock, 1);
}

void profiler_log(uint32_t interval_us) {
    typedef struct { int idx; uint32_t permille; int cpu; } Usage;
    Usage usage[MAX_THREADS];
    int n = 0;

    lock_get();
    for (int i = 0; i < threads_num; i++) {
        SceKernelThreadInfo info;
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(threads[i].thid, &info) < 0)
            continue; // exited
        uint64_t clocks = info.runClocks;
        uint64_t delta = threads[i].last_clocks ? clocks - threads[i].last_clocks : 0;
        threads[i].last_clocks = clocks;
        uint32_t permille = (uint32_t)(delta * 1000 / interval_us);
        if (permille >= 20) { // >= 2% of a core
            usage[n].idx = i;
            usage[n].permille = permille;
            usage[n].cpu = info.lastExecutedCpuId;
            n++;
        }
    }

    // Busiest first
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (usage[j].permille > usage[i].permille) {
                Usage tmp = usage[i];
                usage[i] = usage[j];
                usage[j] = tmp;
            }

    char line[512];
    int len = snprintf(line, sizeof(line), "heartbeat: cpu");
    for (int i = 0; i < n && i < 6 && len < (int)sizeof(line); i++) {
        len += snprintf(line + len, sizeof(line) - len, " | %s %u%% (core %d)",
                        threads[usage[i].idx].label, usage[i].permille / 10, usage[i].cpu);
    }
    sceKernelUnlockLwMutex(&lock, 1);

    if (n)
        filelog_printf("%s\n", line);
}
