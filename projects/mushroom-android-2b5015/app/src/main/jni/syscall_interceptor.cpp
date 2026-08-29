/**
 * syscall_interceptor.cpp — LD_PRELOAD-based syscall interception layer
 *
 * This is the core of the Mushroom "no-ptrace" interception strategy.
 * Rather than ptrace-ing every syscall (which costs ~10,000+ context switches
 * per second), we use LD_PRELOAD to override libc functions at the user-space
 * level before they reach the kernel. This gives near-native execution speed.
 *
 * Strategy:
 *   1. The library is loaded via LD_PRELOAD into every child process spawned
 *      inside the chroot environment.
 *   2. For each intercepted function (open, openat, stat, execve, etc.),
 *      we call dlsym(RTLD_NEXT, ...) to get the real libc implementation.
 *   3. Our wrapper translates paths from the guest view to the host view
 *      using fakechroot_translate_path(), then calls the real function.
 *   4. The interceptor is registered in the parent process so that execve
 *      automatically sets LD_PRELOAD for new children.
 *
 * This approach avoids ptrace entirely while maintaining full path translation.
 */

#include <android/log.h>
#include <dlfcn.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <pthread.h>
#include <map>
#include <string>
#include <vector>

#include "include/engine.h"

#define TAG "MushroomInterceptor"

/* ---------- Type definitions for real libc functions ---------- */

typedef int (*real_open_t)(const char* pathname, int flags, ...);
typedef int (*real_openat_t)(int dirfd, const char* pathname, int flags, ...);
typedef int (*real_close_t)(int fd);
typedef ssize_t (*real_read_t)(int fd, void* buf, size_t count);
typedef ssize_t (*real_write_t)(int fd, const void* buf, size_t count);
typedef int (*real_stat_t)(const char* pathname, struct stat* statbuf);
typedef int (*real_lstat_t)(const char* pathname, struct stat* statbuf);
typedef int (*real_fstat_t)(int fd, struct stat* statbuf);
typedef int (*real_stat64_t)(const char* pathname, struct stat64* statbuf);
typedef int (*real_lstat64_t)(const char* pathname, struct stat64* statbuf);
typedef int (*real_fstatat64_t)(int dirfd, const char* pathname, struct stat64* statbuf, int flags);
typedef int (*real_access_t)(const char* pathname, int mode);
typedef int (*real_mkdir_t)(const char* pathname, mode_t mode);
typedef int (*real_rmdir_t)(const char* pathname);
typedef int (*real_unlink_t)(const char* pathname);
typedef int (*real_rename_t)(const char* oldpath, const char* newpath);
typedef int (*real_chdir_t)(const char* path);
typedef char* (*real_getcwd_t)(char* buf, size_t size);
typedef int (*real_execve_t)(const char* pathname, char* const argv[], char* const envp[]);
typedef int (*real_execvp_t)(const char* file, char* const argv[]);
typedef int (*real_mount_t)(const char* source, const char* target,
                            const char* fstype, unsigned long flags, const void* data);
typedef int (*real_umount_t)(const char* target);
typedef int (*real_umount2_t)(const char* target, int flags);
typedef DIR* (*real_opendir_t)(const char* name);
typedef struct dirent* (*real_readdir_t)(DIR* dirp);
typedef int (*real_closedir_t)(DIR* dirp);

/* ---------- Interceptor context ---------- */

struct InterceptorContext {
    /* Real function pointers (loaded via dlsym(RTLD_NEXT)) */
    real_open_t real_open;
    real_openat_t real_openat;
    real_close_t real_close;
    real_read_t real_read;
    real_write_t real_write;
    real_stat_t real_stat;
    real_lstat_t real_lstat;
    real_fstat_t real_fstat;
    real_stat64_t real_stat64;
    real_lstat64_t real_lstat64;
    real_fstatat64_t real_fstatat64;
    real_access_t real_access;
    real_mkdir_t real_mkdir;
    real_rmdir_t real_rmdir;
    real_unlink_t real_unlink;
    real_rename_t real_rename;
    real_chdir_t real_chdir;
    real_getcwd_t real_getcwd;
    real_execve_t real_execve;
    real_execvp_t real_execvp;
    real_mount_t real_mount;
    real_umount_t real_umount;
    real_umount2_t real_umount2;
    real_opendir_t real_opendir;
    real_readdir_t real_readdir;
    real_closedir_t real_closedir;

