/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  fastheap.h
 * @brief Small-block allocator for the game's malloc / memalign / free.
 */

#ifndef SOLOADER_FASTHEAP_H
#define SOLOADER_FASTHEAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    volatile uint32_t allocs;    // malloc / memalign / calloc (+ realloc of NULL)
    volatile uint32_t small;     // of those, served by the arena
    volatile uint32_t frees;
    volatile uint32_t reallocs;
    volatile uint64_t main_us;   // main thread time in all of them (profiler on)
} FastHeapStats;

extern FastHeapStats fastheap_stats;

// Reserves the arena unless "allocator newlib"; call after settings_load()
void fastheap_init();

void *fastheap_malloc(size_t size);
void *fastheap_memalign(size_t align, size_t size);
void *fastheap_calloc(size_t n, size_t size);
void *fastheap_realloc(void *p, size_t size);
void fastheap_free(void *p);

size_t fastheap_arena_used();

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_FASTHEAP_H
