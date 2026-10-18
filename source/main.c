#include "utils/waitstats.h"
#include "translation.h"
#include "utils/markers.h"
#include "utils/luaprof.h"
#include "reimpl/fastheap.h"
#include "apk_index.h"
#include "utils/init.h"
#include "utils/glutil.h"
#include "utils/logger.h"

#include <malloc.h>
#include <pthread.h>
#include <stdbool.h>

#include <psp2/apputil.h>
#include <psp2/system_param.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>

#include "reimpl/controls.h"
#include "fmod_audio.h"
#include "utils/filelog.h"
#include "utils/profiler.h"
#include "reimpl/fdcache.h"

int _newlib_heap_size_user = 256 * 1024 * 1024;

#ifdef USE_SCELIBC_IO
int sceLibcHeapSize = 4 * 1024 * 1024;
#endif

so_module so_mod;

#define SCREEN_W 960
#define SCREEN_H 544

// Fake Java objects; the game only stores and passes them back
#define FAKE_CLASS     ((jclass)0x42424242)
#define FAKE_ACTIVITY  ((jobject)0x41435449) // 'ACTI'
#define FAKE_ASSET_MGR ((jobject)0x41534D47) // 'ASMG'

// android.graphics.PixelFormat.RGBA_8888 as converted by SDLSurface.surfaceChanged
#define SDL_PIXELFORMAT_RGBA8888 373694468

// SDL thread stack. Android Java threads get ~1MB, the game is heavy on
// recursion in Lua and Scaleform, so give it more.
#define SDL_THREAD_STACK_SIZE (4 * 1024 * 1024)

static void (*Java_SDLActivity_onNativeKeyDown)(JNIEnv *env, jclass cls, int keycode);
static void (*Java_SDLActivity_onNativeKeyUp)(JNIEnv *env, jclass cls, int keycode);
static void (*Java_SDLActivity_onNativeTouch)(JNIEnv *env, jclass cls, int touch_device, int pointer, int action, float x, float y, float p);

static const char *cq_language() {
    if (translation_game_language())
        return translation_game_language();
    int lang = SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang);

    // Same set as CqActivity.LANGUAGE_MAP
    switch (lang) {
        case SCE_SYSTEM_PARAM_LANG_FRENCH:     return "frFR";
        case SCE_SYSTEM_PARAM_LANG_ITALIAN:    return "itIT";
        case SCE_SYSTEM_PARAM_LANG_GERMAN:     return "deDE";
        case SCE_SYSTEM_PARAM_LANG_SPANISH:    return "esES";
        default:                               return "enUS";
    }
}

// Logs I/O activity every few seconds: tells a slow load from a hang
#ifdef HEAPGUARD
int heapguard_check_heap();
#endif

extern volatile int gl_initialized; // patch.c

static int heartbeat_thread(SceSize args, void *argp) {
    uint32_t last_reads = 0;
    for (int tick = 1;; tick++) {
        sceKernelDelayThread(1000 * 1000);
#ifdef HEAPGUARD
        heapguard_check_heap();
#endif
        if (tick % 5)
            continue;
        // Frame timing over the last 5 seconds (patch.c)
        extern volatile uint32_t frame_count, frame_max_us;
        extern volatile uint64_t frame_work_us, frame_swap_us;
        uint32_t frames = frame_count;
        markers_log(frames, 5 * 1000 * 1000);
        luaprof_log(frames);
        {
            static uint32_t last_allocs, last_frees;
            uint32_t allocs = fastheap_stats.allocs, frees = fastheap_stats.frees;
            uint32_t f = frames ? frames : 1;
            filelog_printf("heartbeat: heap %u allocs + %u frees per frame (%u%% small), main thread %u.%u ms/frame in them, arena %u MB\n",
                           (allocs - last_allocs) / f, (frees - last_frees) / f,
                           allocs ? (unsigned)((uint64_t)fastheap_stats.small * 100 / allocs) : 0,
                           (unsigned)(fastheap_stats.main_us / f / 1000), (unsigned)(fastheap_stats.main_us / f / 100 % 10),
                           (unsigned)(fastheap_arena_used() >> 20));
            last_allocs = allocs;
            last_frees = frees;
            fastheap_stats.main_us = 0;
        }
        if (frames) {
            filelog_printf("heartbeat: %u.%u fps, frame avg %u ms (game %u ms + swap %u ms), worst %u ms\n",
                           frames / 5, (frames * 10 / 5) % 10,
                           (unsigned)((frame_work_us + frame_swap_us) / frames / 1000),
                           (unsigned)(frame_work_us / frames / 1000), (unsigned)(frame_swap_us / frames / 1000),
                           frame_max_us / 1000);
        }
        profiler_log(5 * 1000 * 1000);
        waitstats_log(5 * 1000 * 1000);
        gpustats_log();
        frame_count = 0;
        frame_work_us = 0;
        frame_swap_us = 0;
        frame_max_us = 0;

        uint32_t reads = fdcache_stats.reads;
        uint32_t refills = fdcache_stats.refills;
        filelog_printf("heartbeat: read() %u (+%u), card reads %u (avg %u us, %u direct), cache hits %u, "
                       "game got %u KB / card gave %u KB, index hits %u, lseek() %u, last fd %d pos %lld\n",
                       reads, reads - last_reads, refills,
                       refills ? (unsigned)(fdcache_stats.refill_us / refills) : 0, fdcache_stats.direct,
                       fdcache_stats.hits, (unsigned)(fdcache_stats.bytes / 1024),
                       (unsigned)(fdcache_stats.card_bytes / 1024), apk_index_hits, fdcache_stats.seeks,
                       fdcache_stats.last_fd, (long long)fdcache_stats.last_pos);
        last_reads = reads;

        // Once VRAM/RAM pools run out, vitaGL puts GPU data in the newlib heap.
        // Its pools only exist after vglInit (done on the SDL thread).
        if (!gl_initialized)
            continue;
        extern volatile uint32_t vgl_stat_shaders_compiled, vgl_stat_shaders_cached;
        filelog_printf("heartbeat: shaders compiled %u, loaded from cache %u\n",
                       vgl_stat_shaders_compiled, vgl_stat_shaders_cached);
        filelog_printf("heartbeat: vitaGL free VRAM %u KB, RAM %u KB, PHYCONT %u KB; newlib heap in use %u KB\n",
                       (unsigned)(vglMemFree(VGL_MEM_VRAM) / 1024), (unsigned)(vglMemFree(VGL_MEM_RAM) / 1024),
                       (unsigned)(vglMemFree(VGL_MEM_SLOW) / 1024), (unsigned)(mallinfo().uordblks / 1024));
    }
    return 0;
}

