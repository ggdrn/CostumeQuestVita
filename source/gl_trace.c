/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  gl_trace.c
 * @brief Debug wrappers telling which threads call into vitaGL.
 *
 * vitaGL is not thread safe, while the game has a separate render thread
 * (SceneGraph) and may upload resources from others. These wrappers log the
 * first GL call of every thread and any call made while another thread is
 * still inside GL.
 */

#include <stdatomic.h>
#include <string.h>

#include <psp2/kernel/threadmgr.h>
#include <vitaGL.h>

#include "utils/filelog.h"

static atomic_int gl_active_thread = 0; // thread currently inside a GL call
static atomic_int concurrent_reports = 0;

static void thread_name(SceUID thid, char *out, size_t size) {
    SceKernelThreadInfo info;
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(thid, &info) >= 0)
        strncpy(out, info.name, size - 1);
    else
        strncpy(out, "?", size - 1);
    out[size - 1] = '\0';
}

// First call of each GL function per thread, with the calling game function
#define MAX_SEEN 256
static struct { SceUID thid; const char *fn; } seen[MAX_SEEN];
static atomic_int seen_num = 0;

static void gl_enter(const char *fn, void *caller) {
    SceUID thid = sceKernelGetThreadId();

    int n = atomic_load(&seen_num);
    int known = 0;
    for (int i = 0; i < n; i++) {
        if (seen[i].thid == thid && seen[i].fn == fn) {
            known = 1;
            break;
        }
    }
    if (!known && n < MAX_SEEN) {
        int idx = atomic_fetch_add(&seen_num, 1);
        seen[idx].thid = thid;
        seen[idx].fn = fn;

        int new_thread = 1;
        for (int i = 0; i < idx; i++)
            if (seen[i].thid == thid)
                new_thread = 0;
        int threads = 0;
        for (int i = 0; i <= idx; i++) {
            int first = 1;
            for (int j = 0; j < i; j++)
                if (seen[j].thid == seen[i].thid)
                    first = 0;
            threads += first;
        }

        char name[32], where[160];
        thread_name(thid, name, sizeof(name));
        filelog_describe_addr((uintptr_t)caller & ~1, where, sizeof(where));
        filelog_printf("GL: thread 0x%08X \"%s\" first %s from %s%s\n", thid, name, fn, where,
                       (new_thread && threads > 1) ? "  <-- another GL thread!" : "");
    }

    int expected = 0;
    if (!atomic_compare_exchange_strong(&gl_active_thread, &expected, thid) && expected != thid) {
        if (atomic_fetch_add(&concurrent_reports, 1) < 20) {
            char a[32], b[32];
            thread_name(thid, a, sizeof(a));
            thread_name(expected, b, sizeof(b));
            filelog_printf("GL: CONCURRENT call %s from \"%s\" while \"%s\" is inside GL\n", fn, a, b);
        }
    }
}

static void gl_leave() {
    int self = sceKernelGetThreadId();
    atomic_compare_exchange_strong(&gl_active_thread, &self, 0);
}

#define TRACE_VOID(name, params, args) \
    void name##_trace params { gl_enter(#name, __builtin_return_address(0)); name args; gl_leave(); }

#define TRACE_RET(type, name, params, args) \
    type name##_trace params { gl_enter(#name, __builtin_return_address(0)); type r = name args; gl_leave(); return r; }

TRACE_VOID(glTexImage2D, (GLenum t, GLint l, GLint i, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void *p), (t, l, i, w, h, b, f, ty, p))
TRACE_VOID(glCompressedTexImage2D, (GLenum t, GLint l, GLenum i, GLsizei w, GLsizei h, GLint b, GLsizei s, const void *p), (t, l, i, w, h, b, s, p))
TRACE_VOID(glTexSubImage2D, (GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, const void *p), (t, l, x, y, w, h, f, ty, p))
TRACE_VOID(glBufferData, (GLenum t, GLsizeiptr s, const void *d, GLenum u), (t, s, d, u))
TRACE_VOID(glBufferSubData, (GLenum t, GLintptr o, GLsizeiptr s, const void *d), (t, o, s, d))
TRACE_VOID(glDrawElements, (GLenum m, GLsizei c, GLenum t, const void *i), (m, c, t, i))
TRACE_VOID(glDrawArrays, (GLenum m, GLint f, GLsizei c), (m, f, c))
TRACE_VOID(glUseProgram, (GLuint p), (p))
TRACE_VOID(glLinkProgram, (GLuint p), (p))
TRACE_VOID(glGenTextures, (GLsizei n, GLuint *t), (n, t))
TRACE_VOID(glDeleteTextures, (GLsizei n, const GLuint *t), (n, t))
TRACE_VOID(glBindFramebuffer, (GLenum t, GLuint f), (t, f))
TRACE_VOID(glClear, (GLbitfield m), (m))
TRACE_VOID(glBindTexture, (GLenum t, GLuint tex), (t, tex))
TRACE_VOID(glGenBuffers, (GLsizei n, GLuint *b), (n, b))
TRACE_VOID(glDeleteBuffers, (GLsizei n, const GLuint *b), (n, b))
TRACE_RET(GLuint, glCreateProgram, (void), ())
TRACE_RET(GLuint, glCreateShader, (GLenum t), (t))
