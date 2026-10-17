/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  fastheap.c
 * @brief Small-block allocator for the game's malloc / memalign / free.
 *
 * Every allocation of the game, Lua's included, goes through
 * MemoryManager -> _df_memalign -> memalign(16, n). newlib's memalign
 * over-allocates and splits a chunk on every call, under a global lock.
 *
 * Blocks up to FH_MAX_SMALL bytes come from size classes instead: the arena
 * is one region split into 64KB pages, each page holding blocks of a single
 * class (16-byte multiples, so every block is 16-byte aligned), with a free
 * list and a lock per class. Anything else, and anything once the arena is
 * full, goes to newlib. Pointers outside the arena (newlib's, the loader's,
 * strdup's) are always handed back to newlib.
 *
 * "allocator newlib" in config.txt turns it off (before the game starts).
 */

#include "reimpl/fastheap.h"

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "utils/filelog.h"
#include "utils/settings.h"
#include "utils/waitstats.h"

#define FH_ARENA_SIZE (48 * 1024 * 1024)
#define FH_PAGE_SHIFT 16
#define FH_PAGE_SIZE  (1 << FH_PAGE_SHIFT)
#define FH_PAGES      (FH_ARENA_SIZE / FH_PAGE_SIZE)
#define FH_MAX_SMALL  512

static const uint16_t class_size[] = {
    16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512,
};
#define FH_CLASSES (sizeof(class_size) / sizeof(*class_size))

typedef struct FreeBlock { struct FreeBlock *next; } FreeBlock;

typedef struct {
    FreeBlock *free;
    uint8_t *bump;      // next never-used block in the current page
    uint8_t *bump_end;
    SceKernelLwMutexWork lock;
} SizeClass;

static uint8_t *arena;
static uint8_t page_class[FH_PAGES]; // class + 1, 0 = unused page
static volatile uint32_t pages_used;
static SceKernelLwMutexWork page_lock;
static SizeClass classes[FH_CLASSES];
static uint8_t size_to_class[FH_MAX_SMALL / 16 + 1];
static int enabled = 0;

FastHeapStats fastheap_stats;

static inline int in_arena(const void *p) {
    return arena && (const uint8_t *)p >= arena && (const uint8_t *)p < arena + FH_ARENA_SIZE;
}

void fastheap_init() {
    for (size_t s = 0, c = 0; s <= FH_MAX_SMALL / 16; s++) {
        while (class_size[c] < s * 16)
            c++;
        size_to_class[s] = c;
    }
    if (!setting_fast_alloc) {
        filelog_printf("fastheap: off (allocator newlib)\n");
        return;
    }
    arena = memalign(FH_PAGE_SIZE, FH_ARENA_SIZE);
    if (!arena) {
        filelog_printf("fastheap: no memory for the arena, using newlib\n");
        return;
    }
    sceKernelCreateLwMutex(&page_lock, "fh_pages", 0, 0, NULL);
    for (size_t c = 0; c < FH_CLASSES; c++)
        sceKernelCreateLwMutex(&classes[c].lock, "fh_class", 0, 0, NULL);
    enabled = 1;
    filelog_printf("fastheap: %u MB arena at %p, blocks up to %u bytes\n",
                   FH_ARENA_SIZE >> 20, arena, FH_MAX_SMALL);
}

// Needs the class lock held
static int refill(SizeClass *sc, int c) {
    sceKernelLockLwMutex(&page_lock, 1, NULL);
    uint32_t page = pages_used;
    if (page < FH_PAGES) {
        pages_used = page + 1;
        page_class[page] = c + 1;
    }
    sceKernelUnlockLwMutex(&page_lock, 1);
    if (page >= FH_PAGES)
        return 0;
    sc->bump = arena + ((size_t)page << FH_PAGE_SHIFT);
    // Whole blocks only: a block never crosses into the next page
    sc->bump_end = sc->bump + (FH_PAGE_SIZE / class_size[c]) * class_size[c];
    return 1;
}

static void *small_alloc(size_t size) {
    int c = size_to_class[(size + 15) >> 4];
    SizeClass *sc = &classes[c];
    void *p = NULL;
    sceKernelLockLwMutex(&sc->lock, 1, NULL);
    if (sc->free) {
        p = sc->free;
        sc->free = sc->free->next;
    } else if (sc->bump < sc->bump_end || refill(sc, c)) {
        p = sc->bump;
        sc->bump += class_size[c];
    }
    sceKernelUnlockLwMutex(&sc->lock, 1);
    return p;
}

static inline size_t block_size(const void *p) {
    size_t page = ((const uint8_t *)p - arena) >> FH_PAGE_SHIFT;
    return class_size[page_class[page] - 1];
}

static void small_free(void *p) {
    size_t page = ((uint8_t *)p - arena) >> FH_PAGE_SHIFT;
    SizeClass *sc = &classes[page_class[page] - 1];
    sceKernelLockLwMutex(&sc->lock, 1, NULL);
    ((FreeBlock *)p)->next = sc->free;
    sc->free = p;
    sceKernelUnlockLwMutex(&sc->lock, 1);
}

/*
 * Timing (main thread, with the profiler on) and call counts
 */

#define TIMED_HEAP(call) ({ \
    int timed_ = setting_profiler && waitstats_is_main(); \
    uint64_t t0_ = timed_ ? sceKernelGetProcessTimeWide() : 0; \
    __typeof__(call) r_ = (call); \
    if (timed_) fastheap_stats.main_us += sceKernelGetProcessTimeWide() - t0_; \
    r_; })

static void *do_memalign(size_t align, size_t size) {
    fastheap_stats.allocs++;
    if (enabled && align <= 16 && size && size <= FH_MAX_SMALL) {
        void *p = small_alloc(size);
        if (p) {
            fastheap_stats.small++;
            return p;
        }
    }
    return align <= 8 ? malloc(size) : memalign(align, size);
}

static void do_free(void *p) {
    if (!p)
        return;
    fastheap_stats.frees++;
    if (in_arena(p))
        small_free(p);
    else
        free(p);
}

static void *do_realloc(void *p, size_t size) {
    fastheap_stats.reallocs++;
    if (!p)
        return do_memalign(16, size);
    if (!in_arena(p))
        return realloc(p, size);
    if (size == 0) {
        small_free(p);
        return NULL;
    }
    size_t old = block_size(p);
    if (size <= old && size > old / 2)
        return p; // still the right class
    void *n = do_memalign(16, size);
    if (!n)
        return NULL;
    memcpy(n, p, old < size ? old : size);
    small_free(p);
    return n;
}

void *fastheap_malloc(size_t size) { return TIMED_HEAP(do_memalign(8, size)); }
void *fastheap_memalign(size_t align, size_t size) { return TIMED_HEAP(do_memalign(align, size)); }
void fastheap_free(void *p) { TIMED_HEAP((do_free(p), 0)); }
void *fastheap_realloc(void *p, size_t size) { return TIMED_HEAP(do_realloc(p, size)); }

void *fastheap_calloc(size_t n, size_t size) {
    if (size && n > (size_t)-1 / size)
        return NULL;
    void *p = TIMED_HEAP(do_memalign(8, n * size));
    if (p)
        memset(p, 0, n * size);
    return p;
}

size_t fastheap_arena_used() {
    return (size_t)pages_used << FH_PAGE_SHIFT;
}
