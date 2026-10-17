/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/mem.h"
#include "utils/logger.h"

#include <string.h>
#include <malloc.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <stdint.h>

void *sceClibMemclr(void *dst, size_t len) {
    return sceClibMemset(dst, 0, len);
}

/*
 * Anonymous mmap emulated with the heap.
 *
 * Allocators commonly map more than needed and then munmap the unaligned
 * head/tail to get aligned chunks. A plain free() of such a partial range
 * would free a pointer in the middle of a heap block and corrupt the heap,
 * so every mapping is tracked and only whole mappings are released.
 */

#define MAX_MAPPINGS 512

typedef struct {
    uintptr_t start;
    size_t length;
} Mapping;

static Mapping mappings[MAX_MAPPINGS];
static SceKernelLwMutexWork mappings_lock;
static int mappings_lock_ready = 0;

static void mappings_lock_get() {
    if (!mappings_lock_ready) {
        sceKernelCreateLwMutex(&mappings_lock, "mmap", 0, 0, NULL);
        mappings_lock_ready = 1;
    }
    sceKernelLockLwMutex(&mappings_lock, 1, NULL);
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offs) {
    if (length <= 0) {
        return MAP_FAILED;
    }

    // Page aligned like a real mapping; 64KB for big ones, which is what the
    // map-and-trim allocators usually want, so the trimming becomes a no-op
    size_t align = length >= 0x10000 ? 0x10000 : 0x1000;
    void* ret = memalign(align, length);
    if (!ret) {
        l_error("mmap(%p, %u, %i, %i, %i, %li): out of memory", addr, length, prot, flags, fd, offs);
        return MAP_FAILED;
    }
    memset(ret, 0, length);

    mappings_lock_get();
    int slot = -1;
    for (int i = 0; i < MAX_MAPPINGS; i++) {
        if (!mappings[i].start) {
            mappings[i].start = (uintptr_t)ret;
            mappings[i].length = length;
            slot = i;
            break;
        }
    }
    sceKernelUnlockLwMutex(&mappings_lock, 1);

    if (slot < 0)
        l_error("mmap: too many mappings, %p will never be released", ret);
    l_debug("mmap(%p, %u, %i, %i, %i, %li): %p", addr, length, prot, flags, fd, offs, ret);
    return ret;
}

int munmap(void *addr, size_t length) {
    uintptr_t start = (uintptr_t)addr;

    mappings_lock_get();
    for (int i = 0; i < MAX_MAPPINGS; i++) {
        Mapping *m = &mappings[i];
        if (!m->start || start < m->start || start >= m->start + m->length)
            continue;

        if (start == m->start && length >= m->length) {
            l_debug("munmap(%p, %u): released", addr, length);
            m->start = 0;
            sceKernelUnlockLwMutex(&mappings_lock, 1);
            free(addr);
            return 0;
        }

        // Partial unmap (alignment trimming): keep the whole block allocated
        l_debug("munmap(%p, %u): partial unmap of %p (%u), ignored", addr, length, (void *)m->start, m->length);
        sceKernelUnlockLwMutex(&mappings_lock, 1);
        return 0;
    }
    sceKernelUnlockLwMutex(&mappings_lock, 1);

    l_warn("munmap(%p, %u): not a known mapping, ignored", addr, length);
    return 0;
}
