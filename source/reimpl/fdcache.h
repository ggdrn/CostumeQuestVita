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
    volatile uint64_t bytes;        // bytes returned to the .so
    volatile uint32_t seeks;        // lseek() calls from the .so
    volatile int last_fd;
    volatile int64_t last_pos;
} FdCacheStats;

extern FdCacheStats fdcache_stats;

void fdcache_attach(int fd);

void fdcache_detach(int fd);

ssize_t read_soloader(int fd, void *buf, size_t count);

off_t lseek_soloader(int fd, off_t offset, int whence);

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_FDCACHE_H
