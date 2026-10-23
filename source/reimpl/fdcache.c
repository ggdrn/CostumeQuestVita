/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  fdcache.c
 * @brief Read-ahead cache for read-only file descriptors.
 *
 * Each fd gets a few blocks with LRU replacement. Zip parsing alternates
 * between the central directory (end of the file) and the local headers
 * (ascending offsets), so a single buffer would be thrown away on every jump.
 *
 * Read-ahead is adaptive: a miss away from the previous one fetches only
 * FDCACHE_MIN_READ (the game mostly reads small headers scattered over the
 * 670MB archive, and a fixed 64KB fetch read ~5x more than it used); each
 * miss that continues where the last fetch ended doubles the size, up to
 * FDCACHE_BLOCK_SIZE, for streaming.
 */

#include "reimpl/fdcache.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "utils/logger.h"
#include "apk_index.h"

#define FDCACHE_MAX_FD     128
#ifndef FDCACHE_BLOCK_SIZE
#define FDCACHE_BLOCK_SIZE (128 * 1024) // largest fetch (and buffer size)
#endif
#ifndef FDCACHE_MIN_READ
#define FDCACHE_MIN_READ   (8 * 1024)   // fetch for a random access
#endif
#ifndef FDCACHE_BLOCKS
#define FDCACHE_BLOCKS     8
#endif

typedef struct {
    uint8_t *buf;      // allocated on first use
    off_t start;       // file offset of buf[0], -1 if empty
    int len;           // valid bytes (short at EOF)
    uint32_t last_use;
} FdBlock;

typedef struct {
    FdBlock blocks[FDCACHE_BLOCKS];
    uint32_t tick;
    off_t pos;         // position as seen by the game
    off_t seq_end;     // end of the last card fetch
    int ahead;         // size of the next sequential fetch
    int is_apk;        // serve local header reads from apk_index
    SceKernelLwMutexWork lock;
    unsigned users;   // includes readers waiting for lock; guarded by table_lock
    int closing;
    SceUID drained;
} FdCache;

static FdCache *caches[FDCACHE_MAX_FD];

// Guards `caches`: a fd closed on one thread while another reads it must not
// free the cache under the reader
static SceKernelLwMutexWork table_lock;
static volatile int table_lock_ready = 0; // 0=new, 1=initializing, 2=ready

static void table_lock_get() {
    if (__sync_bool_compare_and_swap(&table_lock_ready, 0, 1)) {
        sceKernelCreateLwMutex(&table_lock, "fdcache_tbl", 0, 0, NULL);
        __sync_synchronize();
        table_lock_ready = 2;
    }
    while (table_lock_ready != 2)
        sceKernelDelayThread(100);
    __sync_synchronize();
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
    if (sceKernelCreateLwMutex(&c->lock, "fdcache", 0, 0, NULL) < 0) {
        free(c);
        return;
    }
    c->drained = sceKernelCreateSema("fdcache_drain", 0, 0, 1, NULL);
    if (c->drained < 0) {
        sceKernelDeleteLwMutex(&c->lock);
        free(c);
        return;
    }
    table_lock_get();
    caches[fd] = c;
    table_lock_put();
}

// APK ranges whose bytes the port replaces (set before the game runs)
#define MAX_OVERLAYS 16
typedef struct {
    uint32_t off, len;
    const uint8_t *data;
} Overlay;
static Overlay overlays[MAX_OVERLAYS];
static int overlay_count;

int fdcache_set_apk_overlay(uint32_t offset, const void *data, uint32_t len) {
    if (overlay_count >= MAX_OVERLAYS || !len)
        return -1;
    for (int i = 0; i < overlay_count; i++)
        if (offset < overlays[i].off + overlays[i].len && overlays[i].off < offset + len)
            return -1; // overlapping
    overlays[overlay_count++] = (Overlay){ offset, len, data };
    return 0;
}

// The overlay covering pos, else the distance to the next one (in *gap; 0 = none)
static const Overlay *overlay_at(off_t pos, off_t *gap) {
    *gap = 0;
    for (int i = 0; i < overlay_count; i++) {
        const Overlay *o = &overlays[i];
        if (pos >= o->off && pos < (off_t)o->off + o->len)
            return o;
        if (o->off > pos && (*gap == 0 || o->off - pos < *gap))
            *gap = o->off - pos;
    }
    return NULL;
}

void fdcache_mark_apk(int fd) {
    if (fd < 0 || fd >= FDCACHE_MAX_FD)
        return;
    table_lock_get();
    if (caches[fd])
        caches[fd]->is_apk = 1;
    table_lock_put();
}