    /* RootFS path prefix (host-side) */
    std::string rootfs_path;

    /* Whether we're inside the chroot (path translation enabled) */
    bool inside_chroot;

    /* Hook registration guard */
    pthread_mutex_t mutex;

    /* Process environment for execve interception */
    std::string ld_preload_value;
};

static InterceptorContext* g_ictx = nullptr;

/* ---------- Helper: path translation ---------- */

/**
 * Translate a guest path to a host path.
 * Guest "/" → host "/data/data/com.mushroom.android/files/rootfs"
 * Guest "/usr/bin" → host "/data/.../rootfs/usr/bin"
 * Host paths (starting with "/data/" or "/proc/self/") are left unchanged.
 */
static std::string translate_path(const std::string& path) {
    if (!g_ictx || !g_ictx->inside_chroot || g_ictx->rootfs_path.empty()) {
        return path;
    }

    /* Don't translate paths that are already absolute host paths */
    if (path.find(g_ictx->rootfs_path) == 0) {
        return path;
    }
    /* Don't translate Android system paths */
    if (path.find("/data/") == 0 || path.find("/proc/") == 0 ||
        path.find("/sys/") == 0 || path.find("/dev/") == 0) {
        return path;
    }
    /* Don't translate /dev/null, /dev/zero, etc. */
    if (path.find("/dev/") == 0) {
        return path;
    }

    /* Translate guest path to host path */
    if (path == "/") {
        return g_ictx->rootfs_path;
    }
    if (path[0] == '/') {
        return g_ictx->rootfs_path + path;
    }
    return path;
}

/* ---------- Intercepted functions (LD_PRELOAD overrides) ---------- */

