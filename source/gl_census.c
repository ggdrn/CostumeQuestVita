/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  gl_census.c
 * @brief Debug: logs each distinct texture upload format and vertex
 *        attribute layout the game uses, once, with the calling function.
 *
 * Cheap enough to stay on in Debug builds; used to find which texture
 * formats / vertex layouts vitaGL handles wrong.
 */

#include <string.h>

#include <psp2/kernel/threadmgr.h>
#include <vitaGL.h>

#include "utils/filelog.h"

#define MAX_KEYS 512

static uint32_t keys[MAX_KEYS][4];
static int keys_num = 0;
static SceKernelLwMutexWork lock;
static volatile int lock_ready = 0;

// Returns 1 the first time this (kind, a, b, c) combination is seen
static int first_time(uint32_t kind, uint32_t a, uint32_t b, uint32_t c) {
    if (!lock_ready) {
        sceKernelCreateLwMutex(&lock, "gl_census", 0, 0, NULL);
        lock_ready = 1;
    }
    sceKernelLockLwMutex(&lock, 1, NULL);
    int found = 0;
    for (int i = 0; i < keys_num; i++) {
        if (keys[i][0] == kind && keys[i][1] == a && keys[i][2] == b && keys[i][3] == c) {
            found = 1;
            break;
        }
    }
    if (!found && keys_num >= MAX_KEYS) {
        // Table full: stop reporting instead of logging every call
        sceKernelUnlockLwMutex(&lock, 1);
        return 0;
    }
    if (!found) {
        keys[keys_num][0] = kind;
        keys[keys_num][1] = a;
        keys[keys_num][2] = b;
        keys[keys_num][3] = c;
        keys_num++;
    }
    sceKernelUnlockLwMutex(&lock, 1);
    return !found;
}

static void log_caller(void *caller) {
    char where[160];
    filelog_describe_addr((uintptr_t)caller & ~1, where, sizeof(where));
    filelog_printf("    from %s\n", where);
}

void glTexImage2D_census(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *data) {
    if (first_time(1, internalFormat, format, type)) {
        filelog_printf("CENSUS: glTexImage2D internal 0x%X format 0x%X type 0x%X (first %dx%d, level %d, data %s)\n",
                       internalFormat, format, type, width, height, level, data ? "yes" : "no");
        log_caller(__builtin_return_address(0));
    }
    glTexImage2D(target, level, internalFormat, width, height, border, format, type, data);
}

void glTexSubImage2D_census(GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *data) {
    if (first_time(2, format, type, 0)) {
        filelog_printf("CENSUS: glTexSubImage2D format 0x%X type 0x%X (first %dx%d at %d,%d)\n",
                       format, type, width, height, x, y);
        log_caller(__builtin_return_address(0));
    }
    glTexSubImage2D(target, level, x, y, width, height, format, type, data);
}

void glCompressedTexImage2D_census(GLenum target, GLint level, GLenum internalFormat, GLsizei width, GLsizei height, GLint border, GLsizei size, const void *data) {
    if (first_time(3, internalFormat, 0, 0)) {
        filelog_printf("CENSUS: glCompressedTexImage2D internal 0x%X (first %dx%d, %d bytes)\n",
                       internalFormat, width, height, size);
        log_caller(__builtin_return_address(0));
    }
    glCompressedTexImage2D(target, level, internalFormat, width, height, border, size, data);
}

void glVertexAttribPointer_census(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer) {
    // Offsets matter for GXM alignment rules; only the low bits are kept so
    // client-side pointers don't flood the log
    uint32_t off = (uint32_t)(uintptr_t)pointer & 0xFFF;
    if (first_time(4, (index << 16) | (size << 8) | normalized, type, (stride << 12) | off)) {
        filelog_printf("CENSUS: glVertexAttribPointer index %u size %d type 0x%X normalized %d stride %d offset %p\n",
                       index, size, type, normalized, stride, pointer);
    }
    glVertexAttribPointer(index, size, type, normalized, stride, pointer);
}
