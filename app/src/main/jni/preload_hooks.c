/**
 * Preload Library - LD_PRELOAD Hook Implementation
 * 
 * This is compiled as a separate shared library that gets injected via
 * LD_PRELOAD before any other libraries when executing inside the chroot.
 */

#include <unistd.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <limits.h>

// Real function pointers
static int (*real_open)(const char*, int, ...) = NULL;
static int (*real_openat)(int, const char*, int, ...);
static ssize_t (*real_read)(int, void*, size_t);
static ssize_t (*real_write)(int, const void*, size_t);
static int (*real_stat)(const char*, struct stat*);
static int (*real_fstat)(int, struct stat*);
static int (*real_lstat)(const char*, struct stat*);
static pid_t (*real_fork)(void) = NULL;
static int (*real_execve)(const char*, char* const[], char* const[]);

// Rootfs prefix
static const char* ROOTFS_PREFIX = "/data/data/com.mushroom.android/files/rootfs";
static char g_rootfs_buf[PATH_MAX];

__attribute__((constructor))
static void init_hooks() {
    // Resolve real functions
    real_open = dlsym(RTLD_NEXT, "open");
    real_openat = dlsym(RTLD_NEXT, "openat");
    real_read = dlsym(RTLD_NEXT, "read");
    real_write = dlsym(RTLD_NEXT, "write");
    real_stat = dlsym(RTLD_NEXT, "stat");
    real_fstat = dlsym(RTLD_NEXT, "fstat");
    real_lstat = dlsym(RTLD_NEXT, "lstat");
    real_fork = dlsym(RTLD_NEXT, "fork");
    real_execve = dlsym(RTLD_NEXT, "execve");
    
    // Get rootfs path from environment
    const char* env_rootfs = getenv("ROOTFS_PATH");
    if (env_rootfs) {
        strncpy(ROOTFS_PREFIX, env_rootfs, PATH_MAX - 1);
        ROOTFS_PREFIX[PATH_MAX - 1] = '\0';
    }
}

// Translate path from guest to host
static char* translate_path(const char* path) {
    static char translated[PATH_MAX];
    
    if (!path || path[0] == '\0') {
        return NULL;
    }
    
    // Handle absolute paths
    if (path[0] == '/') {
        snprintf(translated, sizeof(translated), "%s%s", ROOTFS_PREFIX, path);
    } else {
        // Relative path - prepend rootfs
        snprintf(translated, sizeof(translated), "%s/%s", ROOTFS_PREFIX, path);
    }
    
    return translated;
}

int open(const char* pathname, int flags, ...) {
    if (!real_open) init_hooks();
    
    char* translated = translate_path(pathname);
    if (!translated) {
        errno = EINVAL;
        return -1;
    }
    
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, int);
        va_end(args);
        return real_open(translated, flags, mode);
    }
    
    return real_open(translated, flags);
}

int openat(int dirfd, const char* pathname, int flags, ...) {
    if (!real_openat) init_hooks();
    
    char* translated = translate_path(pathname);
    if (!translated) {
        errno = EINVAL;
        return -1;
    }
    
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, int);
        va_end(args);
        return real_openat(dirfd, translated, flags, mode);
    }
    
    return real_openat(dirfd, translated, flags);
}

int stat(const char* pathname, struct stat* statbuf) {
    if (!real_stat) init_hooks();
    
    char* translated = translate_path(pathname);
    if (!translated) {
        errno = ENOENT;
        return -1;
    }
    
    return real_stat(translated, statbuf);
}

int fstat(int fd, struct stat* statbuf) {
    if (!real_fstat) init_hooks();
    return real_fstat(fd, statbuf);
}

int lstat(const char* pathname, struct stat* statbuf) {
    if (!real_lstat) init_hooks();
    
    char* translated = translate_path(pathname);
    if (!translated) {
        errno = ENOENT;
        return -1;
    }
    
    return real_lstat(translated, statbuf);
}

ssize_t read(int fd, void* buf, size_t count) {
    if (!real_read) init_hooks();
    return real_read(fd, buf, count);
}

ssize_t write(int fd, const void* buf, size_t count) {
    if (!real_write) init_hooks();
    return real_write(fd, buf, count);
}

pid_t fork(void) {
    if (!real_fork) init_hooks();
    
    pid_t pid = real_fork();
    if (pid == 0) {
        // Child process - setup LD_PRELOAD
        const char* preload_lib = "/data/data/com.mushroom.android/lib/libmushroom-preload.so";
        setenv("LD_PRELOAD", preload_lib, 1);
    }
    return pid;
}