extern "C" {

int open(const char* pathname, int flags, ...) {
    if (!g_ictx || !g_ictx->real_open) {
        return ::open(pathname, flags);
    }
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    std::string translated = translate_path(pathname);
    return g_ictx->real_open(translated.c_str(), flags, mode);
}

int openat(int dirfd, const char* pathname, int flags, ...) {
    if (!g_ictx || !g_ictx->real_openat) {
        return ::openat(dirfd, pathname, flags);
    }
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    std::string translated = translate_path(pathname);
    return g_ictx->real_openat(dirfd, translated.c_str(), flags, mode);
}

int close(int fd) {
    if (!g_ictx || !g_ictx->real_close) {
        return ::close(fd);
    }
    return g_ictx->real_close(fd);
}

ssize_t read(int fd, void* buf, size_t count) {
    if (!g_ictx || !g_ictx->real_read) {
        return ::read(fd, buf, count);
    }
    return g_ictx->real_read(fd, buf, count);
}

ssize_t write(int fd, const void* buf, size_t count) {
    if (!g_ictx || !g_ictx->real_write) {
        return ::write(fd, buf, count);
    }
    return g_ictx->real_write(fd, buf, count);
}

int stat(const char* pathname, struct stat* statbuf) {
    if (!g_ictx || !g_ictx->real_stat) {
        return ::stat(pathname, statbuf);
    }
    std::string translated = translate_path(pathname);
    return g_ictx->real_stat(translated.c_str(), statbuf);
}

int lstat(const char* pathname, struct stat* statbuf) {
    if (!g_ictx || !g_ictx->real_lstat) {
        return ::lstat(pathname, statbuf);
    }
    std::string translated = translate_path(pathname);
    return g_ictx->real_lstat(translated.c_str(), statbuf);
}

int fstat(int fd, struct stat* statbuf) {
    if (!g_ictx || !g_ictx->real_fstat) {
        return ::fstat(fd, statbuf);
    }
    return g_ictx->real_fstat(fd, statbuf);
}

int stat64(const char* pathname, struct stat64* statbuf) {
    if (!g_ictx || !g_ictx->real_stat64) {
        return ::stat64(pathname, statbuf);
    }
    std::string translated = translate_path(pathname);
    return g_ictx->real_stat64(translated.c_str(), statbuf);
}

int lstat64(const char* pathname, struct stat64* statbuf) {
    if (!g_ictx || !g_ictx->real_lstat64) {
        return ::lstat64(pathname, statbuf);
    }
    std::string translated = translate_path(pathname);
    return g_ictx->real_lstat64(translated.c_str(), statbuf);
}

int fstatat64(int dirfd, const char* pathname, struct stat64* statbuf, int flags) {
    if (!g_ictx || !g_ictx->real_fstatat64) {
        return ::fstatat64(dirfd, pathname, statbuf, flags);
    }
    std::string translated = translate_path(pathname);
    return g_ictx->real_fstatat64(dirfd, translated.c_str(), statbuf, flags);
}

int access(const char* pathname, int mode) {
    if (!g_ictx || !g_ictx->real_access) {
        return ::access(pathname, mode);
    }
    std::string translated = translate_path(pathname);
    return g_ictx->real_access(translated.c_str(), mode);
}

int mkdir(const char* pathname, mode_t mode) {
    if (!g_ictx || !g_ictx->real_mkdir) {
        return ::mkdir(pathname, mode);
    }
    std::string translated = translate_path(pathname);
    return g_ictx->real_mkdir(translated.c_str(), mode);
}

int rmdir(const char* pathname) {
    if (!g_ictx || !g_ictx->real_rmdir) {
        return ::rmdir(pathname);
    }
    std::string translated = translate_path(pathname);
    return g_ictx->real_rmdir(translated.c_str());
}

int unlink(const char* pathname) {
    if (!g_ictx || !g_ictx->real_unlink) {
        return ::unlink(pathname);
    }
    std::string translated = translate_path(pathname);
    return g_ictx->real_unlink(translated.c_str());
}

int rename(const char* oldpath, const char* newpath) {
    if (!g_ictx || !g_ictx->real_rename) {
        return ::rename(oldpath, newpath);
    }
    std::string old_translated = translate_path(oldpath);
    std::string new_translated = translate_path(newpath);
    return g_ictx->real_rename(old_translated.c_str(), new_translated.c_str());
}

int chdir(const char* path) {
    if (!g_ictx || !g_ictx->real_chdir) {
        return ::chdir(path);
    }
    std::string translated = translate_path(path);
    return g_ictx->real_chdir(translated.c_str());
}

char* getcwd(char* buf, size_t size) {
    if (!g_ictx || !g_ictx->real_getcwd) {
        return ::getcwd(buf, size);
    }
    return g_ictx->real_getcwd(buf, size);
}

int execve(const char* pathname, char* const argv[], char* const envp[]) {
    if (!g_ictx || !g_ictx->real_execve) {
        return ::execve(pathname, argv, envp);
    }

    /* Inject LD_PRELOAD into the environment of the child process */
    std::vector<std::string> env_strings;
    if (g_ictx->inside_chroot) {
        bool has_ld_preload = false;
        if (envp) {
            for (int i = 0; envp[i] != nullptr; i++) {
                if (strncmp(envp[i], "LD_PRELOAD=", 11) == 0) {
                    has_ld_preload = true;
                    /* Append our library to existing LD_PRELOAD */
                    std::string merged = "LD_PRELOAD=";
                    merged += g_ictx->ld_preload_value;
                    merged += ":";
                    merged += (envp[i] + 11);
                    env_strings.push_back(merged);
                } else {
                    env_strings.push_back(envp[i]);
                }
            }
        }
        if (!has_ld_preload && !g_ictx->ld_preload_value.empty()) {
            env_strings.push_back("LD_PRELOAD=" + g_ictx->ld_preload_value);
        }
        /* Add MUSHROOM_ROOTFS hint */
        env_strings.push_back("MUSHROOM_ROOTFS=" + g_ictx->rootfs_path);
        env_strings.push_back(nullptr);

        /* Build envp array */
        std::vector<char*> new_envp;
        for (auto& s : env_strings) {
            new_envp.push_back(const_cast<char*>(s.c_str()));
        }

        std::string translated = translate_path(pathname);
        return g_ictx->real_execve(translated.c_str(), argv, new_envp.data());
    }

    return g_ictx->real_execve(pathname, argv, envp);
}

int execvp(const char* file, char* const argv[]) {
    if (!g_ictx || !g_ictx->real_execvp) {
        return ::execvp(file, argv);
    }
    /* Similar to execve, but searches PATH */
    std::string translated = translate_path(file);
    return g_ictx->real_execvp(translated.c_str(), argv);
}

int mount(const char* source, const char* target,
          const char* fstype, unsigned long flags, const void* data) {
    if (!g_ictx || !g_ictx->real_mount) {
        return ::mount(source, target, fstype, flags, data);
    }
    std::string src_translated = translate_path(source ? source : "");
    std::string tgt_translated = translate_path(target ? target : "");
    return g_ictx->real_mount(
        src_translated.empty() ? nullptr : src_translated.c_str(),
        tgt_translated.empty() ? nullptr : tgt_translated.c_str(),
        fstype, flags, data);
}

int umount(const char* target) {
    if (!g_ictx || !g_ictx->real_umount) {
        return ::umount(target);
    }
    std::string translated = translate_path(target);
    return g_ictx->real_umount(translated.c_str());
}

int umount2(const char* target, int flags) {
    if (!g_ictx || !g_ictx->real_umount2) {
        return ::umount2(target, flags);
    }
    std::string translated = translate_path(target);
    return g_ictx->real_umount2(translated.c_str(), flags);
}

DIR* opendir(const char* name) {
    if (!g_ictx || !g_ictx->real_opendir) {
        return ::opendir(name);
    }
    std::string translated = translate_path(name);
    return g_ictx->real_opendir(translated.c_str());
}

struct dirent* readdir(DIR* dirp) {
    if (!g_ictx || !g_ictx->real_readdir) {
        return ::readdir(dirp);
    }
    return g_ictx->real_readdir(dirp);
}

int closedir(DIR* dirp) {
    if (!g_ictx || !g_ictx->real_closedir) {
        return ::closedir(dirp);
    }
    return g_ictx->real_closedir(dirp);
}

} /* extern "C" */

