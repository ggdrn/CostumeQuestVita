/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  filelog.c
 * @brief Log file in the game data folder, plus a crash handler writing
 *        the faulting context to it.
 */

#include "utils/filelog.h"
#include "utils/log_queue.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>

#include <kubridge.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <so_util/so_util.h>

#include "utils/init.h"

#define LOG_PATH      DATA_PATH "debug.log"
#define LOG_PREV_PATH DATA_PATH "debug.prev.log"

static SceUID log_fd = -1;
static SceKernelLwMutexWork log_mutex;
static SceKernelLwMutexWork log_io_mutex;
static SceUID log_writer = -1;
static LogQueue pending;
static atomic_bool log_emergency = ATOMIC_VAR_INIT(false);
static atomic_bool log_ready = ATOMIC_VAR_INIT(false);

static void write_all(const void *bytes, size_t len) {
    const uint8_t *p = bytes;
    while (len) {
        int n = sceIoWrite(log_fd, p, len);
        if (n <= 0) break;
        p += n;
        len -= n;
    }
}

void filelog_flush(void) {
    if (!atomic_load(&log_ready) || atomic_load(&log_emergency)) return;
    uint8_t batch[8192];
    // Only the writer (or an explicit shutdown/crash flush) waits for card IO.
    sceKernelLockLwMutex(&log_io_mutex, 1, NULL);
    for (int i = 0; i < LOG_QUEUE_CAPACITY / sizeof(batch); i++) {
        if (atomic_load(&log_emergency)) break;
        sceKernelLockLwMutex(&log_mutex, 1, NULL);
        size_t len = log_queue_take(&pending, batch, sizeof(batch));
        uint32_t dropped = !pending.count ? pending.dropped : 0;
        if (!pending.count) pending.dropped = 0;
        sceKernelUnlockLwMutex(&log_mutex, 1);
        if (len) write_all(batch, len);
        if (dropped) {
            char message[96];
            int n = sceClibSnprintf(message, sizeof(message), "[log] %u diagnostic lines omitted: queue full\n", dropped);
            write_all(message, n);
        }
        if (len < sizeof(batch)) break;
    }
    sceKernelUnlockLwMutex(&log_io_mutex, 1);
}

static int log_writer_thread(SceSize args, void *argp) {
    for (;;) {
        sceKernelDelayThread(20000);
        filelog_flush();
    }
    return 0;
}

