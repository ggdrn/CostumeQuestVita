/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  profiler.h
 * @brief Per-thread CPU usage, logged by the heartbeat.
 */

#ifndef SOLOADER_PROFILER_H
#define SOLOADER_PROFILER_H

#include <stdint.h>
#include <psp2/types.h>

#ifdef __cplusplus
extern "C" {
#endif

void profiler_register_thread(SceUID thid, const char *label);

// Logs the busiest threads since the previous call, as % of one core
void profiler_log(uint32_t interval_us);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_PROFILER_H
