/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  fdcache.c
 * @brief Read-ahead cache for read-only file descriptors.
 *
 * Each fd gets a few aligned blocks with LRU replacement. Zip parsing
 * alternates between the central directory (end of the file) and the local
 * headers (ascending offsets), so a single buffer would be thrown away on
 * every jump.
 */

#include "reimpl/fdcache.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "utils/logger.h"

#define FDCACHE_MAX_FD     128
#ifndef FDCACHE_BLOCK_SIZE
#define FDCACHE_BLOCK_SIZE (64 * 1024)
#endif
#ifndef FDCACHE_BLOCKS
#define FDCACHE_BLOCKS     8
#endif

typedef struct {
    uint8_t *buf;      // allocated on first use
    off_t start;       // aligned file offset of buf[0], -1 if empty
    int len;           // valid bytes (short at EOF)
    uint32_t last_use;
} FdBlock;

typedef struct {
    FdBlock blocks[FDCACHE_BLOCKS];
    uint32_t tick;
    off_t pos;         // position as seen by the game
    SceKernelLwMutexWork lock;
} FdCache;

static FdCache *caches[FDCACHE_MAX_FD];

// Guards `caches`: a fd closed on one thread while another reads it must not
// free the cache under the reader
static SceKernelLwMutexWork table_lock;
static volatile int table_lock_ready = 0;

static void table_lock_get() {
    if (!table_lock_ready) {
        sceKernelCreateLwMutex(&table_lock, "fdcache_tbl", 0, 0, NULL);
        table_lock_ready = 1;
    }
    sceKernelLockLwMutex(&table_lock, 1, NULL);
}

static void table_lock_put() {
    sceKernelUnlockLwMutex(&table_lock, 1);
}

FdCacheStats fdcache_stats;

void fdcache_attach(int fd) {
    if (fd < 0 || fd >= FDCACHE_MAX_FD)
        return;
    // A cache left over from a fd closed outside close() would hold another
    // file's data
    fdcache_detach(fd);

    FdCache *c = calloc(1, sizeof(FdCache));
    if (!c)
        return;
    for (int i = 0; i < FDCACHE_BLOCKS; i++)
        c->blocks[i].start = -1;
    c->pos = lseek(fd, 0, SEEK_CUR);
    sceKernelCreateLwMutex(&c->lock, "fdcache", 0, 0, NULL);
    table_lock_get();
    caches[fd] = c;
    table_lock_put();
}

void fdcache_detach(int fd) {
    if (fd < 0 || fd >= FDCACHE_MAX_FD)
        return;

    table_lock_get();
    FdCache *c = caches[fd];
    caches[fd] = NULL;
    if (c) {
        // Wait for a reader still inside this cache
        sceKernelLockLwMutex(&c->lock, 1, NULL);
        sceKernelUnlockLwMutex(&c->lock, 1);
    }
    table_lock_put();
    if (!c)
        return;

    sceKernelDeleteLwMutex(&c->lock);
    for (int i = 0; i < FDCACHE_BLOCKS; i++)
        free(c->blocks[i].buf);
    free(c);
}

// Returns the fd's cache already locked, or NULL
static FdCache *get_cache_locked(int fd) {
    if (fd < 0 || fd >= FDCACHE_MAX_FD)
        return NULL;
    table_lock_get();
    FdCache *c = caches[fd];
    if (c)
        sceKernelLockLwMutex(&c->lock, 1, NULL);
    table_lock_put();
    return c;
}

static ssize_t timed_pread(int fd, void *buf, size_t count, off_t offset) {
    uint64_t t = sceKernelGetProcessTimeWide();
    ssize_t r = -1;
    if (lseek(fd, offset, SEEK_SET) >= 0)
        r = read(fd, buf, count);
    fdcache_stats.refills++;
    fdcache_stats.refill_us += sceKernelGetProcessTimeWide() - t;
    if (r > 0)
        fdcache_stats.card_bytes += r;
    return r;
}

// Returns the block holding `pos`, reading it from the card if needed
static FdBlock *get_block(int fd, FdCache *c, off_t pos) {
    off_t start = pos - (pos % FDCACHE_BLOCK_SIZE);
    FdBlock *victim = &c->blocks[0];

    for (int i = 0; i < FDCACHE_BLOCKS; i++) {
        FdBlock *b = &c->blocks[i];
        if (b->start == start) {
            fdcache_stats.hits++;
            b->last_use = ++c->tick;
            return b;
        }
        if (b->start < 0 || b->last_use < victim->last_use)
            victim = b;
        if (b->start < 0)
            break;
    }

    if (!victim->buf) {
        victim->buf = malloc(FDCACHE_BLOCK_SIZE);
        if (!victim->buf)
            return NULL;
    }

    ssize_t r = timed_pread(fd, victim->buf, FDCACHE_BLOCK_SIZE, start);
    if (r <= 0) {
        victim->start = -1;
        return NULL;
    }
    victim->start = start;
    victim->len = r;
    victim->last_use = ++c->tick;
    return victim;
}

ssize_t read_soloader(int fd, void *buf, size_t count) {
    fdcache_stats.reads++;
    fdcache_stats.last_fd = fd;

    FdCache *c = get_cache_locked(fd);
    if (!c) {
        ssize_t ret = read(fd, buf, count);
        if (ret > 0)
            fdcache_stats.bytes += ret;
        return ret;
    }

    size_t done = 0;
    while (done < count) {
        // Big reads go straight to the card
        if (count - done >= FDCACHE_BLOCK_SIZE) {
            fdcache_stats.direct++;
            ssize_t r = timed_pread(fd, (uint8_t *)buf + done, count - done, c->pos);
            if (r <= 0)
                break;
            done += r;
            c->pos += r;
            continue;
        }

        FdBlock *b = get_block(fd, c, c->pos);
        if (!b)
            break; // EOF or error
        off_t in_block = c->pos - b->start;
        if (in_block >= b->len)
            break; // EOF inside a short block
        size_t n = b->len - in_block;
        if (n > count - done)
            n = count - done;
        memcpy((uint8_t *)buf + done, b->buf + in_block, n);
        done += n;
        c->pos += n;
    }

    fdcache_stats.last_pos = c->pos;
    sceKernelUnlockLwMutex(&c->lock, 1);

    fdcache_stats.bytes += done;
    return done;
}

off_t lseek_soloader(int fd, off_t offset, int whence) {
    fdcache_stats.seeks++;

    FdCache *c = get_cache_locked(fd);
    if (!c)
        return lseek(fd, offset, whence);

    off_t ret;
    switch (whence) {
        case SEEK_SET:
            ret = offset;
            break;
        case SEEK_CUR:
            ret = c->pos + offset;
            break;
        default:
            // SEEK_END needs the file size; reads always seek themselves
            ret = lseek(fd, offset, whence);
            break;
    }

    if (ret >= 0)
        c->pos = ret;
    else
        l_warn("lseek(%i, %li, %i): invalid offset", fd, (long)offset, whence);

    fdcache_stats.last_pos = c->pos;
    sceKernelUnlockLwMutex(&c->lock, 1);
    return ret < 0 ? -1 : ret;
}