void filelog_init() {
    if (atomic_load(&log_ready))
        return;

    sceIoMkdir(DATA_PATH, 0777);
    sceIoRemove(LOG_PREV_PATH);
    sceIoRename(LOG_PATH, LOG_PREV_PATH);

    log_fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (log_fd < 0) {
        sceClibPrintf("filelog: can't open %s: 0x%x\n", LOG_PATH, log_fd);
        return;
    }

    // Recursive: the crash handler may log while the crashed thread holds it
    sceKernelCreateLwMutex(&log_mutex, "filelog", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
    sceKernelCreateLwMutex(&log_io_mutex, "filelog IO", SCE_KERNEL_MUTEX_ATTR_RECURSIVE, 0, NULL);
    atomic_store(&log_ready, true);
    log_writer = sceKernelCreateThread("log writer", log_writer_thread, 0x10000100 + 30,
                                      32 * 1024, 0, 0, NULL);
    if (log_writer >= 0 && sceKernelStartThread(log_writer, 0, NULL) < 0) {
        sceKernelDeleteThread(log_writer);
        log_writer = -1;
    }

    filelog_printf("=== Costume Quest Vita " CQ_PORT_VERSION ", built " __DATE__ " " __TIME__ " ===\n");
    filelog_printf("filelog: %s\n", log_writer >= 0 ? "async writer, 64 KB queue" : "synchronous fallback (writer unavailable)");
}

void filelog_write(const char *str) {
    if (!atomic_load(&log_ready))
        return;

    // Strip ANSI color codes coming from the console logger
    char line[2048];
    size_t len = 0;
    for (const char *p = str; *p && len < sizeof(line) - 1; p++) {
        if (*p == '\x1B') {
            while (*p && *p != 'm')
                p++;
            if (!*p)
                break;
            continue;
        }
        line[len++] = *p;
    }

    // Timestamp in ms since the process start, to see where it hangs
    char prefix[24];
    int prefix_len = sceClibSnprintf(prefix, sizeof(prefix), "[%8u] ",
                                     (unsigned)(sceKernelGetProcessTimeWide() / 1000));

    char record[sizeof(prefix) + sizeof(line) + 1];
    memcpy(record, prefix, prefix_len);
    memcpy(record + prefix_len, line, len);
    size_t total = prefix_len + len;
    if (len == 0 || line[len - 1] != '\n') record[total++] = '\n';
    if (atomic_load(&log_emergency)) {
        // A fault may have interrupted either logging mutex's owner. Never
        // wait for that owner to resume to preserve the crash report.
        write_all(record, total);
        return;
    }
    sceKernelLockLwMutex(&log_mutex, 1, NULL);
    log_queue_append(&pending, record, total);
    sceKernelUnlockLwMutex(&log_mutex, 1);
    if (log_writer < 0) filelog_flush(); // Thread creation failure: preserve logs.
}

void filelog_printf(const char *fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    sceClibVsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

#ifdef DEBUG_SOLOADER
    sceClibPrintf("%s", buf);
#endif
    filelog_write(buf);
}

/*
 * Crash handler
 */

// Describes `addr` as "libCq.so+0x1234 (symbol+0x10)" when it is in a loaded .so
void filelog_describe_addr(uintptr_t addr, char *out, size_t out_size) {
    so_module *mods[] = { &so_mod, &fmodevent_mod, &fmodex_mod };

    for (int i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
        so_module *m = mods[i];
        if (!m->text_base || addr < m->text_base || addr >= m->text_base + m->text_size)
            continue;

        // Nearest exported symbol below the address
        const char *best = NULL;
        uintptr_t best_addr = 0;
        for (uint32_t s = 0; s < m->num_dynsym; s++) {
            Elf32_Sym *sym = &m->dynsym[s];
            if (sym->st_shndx == SHN_UNDEF || ELF32_ST_TYPE(sym->st_info) != STT_FUNC)
                continue;
            uintptr_t sa = (m->load_addr + sym->st_value) & ~1;
            if (sa <= addr && sa > best_addr) {
                best_addr = sa;
                best = m->dynstr + sym->st_name;
            }
        }

        if (best)
            sceClibSnprintf(out, out_size, "%s+0x%x (%s+0x%x)", m->soname,
                            addr - m->load_addr, best, addr - best_addr);
        else
            sceClibSnprintf(out, out_size, "%s+0x%x", m->soname, addr - m->load_addr);
        return;
    }

    extern char __executable_start[], __etext[];
    if (addr >= (uintptr_t)__executable_start && addr < (uintptr_t)__etext)
        sceClibSnprintf(out, out_size, "eboot 0x%08X", addr);
    else
        sceClibSnprintf(out, out_size, "eboot or system");
}

static const char *exception_name(SceUInt32 type) {
    switch (type) {
        case KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT:            return "Data abort";
        case KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT:        return "Prefetch abort";
        case KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION: return "Undefined instruction";
        default:                                             return "Unknown exception";
    }
}

static void crash_handler(KuKernelExceptionContext *ctx) {
    // Returning would re-run the faulting instruction and land here again
    static atomic_bool reported = ATOMIC_VAR_INIT(false);
    if (atomic_exchange(&reported, true))
        sceKernelExitProcess(0);

    char where[256];
    SceKernelThreadInfo info;
    info.size = sizeof(info);
    sceKernelGetThreadInfo(sceKernelGetThreadId(), &info);

    atomic_store(&log_emergency, true);
    filelog_printf("\n!!! CRASH: %s in thread \"%s\" !!!\n", exception_name(ctx->exceptionType), info.name);
    filelog_printf("FAR (accessed address): 0x%08X  FSR: 0x%08X\n", ctx->FAR, ctx->FSR);

    filelog_describe_addr(ctx->pc, where, sizeof(where));
    filelog_printf("PC: 0x%08X  %s\n", ctx->pc, where);
    filelog_describe_addr(ctx->lr, where, sizeof(where));
    filelog_printf("LR: 0x%08X  %s\n", ctx->lr, where);

    filelog_printf("R0 %08X  R1 %08X  R2 %08X  R3 %08X\n", ctx->r0, ctx->r1, ctx->r2, ctx->r3);
    filelog_printf("R4 %08X  R5 %08X  R6 %08X  R7 %08X\n", ctx->r4, ctx->r5, ctx->r6, ctx->r7);
    filelog_printf("R8 %08X  R9 %08X  R10 %08X R11 %08X\n", ctx->r8, ctx->r9, ctx->r10, ctx->r11);
    filelog_printf("R12 %08X SP %08X  SPSR %08X\n", ctx->r12, ctx->sp, ctx->SPSR);

    // Return addresses left on the stack, a poor man's backtrace
    filelog_printf("Stack (code addresses only):\n");
    uint32_t *sp = (uint32_t *)ctx->sp;
    int found = 0;
    for (int i = 0; i < 512 && found < 24; i++) {
        uint32_t v = sp[i];
        filelog_describe_addr(v & ~1, where, sizeof(where));
        if (strcmp(where, "eboot or system") != 0) {
            filelog_printf("  [SP+0x%03X] 0x%08X  %s\n", i * 4, v, where);
            found++;
        }
    }

    filelog_printf("Load addresses: libfmodex.so 0x%08X, libfmodevent.so 0x%08X, libCq.so 0x%08X\n",
                   fmodex_mod.load_addr, fmodevent_mod.load_addr, so_mod.load_addr);
    filelog_printf("=== END OF CRASH REPORT ===\n");
    sceKernelExitProcess(0);
}

void filelog_install_crash_handler() {
    const SceUInt32 types[] = {
        KU_KERNEL_EXCEPTION_TYPE_DATA_ABORT,
        KU_KERNEL_EXCEPTION_TYPE_PREFETCH_ABORT,
        KU_KERNEL_EXCEPTION_TYPE_UNDEFINED_INSTRUCTION,
    };
    for (int i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        int ret = kuKernelRegisterExceptionHandler(types[i], crash_handler, NULL, NULL);
        if (ret < 0)
            filelog_printf("filelog: can't register %s handler: 0x%x\n", exception_name(types[i]), ret);
    }
}
