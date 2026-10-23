/* Run with host pthread shims, using the production cache implementation. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <psp2/kernel/threadmgr.h>

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int slow_fd, entered, queued, release_card, other_done, close_started, close_done;
static ssize_t card_read(int fd, void *buf, size_t n) {
    pthread_mutex_lock(&gate);
    if (fd == slow_fd && !entered) {
        entered = 1;
        pthread_cond_broadcast(&changed);
        while (!release_card) pthread_cond_wait(&changed, &gate);
    }
    pthread_mutex_unlock(&gate);
    return read(fd, buf, n);
}
#define read card_read
#ifndef FDCACHE_SOURCE
#define FDCACHE_SOURCE "../source/reimpl/fdcache.c"
#endif
#include FDCACHE_SOURCE
#undef read

int sceKernelCreateLwMutex(SceKernelLwMutexWork *m, const char *name, int a, int b, void *p) {
    (void)a; (void)b; (void)p;
    m->name = name;
    return pthread_mutex_init(&m->mutex, NULL);
}
int sceKernelLockLwMutex(SceKernelLwMutexWork *m, int n, void *p) {
    (void)n; (void)p;
    if (pthread_mutex_trylock(&m->mutex) == 0) return 0;
    if (!strcmp(m->name, "fdcache")) {
        pthread_mutex_lock(&gate);
        queued = 1;
        pthread_cond_broadcast(&changed);
        pthread_mutex_unlock(&gate);
    }
    return pthread_mutex_lock(&m->mutex);
}
int sceKernelUnlockLwMutex(SceKernelLwMutexWork *m, int n) {
    (void)n; return pthread_mutex_unlock(&m->mutex);
}
int sceKernelDeleteLwMutex(SceKernelLwMutexWork *m) { return pthread_mutex_destroy(&m->mutex); }
static struct { pthread_mutex_t lock; pthread_cond_t cv; int count; } sems[128];
static int next_sem;
int sceKernelCreateSema(const char *s, int a, int count, int max, void *p) {
    (void)s; (void)a; (void)max; (void)p;
    int id = ++next_sem;
    assert(id < 128);
    pthread_mutex_init(&sems[id].lock, NULL);
    pthread_cond_init(&sems[id].cv, NULL);
    sems[id].count = count;
    return id;
}
int sceKernelWaitSema(int id, int count, void *p) {
    (void)p;
    pthread_mutex_lock(&sems[id].lock);
    while (sems[id].count < count) pthread_cond_wait(&sems[id].cv, &sems[id].lock);
    sems[id].count -= count;
    pthread_mutex_unlock(&sems[id].lock);
    return 0;
}
int sceKernelSignalSema(int id, int count) {
    pthread_mutex_lock(&sems[id].lock);
    sems[id].count += count;
    pthread_cond_signal(&sems[id].cv);
    pthread_mutex_unlock(&sems[id].lock);
    return 0;
}
int sceKernelDeleteSema(int id) {
    pthread_mutex_destroy(&sems[id].lock);
    return pthread_cond_destroy(&sems[id].cv);
}
int sceKernelDelayThread(unsigned us) {
    struct timespec t = { us / 1000000, (us % 1000000) * 1000 };
    return nanosleep(&t, NULL);
}
uint64_t sceKernelGetProcessTimeWide(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
int apk_index_read(uint32_t pos, void *dst, size_t n) { (void)pos; (void)dst; (void)n; return 0; }
static void wait_flag(int *flag) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 3;
    pthread_mutex_lock(&gate);
    while (!*flag) assert(pthread_cond_timedwait(&changed, &gate, &deadline) == 0);
    pthread_mutex_unlock(&gate);
}
static void *reader(void *p) {
    unsigned char value = 255;
    assert(read_soloader(slow_fd, &value, 1) == 1);
    assert(value == (uintptr_t)p);
    return NULL;
}
static void *closer(void *p) {
    (void)p;
    pthread_mutex_lock(&gate);
    close_started = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
    fdcache_detach(slow_fd);
    pthread_mutex_lock(&gate);
    close_done = 1;
    pthread_mutex_unlock(&gate);
    return NULL;
}
static void *other_reader(void *p) {
    int fd = (int)(intptr_t)p;
    unsigned char value = 255;
    assert(lseek_soloader(fd, 300, SEEK_SET) == 300);
    assert(read_soloader(fd, &value, 1) == 1 && value == (300 % 251));
    pthread_mutex_lock(&gate);
    other_done = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
    return NULL;
}
int main(void) {
    char path[] = "/tmp/cq-fdcache-XXXXXX";
    slow_fd = mkstemp(path);
    assert(slow_fd >= 0 && slow_fd < 128);
    unsigned char contents[20000];
    for (size_t i = 0; i < sizeof(contents); ++i) contents[i] = i % 251;
    assert(write(slow_fd, contents, sizeof(contents)) == sizeof(contents));
    assert(lseek(slow_fd, 0, SEEK_SET) == 0);
    int other = open(path, O_RDONLY);
    unlink(path);
    assert(other >= 0 && other < 128);
    fdcache_attach(slow_fd);
    fdcache_attach(other);
    pthread_t a, b, c, d;
    pthread_create(&a, NULL, reader, (void *)(uintptr_t)0);
    wait_flag(&entered);
    pthread_create(&b, NULL, reader, (void *)(uintptr_t)1);
    wait_flag(&queued);
    pthread_create(&c, NULL, closer, NULL);
    wait_flag(&close_started);
    pthread_create(&d, NULL, other_reader, (void *)(intptr_t)other);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 1;
    pthread_mutex_lock(&gate);
    while (!other_done && pthread_cond_timedwait(&changed, &gate, &deadline) == 0) {}
    int independent = other_done;
    assert(!close_done);
    release_card = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
    pthread_join(a, NULL); pthread_join(b, NULL); pthread_join(c, NULL); pthread_join(d, NULL);
    assert(close_done);
    assert(independent && "unrelated descriptor blocked by global cache lock");
    // Cross-block data, EOF and overlay boundaries still return exact bytes.
    unsigned char out[20010];
    assert(lseek_soloader(other, 0, SEEK_SET) == 0);
    assert(read_soloader(other, out, sizeof(out)) == sizeof(contents));
    assert(!memcmp(out, contents, sizeof(contents)));
    assert(read_soloader(other, out, 1) == 0);
    fdcache_mark_apk(other);
    const unsigned char replacement[] = { 90, 91, 92 };
    assert(fdcache_set_apk_overlay(100, replacement, sizeof(replacement)) == 0);
    assert(lseek_soloader(other, 98, SEEK_SET) == 98);
    assert(read_soloader(other, out, 7) == 7);
    assert(out[0] == 98 && out[1] == 99 && !memcmp(out + 2, replacement, 3));
    assert(out[5] == 103 && out[6] == 104);
    fdcache_detach(other);
    close(slow_fd); close(other);
    puts("Cache: independent descriptors, queued readers, detach lifetime, data and overlays passed");
}