void fdcache_detach(int fd) {
    if (fd < 0 || fd >= FDCACHE_MAX_FD)
        return;

    table_lock_get();
    FdCache *c = caches[fd];
    caches[fd] = NULL;
    int wait = c && c->users;
    if (c)
        c->closing = 1;
    table_lock_put();
    if (!c)
        return;

    // Wait outside the global table lock, including queued readers that pinned c.
    if (wait)
        sceKernelWaitSema(c->drained, 1, NULL);
    sceKernelDeleteSema(c->drained);
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
        ++c->users;
    table_lock_put();
    // A slow read or a queued reader must not stop unrelated file descriptors.
    if (c)
        sceKernelLockLwMutex(&c->lock, 1, NULL);
    return c;
}

static void put_cache(FdCache *c) {
    sceKernelUnlockLwMutex(&c->lock, 1);
    table_lock_get();
    if (--c->users == 0 && c->closing)
        sceKernelSignalSema(c->drained, 1);
    table_lock_put();
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
static FdBlock *get_block(int fd, FdCache *c, off_t pos, size_t want) {
    FdBlock *victim = &c->blocks[0];

    for (int i = 0; i < FDCACHE_BLOCKS; i++) {
        FdBlock *b = &c->blocks[i];
        if (b->start >= 0 && pos >= b->start && pos < b->start + b->len) {
            fdcache_stats.hits++;
            b->last_use = ++c->tick;
            return b;
        }
        if (b->start < 0 || b->last_use < victim->last_use)
            victim = b;
    }

    if (!victim->buf) {
        victim->buf = malloc(FDCACHE_BLOCK_SIZE);
        if (!victim->buf)
            return NULL;
    }

    // Sequential if this continues the previous fetch: grow the read-ahead
    if (pos == c->seq_end && c->ahead) {
        c->ahead *= 2;
        if (c->ahead > FDCACHE_BLOCK_SIZE)
            c->ahead = FDCACHE_BLOCK_SIZE;
    } else {
        c->ahead = FDCACHE_MIN_READ;
    }
    // Start on a 4KB boundary (card sectors), still covering pos
    off_t start = pos & ~(off_t)4095;
    // At least what the game asked for
    size_t size = c->ahead;
    if ((size_t)(pos - start) + want > size)
        size = (size_t)(pos - start) + want;
    if (size > FDCACHE_BLOCK_SIZE)
        size = FDCACHE_BLOCK_SIZE;

    ssize_t r = timed_pread(fd, victim->buf, size, start);
    if (r <= 0 || start + r <= pos) {
        victim->start = -1;
        return NULL;
    }
    victim->start = start;
    victim->len = r;
    victim->last_use = ++c->tick;
    c->seq_end = start + r;
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
    // Zip local header fields (game data index): from memory
    if (c->is_apk && c->pos >= 0 && c->pos < 0xFFFFFFFFLL && apk_index_read((uint32_t)c->pos, buf, count)) {
        c->pos += count;
        done = count;
    }
    while (done < count) {
        size_t want = count - done;
        // File data replaced by the port: never the card's bytes
        if (c->is_apk && overlay_count) {
            off_t gap;
            const Overlay *o = overlay_at(c->pos, &gap);
            if (o) {
                off_t end = (off_t)o->off + o->len;
                size_t n = end - c->pos < (off_t)want ? (size_t)(end - c->pos) : want;
                memcpy((uint8_t *)buf + done, o->data + (c->pos - o->off), n);
                done += n;
                c->pos += n;
                continue;
            }
            if (gap && gap < (off_t)want)
                want = gap;
        }
        // Big reads go straight to the card
        if (want >= FDCACHE_BLOCK_SIZE) {
            fdcache_stats.direct++;
            ssize_t r = timed_pread(fd, (uint8_t *)buf + done, want, c->pos);
            if (r <= 0)
                break;
            done += r;
            c->pos += r;
            continue;
        }

        FdBlock *b = get_block(fd, c, c->pos, want);
        if (!b)
            break; // EOF or error
        off_t in_block = c->pos - b->start;
        size_t n = b->len - in_block;
        if (n > want)
            n = want;
        memcpy((uint8_t *)buf + done, b->buf + in_block, n);
        done += n;
        c->pos += n;
    }

    fdcache_stats.last_pos = c->pos;
    put_cache(c);

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
    put_cache(c);
    return ret < 0 ? -1 : ret;
}
