/**
 * posix_wrappers.cpp — POSIX wrapper implementations for the LD_PRELOAD layer
 *
 * This file implements the actual POSIX function replacements used by the
 * syscall interceptor. Each wrapper:
 *   1. Receives the call from the LD_PRELOAD-overridden function
 *   2. Translates the path from guest view to host view
 *   3. Calls the real libc function (via dlsym RTLD_NEXT)
 *   4. Returns the result
 *
 * The wrappers are installed in the interceptor and are used by the child
 * processes spawned inside the chroot environment.
 */

#include <android/log.h>
#include <dlfcn.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdarg.h>
#include <string>
#include <map>

#include "include/engine.h"

#define TAG "MushroomPOSIX"

/* ---------- Real function pointer resolution ---------- */

/**
 * Resolve a symbol from the "next" (real) library using dlsym(RTLD_NEXT).
 * This is the standard mechanism for LD_PRELOAD wrappers to call the
 * original implementation.
 */
void* real_dlsym(void* /*handle*/, const char* symbol) {
    void* func = dlsym(RTLD_NEXT, symbol);
    if (!func) {
        /* Fallback: try dlsym with RTLD_DEFAULT */
        func = dlsym(RTLD_DEFAULT, symbol);
    }
    if (!func) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "Symbol not found: %s", symbol);
    }
    return func;
}

/* ---------- Path translation helper (used by all wrappers) ---------- */

/**
 * The rootfs path is set by the interceptor when inside_chroot becomes true.
 * This is a global variable set by the interceptor module.
 */
extern InterceptorContext* g_ictx;

static std::string translate_wrapper_path(const char* path) {
    if (!path || !g_ictx || !g_ictx->inside_chroot) {
        return path ? std::string(path) : std::string();
    }

    std::string p(path);

    /* Skip empty paths */
    if (p.empty()) return p;

    /* Don't translate paths that are already absolute host paths */
    if (p.find(g_ictx->rootfs_path) == 0) {
        return p;
    }

    /* Don't translate special device paths */
    if (p == "/dev/null" || p == "/dev/zero" || p == "/dev/random" ||
        p == "/dev/urandom" || p == "/dev/ptmx" || p == "/dev/tty") {
        return p;
    }

    /* Don't translate Android system paths */
    if (p.find("/data/") == 0 || p.find("/proc/") == 0 ||
        p.find("/sys/") == 0 || p.find("/apex/") == 0 ||
        p.find("/system/") == 0 || p.find("/vendor/") == 0) {
        return p;
    }

    /* Translate guest root to host rootfs */
    if (p == "/") {
        return g_ictx->rootfs_path;
    }

    /* Prepend rootfs path for absolute paths */
    if (p[0] == '/') {
        return g_ictx->rootfs_path + p;
    }

    return p;
}

/* ---------- FD tracking for virtual file descriptors ---------- */

/**
 * Some fd operations need special handling because the translated path
 * may differ from the guest view. We maintain a map of "virtual" fds
 * that are transparent to the application.
 */
static std::map<int, std::string> g_virtual_fds;
static pthread_mutex_t g_fd_mutex = PTHREAD_MUTEX_INITIALIZER;

static void track_fd(int fd, const char* path) {
    if (fd < 0) return;
    pthread_mutex_lock(&g_fd_mutex);
    g_virtual_fds[fd] = path ? std::string(path) : std::string();
    pthread_mutex_unlock(&g_fd_mutex);
}

static void untrack_fd(int fd) {
    pthread_mutex_lock(&g_fd_mutex);
    g_virtual_fds.erase(fd);
    pthread_mutex_unlock(&g_fd_mutex);
}

/* ---------- POSIX wrapper implementations ---------- */

/* These are declared in syscall_interceptor.cpp as extern "C" functions.
 * The actual implementations are there for the LD_PRELOAD case.
 * This file provides helper functions used by the interceptor module.
 */

/**
 * Create a symlink inside the rootfs for virtual filesystem entries.
 * Used to make /proc, /sys, /dev, /tmp visible inside the chroot.
 */
int posix_create_virtual_mounts(const char* rootfs_path) {
    if (!rootfs_path) return -1;

    char path[4096];
    struct stat st;

    /* Create mount points inside rootfs */
    const char* dirs[] = {"/proc", "/sys", "/dev", "/tmp", "/dev/pts", "/dev/shm", nullptr};

    for (int i = 0; dirs[i] != nullptr; i++) {
        snprintf(path, sizeof(path), "%s%s", rootfs_path, dirs[i]);
        if (stat(path, &st) != 0) {
            if (mkdir(path, 0755) != 0 && errno != EEXIST) {
                __android_log_print(ANDROID_LOG_WARN, TAG,
                    "Failed to create directory %s: %s", path, strerror(errno));
            }
        }
    }

    /* Create /dev/null, /dev/zero, /dev/random, /dev/urandom as device nodes
     * Note: on Android without root, we can't create real device nodes.
     * Instead, we create symlinks to the host's device files. */
    const char* dev_links[][2] = {
        {"/dev/null", "/dev/null"},
        {"/dev/zero", "/dev/zero"},
        {"/dev/random", "/dev/random"},
        {"/dev/urandom", "/dev/urandom"},
        {"/dev/ptmx", "/dev/ptmx"},
        {"/dev/tty", "/dev/tty"},
        {nullptr, nullptr}
    };

    for (int i = 0; dev_links[i][0] != nullptr; i++) {
        snprintf(path, sizeof(path), "%s%s", rootfs_path, dev_links[i][0]);
        struct stat link_st;
        if (lstat(path, &link_st) != 0) {
            /* Create a regular file as a placeholder for the device */
            int fd = open(path, O_WRONLY | O_CREAT, 0666);
            if (fd >= 0) close(fd);
        }
    }

    __android_log_print(ANDROID_LOG_INFO, TAG, "Virtual mount points created in %s", rootfs_path);
    return 0;
}

/**
 * Initialize the POSIX wrappers module.
 * This is called during engine initialization to set up the wrapper layer.
 */
int posix_wrappers_init(EngineContext* ctx) {
    __android_log_print(ANDROID_LOG_INFO, TAG, "POSIX wrappers init");

    /* Create virtual mount points in the rootfs directory */
    if (ctx->config.rootfs_path[0] != '\0') {
        posix_create_virtual_mounts(ctx->config.rootfs_path);
    }

    return 0;
}