/* ---------- Interceptor module lifecycle ---------- */

int interceptor_init(EngineContext* ctx) {
    __android_log_print(ANDROID_LOG_INFO, TAG, "Initializing syscall interceptor");

    if (!g_ictx) {
        g_ictx = new (std::nothrow) InterceptorContext();
        if (!g_ictx) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to allocate interceptor context");
            return -1;
        }
        pthread_mutex_init(&g_ictx->mutex, nullptr);
    }

    pthread_mutex_lock(&g_ictx->mutex);

    g_ictx->rootfs_path = ctx->config.rootfs_path;
    g_ictx->inside_chroot = false;
    g_ictx->ld_preload_value = "";

    /* Resolve real function pointers via dlsym(RTLD_NEXT) */
    g_ictx->real_open = (real_open_t)dlsym(RTLD_NEXT, "open");
    g_ictx->real_openat = (real_openat_t)dlsym(RTLD_NEXT, "openat");
    g_ictx->real_close = (real_close_t)dlsym(RTLD_NEXT, "close");
    g_ictx->real_read = (real_read_t)dlsym(RTLD_NEXT, "read");
    g_ictx->real_write = (real_write_t)dlsym(RTLD_NEXT, "write");
    g_ictx->real_stat = (real_stat_t)dlsym(RTLD_NEXT, "stat");
    g_ictx->real_lstat = (real_lstat_t)dlsym(RTLD_NEXT, "lstat");
    g_ictx->real_fstat = (real_fstat_t)dlsym(RTLD_NEXT, "fstat");
    g_ictx->real_stat64 = (real_stat64_t)dlsym(RTLD_NEXT, "stat64");
    g_ictx->real_lstat64 = (real_lstat64_t)dlsym(RTLD_NEXT, "lstat64");
    g_ictx->real_fstatat64 = (real_fstatat64_t)dlsym(RTLD_NEXT, "fstatat64");
    g_ictx->real_access = (real_access_t)dlsym(RTLD_NEXT, "access");
    g_ictx->real_mkdir = (real_mkdir_t)dlsym(RTLD_NEXT, "mkdir");
    g_ictx->real_rmdir = (real_rmdir_t)dlsym(RTLD_NEXT, "rmdir");
    g_ictx->real_unlink = (real_unlink_t)dlsym(RTLD_NEXT, "unlink");
    g_ictx->real_rename = (real_rename_t)dlsym(RTLD_NEXT, "rename");
    g_ictx->real_chdir = (real_chdir_t)dlsym(RTLD_NEXT, "chdir");
    g_ictx->real_getcwd = (real_getcwd_t)dlsym(RTLD_NEXT, "getcwd");
    g_ictx->real_execve = (real_execve_t)dlsym(RTLD_NEXT, "execve");
    g_ictx->real_execvp = (real_execvp_t)dlsym(RTLD_NEXT, "execvp");
    g_ictx->real_mount = (real_mount_t)dlsym(RTLD_NEXT, "mount");
    g_ictx->real_umount = (real_umount_t)dlsym(RTLD_NEXT, "umount");
    g_ictx->real_umount2 = (real_umount2_t)dlsym(RTLD_NEXT, "umount2");
    g_ictx->real_opendir = (real_opendir_t)dlsym(RTLD_NEXT, "opendir");
    g_ictx->real_readdir = (real_readdir_t)dlsym(RTLD_NEXT, "readdir");
    g_ictx->real_closedir = (real_closedir_t)dlsym(RTLD_NEXT, "closedir");

    /* Verify critical symbols were resolved */
    if (!g_ictx->real_open || !g_ictx->real_stat || !g_ictx->real_execve) {
        __android_log_print(ANDROID_LOG_ERROR, TAG,
            "Failed to resolve critical symbols: open=%p, stat=%p, execve=%p",
            g_ictx->real_open, g_ictx->real_stat, g_ictx->real_execve);
        pthread_mutex_unlock(&g_ictx->mutex);
        return -1;
    }

    __android_log_print(ANDROID_LOG_INFO, TAG, "Interceptor initialized with %zu hooks", (size_t)26);

    ctx->interceptor_ctx = (void*)g_ictx;
    pthread_mutex_unlock(&g_ictx->mutex);
    return 0;
}

int interceptor_start(EngineContext* ctx) {
    if (!g_ictx) return -1;

    pthread_mutex_lock(&g_ictx->mutex);

    /* Set inside_chroot to enable path translation */
    g_ictx->inside_chroot = true;

    /* Determine the LD_PRELOAD path for child processes */
    /* The interceptor library is loaded from the app's native lib dir */
    char libpath[4096];
    snprintf(libpath, sizeof(libpath), "%s/libmushroom-engine.so",
             ctx->config.engine_path);
    g_ictx->ld_preload_value = libpath;

    __android_log_print(ANDROID_LOG_INFO, TAG, "Interceptor started, path translation enabled");

    pthread_mutex_unlock(&g_ictx->mutex);
    return 0;
}

void interceptor_stop(EngineContext* ctx) {
    if (!g_ictx) return;

    pthread_mutex_lock(&g_ictx->mutex);
    g_ictx->inside_chroot = false;
    pthread_mutex_unlock(&g_ictx->mutex);

    __android_log_print(ANDROID_LOG_INFO, TAG, "Interceptor stopped");
}