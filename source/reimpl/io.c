/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/io.h"
#include "reimpl/fdcache.h"

#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <stdarg.h>
#include <psp2/kernel/threadmgr.h>

#ifdef USE_SCELIBC_IO
#include <libc_bridge/libc_bridge.h>
#endif

#include "utils/filelog.h"
#include "utils/logger.h"
#include "utils/utils.h"

// Includes the following inline utilities:
// int oflags_musl_to_newlib(int flags);
// dirent64_bionic * dirent_newlib_to_bionic(struct dirent* dirent_newlib);
// void stat_newlib_to_bionic(struct stat * src, stat64_bionic * dst);
#include "reimpl/bits/_struct_converters.c"

/*
 * Saves: the game writes saves and options to <resource dir>/_write
 * (DFApp::InitFileSystem, GetAppResourceDir() = /sdcard/dfp/cq-droid).
 * Only that folder is redirected; the rest of /sdcard stays missing, as the
 * game's loose-file / HD-cache lookups expect.
 */
#define ANDROID_SAVE_DIR "/sdcard/dfp/cq-droid/_write"
#define VITA_SAVE_DIR DATA_PATH "save"

// Files the port writes for the game (controls_ui.c), found through its
// loose-file lookup: /sdcard/dfp/cq-droid/Data/UI/Vita_... only
#define ANDROID_PORT_UI "/sdcard/dfp/cq-droid/Data/UI/Vita_"
#define VITA_PORT_UI    DATA_PATH "Data/UI/Vita_"

const char *io_path(const char *path, char *buf, size_t size) {
    static int logged = 0;
    if (path && strncasecmp(path, ANDROID_PORT_UI, sizeof(ANDROID_PORT_UI) - 1) == 0) {
        snprintf(buf, size, "%s%s", VITA_PORT_UI, path + sizeof(ANDROID_PORT_UI) - 1);
        static int ui_logged = 0;
        if (ui_logged++ < 10)
            filelog_printf("port ui path: %s -> %s", path, buf);
        return buf;
    }
    size_t n = sizeof(ANDROID_SAVE_DIR) - 1;
    if (!path || strncmp(path, ANDROID_SAVE_DIR, n) != 0 || (path[n] != '/' && path[n] != 0))
        return path;
    snprintf(buf, size, "%s%s", VITA_SAVE_DIR, path + n);
    if (logged < 30) {
        logged++;
        filelog_printf("save path: %s -> %s", path, buf);
    }
    return buf;
}

#define IO_PATH(p) char p##_buf[512]; p = io_path(p, p##_buf, sizeof(p##_buf))

FILE * fopen_soloader(const char * filename, const char * mode) {
    if (strcmp(filename, "/proc/cpuinfo") == 0) {
        return fopen_soloader("app0:/cpuinfo", mode);
    } else if (strcmp(filename, "/proc/meminfo") == 0) {
        return fopen_soloader("app0:/meminfo", mode);
    }

    IO_PATH(filename);
#ifdef USE_SCELIBC_IO
    FILE* ret = sceLibcBridge_fopen(filename, mode);
#else
    FILE* ret = fopen(filename, mode);
#endif

    if (ret)
        l_debug("fopen(%s, %s): %p", filename, mode, ret);
    else
        l_warn("fopen(%s, %s): %p", filename, mode, ret);

    return ret;
}

int open_soloader(const char * path, int oflag, ...) {
    if (strcmp(path, "/proc/cpuinfo") == 0) {
        return open_soloader("app0:/cpuinfo", oflag);
    } else if (strcmp(path, "/proc/meminfo") == 0) {
        return open_soloader("app0:/meminfo", oflag);
    } else if (strcmp(path, "/dev/urandom") == 0) {
        return open_soloader("app0:/urandom", oflag);
    }

    IO_PATH(path);
    mode_t mode = 0666;
    if (((oflag & BIONIC_O_CREAT) == BIONIC_O_CREAT) ||
        ((oflag & BIONIC_O_TMPFILE) == BIONIC_O_TMPFILE)) {
        va_list args;
        va_start(args, oflag);
        mode = (mode_t)(va_arg(args, int));
        va_end(args);
    }

    int read_only = (oflag & 3) == 0; // O_RDONLY, bionic access mode bits
    oflag = oflags_bionic_to_newlib(oflag);
    int ret = open(path, oflag, mode);
    if (ret >= 0 && read_only) {
        fdcache_attach(ret);
        if (strcmp(path, APK_PATH) == 0)
            fdcache_mark_apk(ret);
    }
    if (ret >= 0)
        l_debug("open(%s, %x): %i", path, oflag, ret);
    else
        l_warn("open(%s, %x): %i", path, oflag, ret);
    return ret;
}

