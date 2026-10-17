/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  heapguard.c
 * @brief Debug allocator for the .so files: catches heap overflows and
 *        writes into freed memory, and logs who allocated/freed the block.
 *
 * Every block gets a header (magic, size, raw pointer, short backtrace) and a
 * trailing canary. Freed blocks are filled with a poison byte and kept in a
 * quarantine for a while before going back to newlib; when they leave it,
 * the poison is verified.
 *
 * Pointers not allocated here (e.g. from newlib's own strdup) are passed to
 * the real free()/realloc().
 */

#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/threadmgr.h>

#include <so_util/so_util.h>

#include "utils/filelog.h"
#include "utils/init.h"

/*
 * With HEAPGUARD_WRAP the eboot is linked with --wrap=malloc,free,... so
 * every allocation in the process (vitaGL, newlib, pthread, FalsoJNI, the
 * loader) goes through here, not only the ones made by the .so files.
 */
#ifdef HEAPGUARD_WRAP
void *__real_malloc(size_t size);
void __real_free(void *p);
void *__real_realloc(void *p, size_t size);
size_t __real_malloc_usable_size(void *p);
#define REAL(f) __real_##f
#else
#define REAL(f) f
#endif

#define HG_MAGIC_LIVE  0x4C495645 // 'LIVE'
#define HG_MAGIC_FREED 0x46524545 // 'FREE'
#define HG_CANARY      0xFEEDFACE
#define HG_POISON      0xDD
#define HG_BT_DEPTH    3

#define QUARANTINE_SLOTS 2048
#define QUARANTINE_BYTES (16 * 1024 * 1024)

typedef struct {
    uint32_t magic;
    uint32_t size;
    void *raw;                       // what newlib returned
    uintptr_t alloc_bt[HG_BT_DEPTH];
    uintptr_t free_bt[HG_BT_DEPTH];
    uint32_t check;                  // ~size ^ raw: detects writes before the block
} HgHeader;                          // 40 bytes

_Static_assert(sizeof(HgHeader) % 8 == 0, "header must keep 8-byte alignment");

static HgHeader *quarantine[QUARANTINE_SLOTS];
static int q_head = 0, q_count = 0;
static size_t q_bytes = 0;

static SceKernelLwMutexWork hg_lock;
static volatile int hg_lock_ready = 0;
static volatile int reports = 0;

static void lock() {
    if (!hg_lock_ready) {
        sceKernelCreateLwMutex(&hg_lock, "heapguard", 2 /* recursive */, 0, NULL);
        hg_lock_ready = 1;
    }
    sceKernelLockLwMutex(&hg_lock, 1, NULL);
}

static void unlock() {
    sceKernelUnlockLwMutex(&hg_lock, 1);
}

extern char __executable_start[], __etext[];

static int in_so_text(uintptr_t a) {
    if (a >= (uintptr_t)__executable_start && a < (uintptr_t)__etext)
        return 1;
    so_module *mods[] = { &so_mod, &fmodevent_mod, &fmodex_mod };
    for (int i = 0; i < 3; i++)
        if (mods[i]->text_base && a >= mods[i]->text_base && a < mods[i]->text_base + mods[i]->text_size)
            return 1;
    return 0;
}

// Poor man's backtrace: first .so code addresses found on the stack
static void backtrace_so(uintptr_t *out, void *caller) {
    uint32_t *sp = (uint32_t *)__builtin_frame_address(0);
    int n = 0;
    if (caller)
        out[n++] = (uintptr_t)caller & ~1;
    for (int i = 0; i < 96 && n < HG_BT_DEPTH; i++) {
        uintptr_t v = sp[i] & ~1;
        if (in_so_text(v) && (n == 0 || v != out[0]))
            out[n++] = v;
    }
    while (n < HG_BT_DEPTH)
        out[n++] = 0;
}

static void log_bt(const char *what, const uintptr_t *bt) {
    for (int i = 0; i < HG_BT_DEPTH && bt[i]; i++) {
        char where[160];
        filelog_describe_addr(bt[i], where, sizeof(where));
        filelog_printf("    %s #%d: %s\n", what, i, where);
    }
}

static void report(const char *kind, HgHeader *h, uint32_t offset, uint32_t value) {
    if (reports++ >= 20)
        return;
    filelog_printf("HEAP: %s in block %p (size %u) at offset %u, value 0x%08X\n",
                   kind, (void *)(h + 1), h->size, offset, value);
    log_bt("allocated by", h->alloc_bt);
    if (h->magic == HG_MAGIC_FREED)
        log_bt("freed by", h->free_bt);
}

static int check_canary(HgHeader *h) {
    uint32_t c;
    memcpy(&c, (uint8_t *)(h + 1) + h->size, 4);
    if (c != HG_CANARY) {
        report("OVERFLOW past the end", h, h->size, c);
        return 0;
    }
    return 1;
}

static void check_poison(HgHeader *h) {
    uint8_t *p = (uint8_t *)(h + 1);
    for (uint32_t i = 0; i < h->size; i++) {
        if (p[i] != HG_POISON) {
            uint32_t v = 0;
            memcpy(&v, p + (i & ~3), (i & ~3) + 4 <= h->size ? 4 : 1);
            report("WRITE AFTER FREE", h, i, v);
            return;
        }
    }
}

static uint32_t header_check(HgHeader *h) {
    return ~h->size ^ (uint32_t)(uintptr_t)h->raw;
}

static void *guarded_alloc(size_t size, size_t align, void *caller) {
    // newlib rounds odd alignments up to a power of two, so do we
    size_t a = 8;
    while (a < align)
        a <<= 1;
    align = a;
    // raw | padding | header | user (aligned) | canary
    size_t total = size + sizeof(HgHeader) + align + 4;
    uint8_t *raw = REAL(malloc)(total);
    if (!raw)
        return NULL;
    uintptr_t user = ((uintptr_t)raw + sizeof(HgHeader) + align - 1) & ~(uintptr_t)(align - 1);
    HgHeader *h = (HgHeader *)user - 1;
    h->magic = HG_MAGIC_LIVE;
    h->size = size;
    h->raw = raw;
    h->check = header_check(h);
    backtrace_so(h->alloc_bt, caller);
    uint32_t c = HG_CANARY;
    memcpy((uint8_t *)user + size, &c, 4);
    return (void *)user;
}

/*
 * Sanity check for pointers we did not allocate, before handing them to
 * newlib: inside the sbrk heap, sane chunk size, and marked in use by the
 * next chunk's PREV_INUSE bit. Freeing anything else corrupts the heap.
 */
extern char *__malloc_sbrk_base;
void *sbrk(ptrdiff_t incr);

static int newlib_chunk_valid(void *p) {
    uintptr_t base = (uintptr_t)__malloc_sbrk_base;
    uintptr_t top = (uintptr_t)sbrk(0);
    uintptr_t up = (uintptr_t)p;
    if (base == (uintptr_t)-1 || up < base + 8 || up >= top || (up & 7))
        return 0;

    uintptr_t chunk = up - 8;
    uint32_t size = *(uint32_t *)(chunk + 4) & ~7u;
    if (size < 16 || chunk + size + 8 > top)
        return 0;

    uint32_t next_size = *(uint32_t *)(chunk + size + 4);
    return next_size & 1; // PREV_INUSE: this chunk is allocated
}

// Logs each distinct caller of a foreign free/realloc once
static void report_foreign(const char *what, void *p, int valid, void *caller) {
    static uintptr_t seen[64];
    static int seen_num = 0;

    uintptr_t bt[HG_BT_DEPTH];
    backtrace_so(bt, caller);
    for (int i = 0; i < seen_num; i++)
        if (seen[i] == bt[0] && valid)
            return;
    if (seen_num < 64)
        seen[seen_num++] = bt[0];

    filelog_printf("HEAP: %s of %s pointer %p\n", what, valid ? "newlib" : "INVALID", p);
    log_bt("called from", bt);
}

static HgHeader *header_of(void *p) {
    HgHeader *h = (HgHeader *)p - 1;
    return h->magic == HG_MAGIC_LIVE ? h : NULL;
}

// Header intact: check word matches and the block lies inside its raw allocation
static int header_sane(HgHeader *h) {
    return h->check == header_check(h) && (uint8_t *)h->raw <= (uint8_t *)h
        && (uint8_t *)h - (uint8_t *)h->raw < 0x100000 && h->size < 0x10000000;
}

#define CALLER __builtin_return_address(0)

static void guarded_free(void *p, void *caller);

void *heapguard_malloc(size_t size) {
    return guarded_alloc(size, 8, CALLER);
}

void *heapguard_memalign(size_t align, size_t size) {
    return guarded_alloc(size, align, CALLER);
}

void *heapguard_calloc(size_t n, size_t size) {
    void *p = guarded_alloc(n * size, 8, CALLER);
    if (p)
        memset(p, 0, n * size);
    return p;
}

void heapguard_free(void *p) {
    guarded_free(p, CALLER);
}

static void guarded_free(void *p, void *caller) {
    if (!p)
        return;

    lock();
    HgHeader *h = header_of(p);
    if (!h) {
        if (((HgHeader *)p - 1)->magic == HG_MAGIC_FREED) {
            report("DOUBLE FREE", (HgHeader *)p - 1, 0, 0);
            unlock();
            return;
        }
        // Not ours: only pass real newlib blocks on
        int valid = newlib_chunk_valid(p);
        report_foreign("free", p, valid, caller);
        unlock();
        if (valid)
            REAL(free)(p);
        return;
    }

    if (!header_sane(h)) {
        // Something wrote right before the block: trusting size/raw would
        // poison or free the wrong memory. Leak it instead.
        if (reports++ < 20) {
            filelog_printf("HEAP: HEADER CORRUPTED (write before the block) at %p: size %u, raw %p\n",
                           p, h->size, h->raw);
            uintptr_t bt[HG_BT_DEPTH];
            backtrace_so(bt, caller);
            log_bt("freed by", bt);
        }
        h->magic = 0;
        unlock();
        return;
    }

    check_canary(h);
    h->magic = HG_MAGIC_FREED;
    backtrace_so(h->free_bt, caller);
    memset(p, HG_POISON, h->size);

    // Quarantine, evicting the oldest blocks if needed
    while (q_count > 0 && (q_count == QUARANTINE_SLOTS || q_bytes + h->size > QUARANTINE_BYTES)) {
        int idx = (q_head - q_count + QUARANTINE_SLOTS) % QUARANTINE_SLOTS;
        HgHeader *old = quarantine[idx];
        check_poison(old);
        q_bytes -= old->size;
        q_count--;
        old->magic = 0;
        REAL(free)(old->raw);
    }
    quarantine[q_head] = h;
    q_head = (q_head + 1) % QUARANTINE_SLOTS;
    q_count++;
    q_bytes += h->size;
    unlock();
}

void *heapguard_realloc(void *p, size_t size) {
    void *caller = CALLER;
    if (!p)
        return guarded_alloc(size, 8, caller);

    lock();
    HgHeader *h = header_of(p);
    unlock();
    if (!h) {
        lock();
        int valid = newlib_chunk_valid(p);
        report_foreign("realloc", p, valid, caller);
        unlock();
        if (!valid)
            return guarded_alloc(size, 8, caller); // can't know the old size, data is lost
        return REAL(realloc)(p, size);
    }

    if (size == 0) {
        guarded_free(p, caller);
        return NULL;
    }

    void *n = guarded_alloc(size, 8, caller);
    if (!n)
        return NULL;
    memcpy(n, p, h->size < size ? h->size : size);
    guarded_free(p, caller);
    return n;
}

/*
 * Heap walk: follows newlib's chunk chain from the sbrk base to the top and
 * reports the first broken chunk with its predecessor, which is usually the
 * block that overflowed into it.
 */
struct _reent;
extern struct _reent *_impure_ptr;
void __malloc_lock(struct _reent *r);
void __malloc_unlock(struct _reent *r);

static void describe_chunk(const char *what, uintptr_t chunk, uint32_t size) {
    filelog_printf("    %s chunk %p, size %u\n", what, (void *)chunk, size);
    HgHeader *h = (HgHeader *)(chunk + 8);
    if (size >= 16 + sizeof(HgHeader) && h->magic != HG_MAGIC_LIVE && h->magic != HG_MAGIC_FREED)
        h = (HgHeader *)(chunk + 16);
    if (size >= 8 + sizeof(HgHeader) && (h->magic == HG_MAGIC_LIVE || h->magic == HG_MAGIC_FREED)) {
        filelog_printf("    -> heapguard block %p (size %u, %s)\n", (void *)(h + 1), h->size,
                       h->magic == HG_MAGIC_LIVE ? "live" : "in quarantine");
        log_bt("allocated by", h->alloc_bt);
        if (h->magic == HG_MAGIC_FREED)
            log_bt("freed by", h->free_bt);
    } else {
        filelog_printf("    -> not a game allocation (newlib, vitaGL, loader...)\n");
    }
}

// Guard header of a live game block stored in this newlib chunk, if any
// (the header sits right at the start or after 8 bytes of alignment padding)
static HgHeader *live_block_in_chunk(uintptr_t chunk, uint32_t size) {
    for (uint32_t off = 8; off <= 16; off += 8) {
        if (size < off + sizeof(HgHeader))
            break;
        HgHeader *h = (HgHeader *)(chunk + off);
        if (h->magic == HG_MAGIC_LIVE && header_sane(h) && (uintptr_t)h->raw == chunk + 8)
            return h;
    }
    return NULL;
}

int heapguard_check_heap() {
    static int reported = 0;
    static HgHeader *overflow_seen[32];
    static int overflow_seen_num = 0;
    if (reported)
        return 0;

    uintptr_t bad = 0, prev = 0, prev2 = 0;
    uint32_t bad_field = 0, prev_size = 0, prev2_size = 0, chunks = 0;

    // Only newlib's lock is held here: taking ours inside would invert the
    // lock order of heapguard_free() and deadlock
    __malloc_lock(_impure_ptr);
    uintptr_t base = (uintptr_t)__malloc_sbrk_base;
    uintptr_t top = (uintptr_t)sbrk(0);
    if (base != (uintptr_t)-1) {
        uintptr_t chunk = (base + 7) & ~7u;
        while (chunk + 8 <= top) {
            uint32_t size = *(uint32_t *)(chunk + 4) & ~7u;
            if (size < 16 || chunk + size > top) {
                bad = chunk;
                bad_field = *(uint32_t *)(chunk + 4);
                break;
            }

            // Live game block overflowing into the next chunk: catch it now
            HgHeader *h = live_block_in_chunk(chunk, size);
            if (h) {
                uint32_t c;
                memcpy(&c, (uint8_t *)(h + 1) + h->size, 4);
                if (c != HG_CANARY) {
                    int seen = 0;
                    for (int i = 0; i < overflow_seen_num; i++)
                        if (overflow_seen[i] == h)
                            seen = 1;
                    if (!seen && overflow_seen_num < 32) {
                        overflow_seen[overflow_seen_num++] = h;
                        report("OVERFLOW (live block)", h, h->size, c);
                    }
                }
            }

            prev2 = prev;
            prev2_size = prev_size;
            prev = chunk;
            prev_size = size;
            chunk += size;
            chunks++;
        }
    }
    __malloc_unlock(_impure_ptr);

    if (!bad)
        return 1;

    reported = 1;
    filelog_printf("HEAP: CORRUPTED chunk chain after %u chunks: chunk %p has size field 0x%08X\n",
                   chunks, (void *)prev, prev ? *(uint32_t *)(prev + 4) : bad_field);
    filelog_printf("    (walk stopped at %p, field 0x%08X)\n", (void *)bad, bad_field);
    if (prev)
        describe_chunk("smashed", prev, prev_size);
    if (prev2)
        describe_chunk("before it (likely overflowed)", prev2, prev2_size);

    lock();
    for (int i = 0; i < q_count; i++) {
        HgHeader *q = quarantine[(q_head - 1 - i + QUARANTINE_SLOTS) % QUARANTINE_SLOTS];
        uintptr_t u = (uintptr_t)(q + 1);
        if (prev + 4 >= u && prev + 4 < u + q->size) {
            filelog_printf("    -> the smashed size field lies INSIDE quarantined block %p (size %u, raw %p)\n",
                           (void *)u, q->size, q->raw);
            log_bt("allocated by", q->alloc_bt);
            log_bt("freed by", q->free_bt);
            break;
        }
    }
    unlock();
    return 0;
}

#ifdef HEAPGUARD_WRAP
void *__wrap_malloc(size_t size) {
    return guarded_alloc(size, 8, CALLER);
}

void *__wrap_calloc(size_t n, size_t size) {
    void *p = guarded_alloc(n * size, 8, CALLER);
    if (p)
        memset(p, 0, n * size);
    return p;
}

void *__wrap_memalign(size_t align, size_t size) {
    return guarded_alloc(size, align, CALLER);
}

void __wrap_free(void *p) {
    guarded_free(p, CALLER);
}

void *__wrap_realloc(void *p, size_t size) {
    return heapguard_realloc(p, size);
}

size_t __wrap_malloc_usable_size(void *p) {
    if (!p)
        return 0;
    lock();
    HgHeader *h = header_of(p);
    unlock();
    return h ? h->size : __real_malloc_usable_size(p);
}
#endif
