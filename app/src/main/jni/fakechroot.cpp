/**
 * FakeChroot Implementation
 * 
 * Implements chroot-like behavior without actual system call chroot(),
 * using path translation and environment manipulation.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <android/log.h>

#define LOG_TAG "Mushroom/FakeChroot"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" {
    int fakechroot_init(const char* rootfs_path);
    void fakechroot_cleanup();
    char* fakechroot_translate_path(const char* path);
}

static char g_rootfs[4096] = "";
static int g_initialized = 0;

int fakechroot_init(const char* rootfs_path) {
    strncpy(g_rootfs, rootfs_path, sizeof(g_rootfs) - 1);
    g_rootfs[sizeof(g_rootfs) - 1] = '\0';
    g_initialized = 1;
    
    // Verify rootfs exists
    struct stat st;
    if (stat(g_rootfs, &st) != 0 || !S_ISDIR(st.st_mode)) {
        LOGE("Rootfs path does not exist or is not a directory: %s", g_rootfs);
        return -1;
    }
    
    // Set environment variables for guest OS
    setenv("ROOTFS_PATH", g_rootfs, 1);
    setenv("HOME", "/root", 1);
    setenv("TERM", "linux", 1);
    
    LOGI("FakeChroot initialized with: %s", g_rootfs);
    return 0;
}

void fakechroot_cleanup() {
    unsetenv("ROOTFS_PATH");
    g_initialized = 0;
    memset(g_rootfs, 0, sizeof(g_rootfs));
}

char* fakechroot_translate_path(const char* path) {
    static char translated[PATH_MAX];
    
    if (!g_initialized) {
        errno = ENOENT;
        return NULL;
    }
    
    if (!path || path[0] == '\0') {
        errno = EINVAL;
        return NULL;
    }
    
    // Handle absolute paths
    if (path[0] == '/') {
        snprintf(translated, sizeof(translated), "%s%s", g_rootfs, path);
    } else {
        // Relative path - prepend current working directory simulation
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            // Get the "virtual" cwd from our tracking
            snprintf(translated, sizeof(translated), "%s/%s", g_rootfs, path);
        } else {
            snprintf(translated, sizeof(translated), "%s/%s", g_rootfs, path);
        }
    }
    
    return translated;
}

// Wrapper functions that use translation
FILE* fake_fopen(const char* path, const char* mode) {
    char* translated = fakechroot_translate_path(path);
    if (!translated) return NULL;
    return fopen(translated, mode);
}

int fake_open(const char* path, int flags, ...) {
    char* translated = fakechroot_translate_path(path);
    if (!translated) return -1;
    
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, int);
        va_end(args);
        return open(translated, flags, mode);
    }
    return open(translated, flags);
}

int fake_stat(const char* path, struct stat* statbuf) {
    char* translated = fakechroot_translate_path(path);
    if (!translated) return -1;
    return stat(translated, statbuf);
}
