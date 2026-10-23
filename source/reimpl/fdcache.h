/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  fdcache.h
 * @brief Read-ahead cache for read-only file descriptors.
 *
 * The game parses zip directories (the APK and the 35k-entry data pack) with
 * unbuffered 2-4 byte read() calls. Cheap syscalls on Android, but every
 * sceIoRead goes to the memory card on Vita. read()/lseek() on read-only fds
 * are served from a per-fd buffer instead.
 * Cache table synchronization never waits for file IO: operations pin their
 * cache before taking its private lock, and close waits for those pins to drain.
 */

#ifndef SOLOADER_FDCACHE_H
#define SOLOADER_FDCACHE_H

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    volatile uint32_t reads;        // read() calls from the .so
    volatile uint32_t refills;      // actual reads from the card
    volatile uint64_t refill_us;    // time spent in them
    volatile uint64_t card_bytes;   // bytes they returned
    volatile uint32_t hits;         // reads served from cached blocks
    volatile uint32_t direct;       // big reads sent straight to the card
    volatile uint64_t bytes;        // bytes returned to the .so
    volatile uint32_t seeks;        // lseek() calls from the .so
    volatile int last_fd;
    volatile int64_t last_pos;
} FdCacheStats;

extern FdCacheStats fdcache_stats;

// The fd is the APK: reads inside zip local headers come from apk_index
void fdcache_mark_apk(int fd);

// Reads of the APK in [offset, offset + len) return data instead (before the game runs); 0 on success
int fdcache_set_apk_overlay(uint32_t offset, const void *data, uint32_t len);

void fdcache_attach(int fd);

void fdcache_detach(int fd);

ssize_t read_soloader(int fd, void *buf, size_t count);

off_t lseek_soloader(int fd, off_t offset, int whence);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_FDCACHE_H