int fstat_soloader(int fd, stat64_bionic * buf) {
    struct stat st;
    int res = fstat(fd, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("fstat(%i): %i", fd, res);
    return res;
}

int stat_soloader(const char * path, stat64_bionic * buf) {
    if (strcmp(path, "/system/lib/libOpenSLES.so") == 0) {
        l_debug("stat(%s): returning 0 in case this is a check for OpenSLES support", path);
        return 0;
    }

    IO_PATH(path);
    struct stat st;
    int res = stat(path, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("stat(%s): %i", path, res);
    return res;
}

int fclose_soloader(FILE * f) {
#ifdef USE_SCELIBC_IO
    int ret = sceLibcBridge_fclose(f);
#else
    int ret = fclose(f);
#endif

    l_debug("fclose(%p): %i", f, ret);
    return ret;
}

int close_soloader(int fd) {
    fdcache_detach(fd);
    int ret = close(fd);
    l_debug("close(%i): %i", fd, ret);
    return ret;
}

DIR* opendir_soloader(char* _pathname) {
    char buf[512];
    DIR* ret = opendir(io_path(_pathname, buf, sizeof(buf)));
    l_debug("opendir(\"%s\"): %p", _pathname, ret);
    return ret;
}

struct dirent64_bionic * readdir_soloader(DIR * dir) {
    static struct dirent64_bionic dirent_tmp;

    struct dirent* ret = readdir(dir);
    l_debug("readdir(%p): %p", dir, ret);

    if (ret) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(ret);
        memcpy(&dirent_tmp, entry_tmp, sizeof(dirent64_bionic));
        free(entry_tmp);
        return &dirent_tmp;
    }

    return NULL;
}

int readdir_r_soloader(DIR * dirp, dirent64_bionic * entry,
                       dirent64_bionic ** result) {
    struct dirent dirent_tmp;
    struct dirent * pdirent_tmp;

    int ret = readdir_r(dirp, &dirent_tmp, &pdirent_tmp);

    if (ret == 0) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(&dirent_tmp);
        memcpy(entry, entry_tmp, sizeof(dirent64_bionic));
        *result = (pdirent_tmp != NULL) ? entry : NULL;
        free(entry_tmp);
    }

    l_debug("readdir_r(%p, %p, %p): %i", dirp, entry, result, ret);
    return ret;
}

int closedir_soloader(DIR * dir) {
    int ret = closedir(dir);
    l_debug("closedir(%p): %i", dir, ret);
    return ret;
}

int fcntl_soloader(int fd, int cmd, ...) {
    l_warn("fcntl(%i, %i, ...): not implemented", fd, cmd);
    return 0;
}

int ioctl_soloader(int fd, int request, ...) {
    l_warn("ioctl(%i, %i, ...): not implemented", fd, request);
    return 0;
}

int fsync_soloader(int fd) {
    int ret = fsync(fd);
    l_debug("fsync(%i): %i", fd, ret);
    return ret;
}

int mkdir_soloader(const char * path, mode_t mode) {
    IO_PATH(path);
    return mkdir(path, mode);
}

int access_soloader(const char * path, int amode) {
    IO_PATH(path);
    return access(path, amode);
}

int remove_soloader(const char * path) {
    IO_PATH(path);
    return remove(path);
}

int unlink_soloader(const char * path) {
    IO_PATH(path);
    return unlink(path);
}

int rmdir_soloader(const char * path) {
    IO_PATH(path);
    return rmdir(path);
}

int rename_soloader(const char * from, const char * to) {
    IO_PATH(from);
    IO_PATH(to);
    return rename(from, to);
}