static void *sdl_main_thread(void *arg) {
    // SDLMain.run(): SDL_Android_Init + SDL_main, never returns while the game runs
    profiler_register_thread(sceKernelGetThreadId(), "SDL/game main");
    waitstats_set_main_thread(sceKernelGetThreadId());
    void (*Java_SDLActivity_nativeInit)(JNIEnv *env, jclass cls) =
        (void *)so_symbol(&so_mod, "Java_org_libsdl_app_SDLActivity_nativeInit");

    l_info("SDLActivity.nativeInit()");
    Java_SDLActivity_nativeInit(&jni, FAKE_CLASS);
    l_info("SDLActivity.nativeInit() returned, exiting");
    fmod_audio_stop();
    sceKernelExitProcess(0);
    return NULL;
}

int main() {
    filelog_init();
    SceUID hb = sceKernelCreateThread("heartbeat", heartbeat_thread, 0x10000100 + 20, 16 * 1024, 0, 0, NULL);
    if (hb >= 0)
        sceKernelStartThread(hb, 0, NULL);

    soloader_init_all();
    fmod_audio_init();

    int (*JNI_OnLoad)(void *jvm) = (void *)so_symbol(&so_mod, "JNI_OnLoad");
    l_info("JNI_OnLoad()");
    JNI_OnLoad(&jvm);

    Java_SDLActivity_onNativeKeyDown = (void *)so_symbol(&so_mod, "Java_org_libsdl_app_SDLActivity_onNativeKeyDown");
    Java_SDLActivity_onNativeKeyUp = (void *)so_symbol(&so_mod, "Java_org_libsdl_app_SDLActivity_onNativeKeyUp");
    Java_SDLActivity_onNativeTouch = (void *)so_symbol(&so_mod, "Java_org_libsdl_app_SDLActivity_onNativeTouch");

    void (*Java_CqActivity_dfNativeSetLanguage)(JNIEnv *env, jclass cls, jstring lang) =
        (void *)so_symbol(&so_mod, "Java_com_doublefine_cq_CqActivity_dfNativeSetLanguage");
    void (*Java_CqActivity_dfNativeInit)(JNIEnv *env, jclass cls, jobject activity, jobject asset_manager, jstring obb_path, jstring apk_path) =
        (void *)so_symbol(&so_mod, "Java_com_doublefine_cq_CqActivity_dfNativeInit");
    void (*Java_SDLActivity_nativeSetMaxScreenResolution)(JNIEnv *env, jclass cls, int w, int h) =
        (void *)so_symbol(&so_mod, "Java_org_libsdl_app_SDLActivity_nativeSetMaxScreenResolution");
    void (*Java_CqActivity_setDeviceInfo)(JNIEnv *env, jclass cls, jstring brand, jstring device, jstring manufacturer, jstring model, jstring product, jstring release, jboolean touchscreen) =
        (void *)so_symbol(&so_mod, "Java_com_doublefine_cq_CqActivity_setDeviceInfo");
    void (*Java_SDLActivity_onNativeResize)(JNIEnv *env, jclass cls, int w, int h, int format) =
        (void *)so_symbol(&so_mod, "Java_org_libsdl_app_SDLActivity_onNativeResize");
    void (*Java_SDLActivity_onNativeSurfaceChanged)(JNIEnv *env, jclass cls) =
        (void *)so_symbol(&so_mod, "Java_org_libsdl_app_SDLActivity_onNativeSurfaceChanged");

    // SDLActivity.onCreate(): sFMODAudioDevice.start()
    fmod_audio_start();

    // CqActivity.onCreate()
    const char *lang = cq_language();
    l_info("Language: %s", lang);
    Java_CqActivity_dfNativeSetLanguage(&jni, FAKE_CLASS, jni->NewStringUTF(&jni, lang));

    l_info("CqActivity.dfNativeInit(apk: %s)", APK_PATH);
    // No OBB: the game falls back to reading assets/ant.mp3 from inside the APK
    Java_CqActivity_dfNativeInit(&jni, FAKE_CLASS, FAKE_ACTIVITY, FAKE_ASSET_MGR,
                                 NULL, jni->NewStringUTF(&jni, APK_PATH));

    l_info("dfNativeInit() done");

    Java_SDLActivity_nativeSetMaxScreenResolution(&jni, FAKE_CLASS, SCREEN_W, SCREEN_H);

    // Touchscreen reported as absent so the game keeps its gamepad (OUYA) UI
    Java_CqActivity_setDeviceInfo(&jni, FAKE_CLASS,
                                  jni->NewStringUTF(&jni, "Sony"),
                                  jni->NewStringUTF(&jni, "vita"),
                                  jni->NewStringUTF(&jni, "Sony"),
                                  jni->NewStringUTF(&jni, "PlayStation Vita"),
                                  jni->NewStringUTF(&jni, "vita"),
                                  jni->NewStringUTF(&jni, "4.4.2"),
                                  JNI_FALSE);

    // SDLSurface.surfaceChanged()
    l_info("SDLSurface.surfaceChanged(%dx%d)", SCREEN_W, SCREEN_H);
    Java_SDLActivity_onNativeResize(&jni, FAKE_CLASS, SCREEN_W, SCREEN_H, SDL_PIXELFORMAT_RGBA8888);
    Java_SDLActivity_onNativeSurfaceChanged(&jni, FAKE_CLASS);

    pthread_t sdl_thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, SDL_THREAD_STACK_SIZE);
    pthread_create(&sdl_thread, &attr, sdl_main_thread, NULL);
    pthread_attr_destroy(&attr);

    sceKernelExitDeleteThread(0);
}

