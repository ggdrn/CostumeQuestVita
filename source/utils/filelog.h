/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  filelog.h
 * @brief Log file in the game data folder, plus a crash handler writing
 *        the faulting context to it.
 *
 * DATA_PATH "debug.log" holds the current run, the previous one is kept as
 * DATA_PATH "debug.prev.log". A bounded queue keeps card writes on a background thread.
 * Normal shutdown drains pending diagnostics; crash reports bypass the queue.
 */

#ifndef SOLOADER_FILELOG_H
#define SOLOADER_FILELOG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void filelog_init();
void filelog_flush(void);

// Appends an already formatted string (ANSI color codes are stripped)
void filelog_write(const char *str);

// printf-style: queues the file record; debug builds also print to console
void filelog_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Describes a code address as "libCq.so+0x1234 (symbol+0x10)"
void filelog_describe_addr(uintptr_t addr, char *out, size_t out_size);

// Registers kubridge exception handlers that dump the crash to the log
void filelog_install_crash_handler();

#ifdef __cplusplus
};
#endif

#endif // SOLOADER_FILELOG_H
