/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2021-2022 Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/init.h"

#include "utils/dialog.h"
#include "utils/glutil.h"
#include "utils/logger.h"
#include "utils/utils.h"
#include "utils/settings.h"
#include "reimpl/fastheap.h"
#include "apk_index.h"

#include <reimpl/controls.h>

#include <string.h>

#include <psp2/appmgr.h>
#include <psp2/io/stat.h>
#include <psp2/apputil.h>
#include <psp2/kernel/clib.h>
#include <psp2/power.h>

#include <falso_jni/FalsoJNI.h>
#include <so_util/so_util.h>
#include <fios/fios.h>

#include "apk_extract.h"
#include "utils/filelog.h"

so_module fmodex_mod;
so_module fmodevent_mod;

typedef struct {
    so_module *mod;
    const char *path;
    const char *apk_entry; // where to find it inside the APK
    uintptr_t load_addr;   // so_util places a 64KB patch arena right below this
} SoEntry;

// Dependency order: libfmodevent needs libfmodex, libCq needs both.
// Sizes (text + bss): fmodex ~1.1MB (debug 6MB), fmodevent ~0.4MB (debug
// 3.6MB), libCq ~12.5MB; everything has to stay below the kuser atomic block
// at 0x9A000000.
static const SoEntry so_entries[] = {
    // The debug FMOD libraries are bigger (5.6MB / 3.4MB), hence the gaps
    { &fmodex_mod,    SO_FMODEX_PATH,    "lib/armeabi-v7a/libfmodex" FMOD_SUFFIX ".so",    0x98010000 },
    { &fmodevent_mod, SO_FMODEVENT_PATH, "lib/armeabi-v7a/libfmodevent" FMOD_SUFFIX ".so", 0x98810000 },
    { &so_mod,        SO_PATH,           "lib/armeabi-v7a/libCq.so",        0x98C10000 },
};
#define SO_ENTRIES_NUM (sizeof(so_entries) / sizeof(so_entries[0]))

void soloader_init_all() {
	// Launch `app0:configurator.bin` on `-config` init param
    sceAppUtilInit(&(SceAppUtilInitParam){}, &(SceAppUtilBootParam){});
    SceAppUtilAppEventParam eventParam;
    sceClibMemset(&eventParam, 0, sizeof(SceAppUtilAppEventParam));
    sceAppUtilReceiveAppEvent(&eventParam);
    if (eventParam.type == 0x05) {
        char buffer[2048];
        sceAppUtilAppEventParseLiveArea(&eventParam, buffer);
        if (strstr(buffer, "-config"))
            sceAppMgrLoadExec("app0:/configurator.bin", NULL, NULL);
    }

    // Set default overclock values
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

#ifdef USE_SCELIBC_IO
    if (fios_init(DATA_PATH) == 0)
        l_success("FIOS initialized.");
#endif

    if (!module_loaded("kubridge")) {
        l_fatal("kubridge is not loaded.");
        fatal_error("Error: kubridge.skprx is not installed.");
    }
    l_success("kubridge check passed.");

    filelog_install_crash_handler();

    if (!file_exists(APK_PATH)) {
        fatal_error("Game data not found. Please copy the original APK "
                    "to %s.", APK_PATH);
    }

    // First boot: unpack the native libraries from the APK
    for (int i = 0; i < SO_ENTRIES_NUM; i++) {
        if (file_exists(so_entries[i].path))
            continue;

        l_info("Extracting %s from the APK...", so_entries[i].apk_entry);
        ApkResult res = apk_extract_file(APK_PATH, so_entries[i].apk_entry, so_entries[i].path);
        if (res != APK_OK) {
            l_fatal("Extracting %s failed: %s", so_entries[i].apk_entry, apk_result_str(res));
            fatal_error("Error: could not extract %s from %s: %s.",
                        so_entries[i].apk_entry, APK_PATH, apk_result_str(res));
        }
        l_success("Extracted %s.", so_entries[i].path);
    }

    // All modules have to be loaded before resolving, so that so_util can
    // link imports between them through DT_NEEDED.
    for (int i = 0; i < SO_ENTRIES_NUM; i++) {
        if (so_file_load(so_entries[i].mod, so_entries[i].path, so_entries[i].load_addr) < 0) {
            l_fatal("SO could not be loaded: %s", so_entries[i].path);
            fatal_error("Error: could not load %s.", so_entries[i].path);
        }
    }
    l_success("SOs loaded.");
    for (int i = 0; i < SO_ENTRIES_NUM; i++)
        filelog_printf("Loaded %s at 0x%08X (text 0x%08X, size 0x%X)\n", so_entries[i].path,
                       so_entries[i].mod->load_addr, so_entries[i].mod->text_base, so_entries[i].mod->text_size);

    settings_load();
    fastheap_init();
    apk_index_init(APK_PATH, DATA_PATH "apk_index.bin");
    sceIoMkdir(DATA_PATH "save", 0777); // the game's _write folder (see reimpl/io.c)
    l_success("Settings loaded.");

    for (int i = 0; i < SO_ENTRIES_NUM; i++) {
        so_relocate(so_entries[i].mod);
        resolve_imports(so_entries[i].mod);
    }
    l_success("SOs relocated, imports resolved.");

    so_patch();
    l_success("SOs patched.");

    for (int i = 0; i < SO_ENTRIES_NUM; i++)
        so_flush_caches(so_entries[i].mod);
    l_success("SO caches flushed.");

    for (int i = 0; i < SO_ENTRIES_NUM; i++)
        so_initialize(so_entries[i].mod);
    l_success("SOs initialized.");

#ifdef FMOD_DEBUG
    // FMOD Ex debug levels: ERROR 0x2 | WARNING 0x4 | HINT 0x8 (LOG 0x1 floods)
    int (*FMOD_Debug_SetLevel)(unsigned int level) = (void *)so_symbol(&fmodex_mod, "FMOD_Debug_SetLevel");
    if (FMOD_Debug_SetLevel) {
        FMOD_Debug_SetLevel(0x2 | 0x4 | 0x8);
        filelog_printf("FMOD debug libraries loaded, logging errors/warnings/hints\n");
    }
#endif

    gl_preload();
    l_success("OpenGL preloaded.");

    jni_init();
    l_success("FalsoJNI initialized.");

#ifndef NDK_PORT
    controls_init();
    l_success("Controls initialized.");
#endif
}