/*
 * Input: SDLSurface forwards Android key codes and normalized touches.
 * The OUYA controller reaches the game as plain key events (BUTTON_A, DPAD_*),
 * so the Vita pad is fed through the same path.
 * Controls are polled once per frame from the swap hook in patch.c.
 */

void controls_handler_key(int32_t keycode, ControlsAction action) {
    // The Vita buttons reach the game as an SDL game controller (vita_pad.c);
    // sending them as Android keys too would double every press
    return;
    if (action == CONTROLS_ACTION_DOWN)
        Java_SDLActivity_onNativeKeyDown(&jni, FAKE_CLASS, keycode);
    else if (action == CONTROLS_ACTION_UP)
        Java_SDLActivity_onNativeKeyUp(&jni, FAKE_CLASS, keycode);
}

void controls_handler_touch(int32_t id, float x, float y, ControlsAction action) {
    // MotionEvent: ACTION_DOWN 0, ACTION_UP 1, ACTION_MOVE 2
    int android_action;
    switch (action) {
        case CONTROLS_ACTION_DOWN: android_action = 0; break;
        case CONTROLS_ACTION_UP:   android_action = 1; break;
        default:                   android_action = 2; break;
    }
    Java_SDLActivity_onNativeTouch(&jni, FAKE_CLASS, 0, id, android_action,
                                   x / SCREEN_W, y / SCREEN_H, 1.0f);
}

// Android has no analog path to the game here: the left stick emulates the
// D-pad, like the OUYA controller does in its system menus.
// TODO: check whether SDLGamepad::UpdateDeviceState_Joystick can be fed
//       real analog values instead.
void controls_handler_analog(ControlsStickId which, float x, float y, ControlsAction action) {
    // Sticks are real analog axes now (vita_pad.c)
    return;
    if (which != CONTROLS_STICK_LEFT)
        return;

    static bool up, down, left, right;
    const float threshold = 0.5f;

    bool n_up = y < -threshold, n_down = y > threshold;
    bool n_left = x < -threshold, n_right = x > threshold;

    if (n_up != up)       controls_handler_key(AKEYCODE_DPAD_UP,    n_up    ? CONTROLS_ACTION_DOWN : CONTROLS_ACTION_UP);
    if (n_down != down)   controls_handler_key(AKEYCODE_DPAD_DOWN,  n_down  ? CONTROLS_ACTION_DOWN : CONTROLS_ACTION_UP);
    if (n_left != left)   controls_handler_key(AKEYCODE_DPAD_LEFT,  n_left  ? CONTROLS_ACTION_DOWN : CONTROLS_ACTION_UP);
    if (n_right != right) controls_handler_key(AKEYCODE_DPAD_RIGHT, n_right ? CONTROLS_ACTION_DOWN : CONTROLS_ACTION_UP);

    up = n_up; down = n_down; left = n_left; right = n_right;
}
