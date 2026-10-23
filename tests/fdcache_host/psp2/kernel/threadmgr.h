/* Host-only Vita synchronization shim for the cache concurrency regression. */
#ifndef CQ_FDCACHE_HOST_THREADMGR_H
#define CQ_FDCACHE_HOST_THREADMGR_H
#include <pthread.h>
#include <stdint.h>
typedef int SceUID;
typedef struct { pthread_mutex_t mutex; const char *name; } SceKernelLwMutexWork;
int sceKernelCreateLwMutex(SceKernelLwMutexWork *, const char *, int, int, void *);
int sceKernelLockLwMutex(SceKernelLwMutexWork *, int, void *);
int sceKernelUnlockLwMutex(SceKernelLwMutexWork *, int);
int sceKernelDeleteLwMutex(SceKernelLwMutexWork *);
int sceKernelCreateSema(const char *, int, int, int, void *);
int sceKernelWaitSema(int, int, void *);
int sceKernelSignalSema(int, int);
int sceKernelDeleteSema(int);
int sceKernelDelayThread(unsigned);
#endif
