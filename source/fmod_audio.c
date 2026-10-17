/*
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  fmod_audio.c
 * @brief Native replacement for org.fmod.FMODAudioDevice.
 *
 * On Android, FMOD Ex does not open the audio device itself: a Java thread
 * (FMODAudioDevice.run) polls `fmodGetInfo` until the mixer is configured,
 * then repeatedly calls `fmodProcess(ByteBuffer)` to mix one DSP block of
 * 16-bit stereo PCM and writes it to an AudioTrack. This file does the same
 * with a Vita thread and sceAudioOut.
 */

#include "fmod_audio.h"

#include <stdlib.h>
#include <string.h>

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#include "utils/init.h"
#include "utils/logger.h"
#include "utils/profiler.h"

#define FMOD_INFO_SAMPLERATE      0
#define FMOD_INFO_DSPBUFFERLENGTH 1
#define FMOD_INFO_DSPNUMBUFFERS   2
#define FMOD_INFO_MIXERRUNNING    3

#define NUMCHANNELS 2

// Fake java.nio.ByteBuffer created with allocateDirect(): FalsoJNI does not
// implement direct buffers, so GetDirectBufferAddress is overridden below.
#define DIRECT_BUFFER_MAGIC 0x44425546 // 'DBUF'

typedef struct {
    uint32_t magic;
    void *address;
    int64_t capacity;
} DirectBuffer;

static int (*fmodGetInfo)(JNIEnv *env, jobject thiz, int type);
static int (*fmodProcess)(JNIEnv *env, jobject thiz, jobject buffer);

static void *(*GetDirectBufferAddress_orig)(JNIEnv *env, jobject buf);
static jlong (*GetDirectBufferCapacity_orig)(JNIEnv *env, jobject buf);

extern struct JNINativeInterface *_jni;

static SceUID audio_thid = -1;
static volatile int running = 0;

// Fake `this` for the FMODAudioDevice instance
static jobject fmod_device = (jobject)0x464D4F44; // 'FMOD'

static void *GetDirectBufferAddress_soloader(JNIEnv *env, jobject buf) {
    DirectBuffer *db = (DirectBuffer *)buf;
    if (db && db->magic == DIRECT_BUFFER_MAGIC)
        return db->address;
    return GetDirectBufferAddress_orig(env, buf);
}

static jlong GetDirectBufferCapacity_soloader(JNIEnv *env, jobject buf) {
    DirectBuffer *db = (DirectBuffer *)buf;
    if (db && db->magic == DIRECT_BUFFER_MAGIC)
        return db->capacity;
    return GetDirectBufferCapacity_orig(env, buf);
}

static int audio_thread(SceSize args, void *argp) {
    int port = -1;
    int samplerate = 0, dsp_len = 0;
    DirectBuffer db = { DIRECT_BUFFER_MAGIC, NULL, 0 };

    // sceAudioOut wants a multiple of 64 frames, FMOD gives dsp_len frames
    // per fmodProcess call. Collect them into port-sized chunks.
    int16_t *chunk = NULL;
    int chunk_frames = 0, chunk_fill = 0;

    while (running) {
        if (port < 0) {
            samplerate = fmodGetInfo(&jni, fmod_device, FMOD_INFO_SAMPLERATE);
            dsp_len = fmodGetInfo(&jni, fmod_device, FMOD_INFO_DSPBUFFERLENGTH);
            int dsp_num = fmodGetInfo(&jni, fmod_device, FMOD_INFO_DSPNUMBUFFERS);

            if (samplerate <= 0 || dsp_len <= 0 || dsp_num <= 0) {
                sceKernelDelayThread(100 * 1000);
                continue;
            }

            chunk_frames = (dsp_len % 64 == 0) ? dsp_len : 1024;
            port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, chunk_frames,
                                       samplerate, SCE_AUDIO_OUT_MODE_STEREO);
            if (port < 0) {
                l_error("FMOD: sceAudioOutOpenPort(%d frames, %d Hz) failed: 0x%x",
                        chunk_frames, samplerate, port);
                running = 0;
                break;
            }

            int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
            sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);

            db.capacity = dsp_len * 2 * NUMCHANNELS;
            db.address = malloc(db.capacity);
            chunk = malloc(chunk_frames * 2 * NUMCHANNELS);
            chunk_fill = 0;

            l_success("FMOD: audio out %d Hz, dsp block %d x %d, port chunk %d",
                      samplerate, dsp_len, dsp_num, chunk_frames);
        } else if (fmodGetInfo(&jni, fmod_device, FMOD_INFO_MIXERRUNNING) == 1) {
            fmodProcess(&jni, fmod_device, (jobject)&db);

            int16_t *src = db.address;
            int left = dsp_len;
            while (left > 0) {
                int n = chunk_frames - chunk_fill;
                if (n > left)
                    n = left;
                memcpy(chunk + chunk_fill * NUMCHANNELS, src, n * 2 * NUMCHANNELS);
                chunk_fill += n;
                src += n * NUMCHANNELS;
                left -= n;
                if (chunk_fill == chunk_frames) {
                    sceAudioOutOutput(port, chunk); // blocks, paces the loop
                    chunk_fill = 0;
                }
            }
        } else {
            sceKernelDelayThread(10 * 1000);
        }
    }

    if (port >= 0)
        sceAudioOutReleasePort(port);
    free(db.address);
    free(chunk);
    return sceKernelExitDeleteThread(0);
}

void fmod_audio_init() {
    fmodGetInfo = (void *)so_symbol(&fmodex_mod, "Java_org_fmod_FMODAudioDevice_fmodGetInfo");
    fmodProcess = (void *)so_symbol(&fmodex_mod, "Java_org_fmod_FMODAudioDevice_fmodProcess");
    if (!fmodGetInfo || !fmodProcess)
        l_fatal("FMOD: AudioDevice JNI exports not found in libfmodex.so");

    GetDirectBufferAddress_orig = _jni->GetDirectBufferAddress;
    GetDirectBufferCapacity_orig = _jni->GetDirectBufferCapacity;
    _jni->GetDirectBufferAddress = GetDirectBufferAddress_soloader;
    _jni->GetDirectBufferCapacity = GetDirectBufferCapacity_soloader;
}

void fmod_audio_start() {
    if (running)
        return;
    running = 1;
    // Java sets Thread.MAX_PRIORITY for this one
    audio_thid = sceKernelCreateThread("fmod_audio", audio_thread, 0x10000100 - 10,
                                       64 * 1024, 0, 0, NULL);
    if (audio_thid < 0) {
        l_error("FMOD: can't create audio thread: 0x%x", audio_thid);
        running = 0;
        return;
    }
    profiler_register_thread(audio_thid, "fmod mixer");
    sceKernelStartThread(audio_thid, 0, NULL);
}

void fmod_audio_stop() {
    running = 0;
    if (audio_thid >= 0)
        sceKernelWaitThreadEnd(audio_thid, NULL, NULL);
    audio_thid = -1;
}
