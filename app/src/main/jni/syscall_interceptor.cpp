/**
 * Syscall Interception Layer - Core Engine
 * 
 * This module implements LD_PRELOAD-based syscall interception for userspace
 * Linux environment emulation on Android without ptrace overhead.
 */

#include <dlfcn.h>
#include <sys/syscall.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <sys/prctl.h>
#include <android/log.h>

#define LOG_TAG "Mushroom/Syscall"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Hook registry structure
typedef struct {
    const char* syscall_name;
    void* original_func;
    void* hook_func;
    int enabled;
} SyscallHook;

// Global hook registry
static SyscallHook g_hooks[] = {
    {"open", nullptr, nullptr, 1},
    {"openat", nullptr, nullptr, 1},
    {"read", nullptr, nullptr, 1},
    {"write", nullptr, nullptr, 1},
    {"stat", nullptr, nullptr, 1},
    {"fstat", nullptr, nullptr, 1},
    {"lstat", nullptr, nullptr, 1},
    {"mount", nullptr, nullptr, 1},
    {"umount2", nullptr, nullptr, 1},
    {"chdir", nullptr, nullptr, 1},
    {"mkdir", nullptr, nullptr, 1},
    {"execve", nullptr, nullptr, 1},
    {nullptr, nullptr, nullptr, 0}
};

// Rootfs path for translation
static char g_rootfs_path[4096] = "/data/data/com.mushroom.android/files/rootfs";
static size_t g_rootfs_len = 0;

// Real function pointers (resolved at runtime)
typedef int (*real_open_t)(const char*, int, ...);
typedef int (*real_openat_t)(int, const char*, int, ...);
typedef ssize_t (*real_read_t)(int, void*, size_t);
typedef ssize_t (*real_write_t)(int, const void*, size_t);
typedef int (*real_stat_t)(const char*, struct stat*);
typedef int (*real_fstat_t)(int, struct stat*);
typedef int (*real_lstat_t)(const char*, struct stat*);
typedef int (*real_mount_t)(const char*, const char*, const char*, unsigned long, const void*);
typedef int (*real_execve_t)(const char*, char* const[], char* const[]);

// Resolve real functions
static void resolve_real_functions() {
    typedef real_open_t open_func_t;
    typedef real_openat_t openat_func_t;
    typedef real_read_t read_func_t;
    typedef real_write_t write_func_t;
    typedef real_stat_t stat_func_t;
    typedef real_fstat_t fstat_func_t;
    typedef real_lstat_t lstat_func_t;
    typedef real_mount_t mount_func_t;
    typedef real_execve_t execve_func_t;
    
    *(void**)&real_open = dlsym(RTLD_NEXT, "open");
    *(void**)&real_openat = dlsym(RTLD_NEXT, "openat");
    *(void**)&real_read = dlsym(RTLD_NEXT, "read");
    *(void**)&real_write = dlsym(RTLD_NEXT, "write");
    *(void**)&real_stat = dlsym(RTLD_NEXT, "stat");
    *(void**)&real_fstat = dlsym(RTLD_NEXT, "fstat");
    *(void**)&real_lstat = dlsym(RTLD_NEXT, "lstat");
    *(void**)&real_mount = dlsym(RTLD_NEXT, "mount");
    *(void**)&real_execve = dlsym(RTLD_NEXT, "execve");
    
    LOGI("Real function pointers resolved");
}

// Translate guest paths to host paths
static void translate_path(char* path, size_t max_len) {
    if (!path || strlen(path) == 0) return;
    
    // If path doesn't start with /, it's relative - translate it
    if (path[0] == '/') {
        // Replace prefix with rootfs path
        char translated[4096];
        snprintf(translated, sizeof(translated), "%s%s", g_rootfs_path, path);
        strncpy(path, translated, max_len);
        path[max_len - 1] = '\0';
    }
}

// Intercept open()
int open(const char* pathname, int flags, ...) {
    static bool initialized = false;
    if (!initialized) {
        resolve_real_functions();
        g_rootfs_len = strlen(g_rootfs_path);
        initialized = true;
    }
    
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode_t mode = va_arg(args, int);
        va_end(args);
        
        translate_path((char*)pathname, 4096);
        return real_open(pathname, flags, mode);
    }
    
    translate_path((char*)pathname, 4096);
    return real_open(pathname, flags);
}

// Intercept openat()
int openat(int dirfd, const char* pathname, int flags, ...) {
    static bool initialized = false;
    if (!initialized) {
        resolve_real_functions();
        g_rootfs_len = strlen(g_rootfs_path);
        initialized = true;
    }
    
    translate_path((char*)pathname, 4096);
    
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode_t mode = va_arg(args, int);
        va_end(args);
        
        return real_openat(dirfd, pathname, flags, mode);
    }
    
    return real_openat(dirfd, pathname, flags);
}

// Intercept stat(), fstat(), lstat()
int stat(const char* pathname, struct stat* statbuf) {
    translate_path((char*)pathname, 4096);
    return real_stat(pathname, statbuf);
}

int fstat(int fd, struct stat* statbuf) {
    return real_fstat(fd, statbuf);
}

int lstat(const char* pathname, struct stat* statbuf) {
    translate_path((char*)pathname, 4096);
    return real_lstat(pathname, statbuf);
}

// Intercept mount()
int mount(const char* source, const char* target, const char* filesystemtype,
          unsigned long mountflags, const void* data) {
    // Allow mounting /proc, /dev, /sys from host
    if (strstr(target, "/proc") == target || 
        strstr(target, "/dev") == target ||
        strstr(target, "/sys") == target) {
        // These should be bind-mounted from host equivalents
        char host_target[4096];
        snprintf(host_target, sizeof(host_target), "%s%s", g_rootfs_path, target);
        mkdir(host_target, 0755);
        return 0; // Success - virtual mount
    }
    
    translate_path((char*)target, 4096);
    return real_mount(source, target, filesystemtype, mountflags, data);
}

// Intercept chdir()
int chdir(const char* path) {
    translate_path((char*)path, 4096);
    return real_chdir(path);
}

// Intercept mkdir()
int mkdir(const char* pathname, mode_t mode) {
    translate_path((char*)pathname, 4096);
    return real_mkdir(pathname, mode);
}

// Set rootfs path
void set_rootfs_path(const char* path) {
    strncpy(g_rootfs_path, path, sizeof(g_rootfs_path) - 1);
    g_rootfs_path[sizeof(g_rootfs_path) - 1] = '\0';
    g_rootfs_len = strlen(g_rootfs_path);
    LOGI("Rootfs path set to: %s", g_rootfs_path);
}

// Get rootfs path
const char* get_rootfs_path() {
    return g_rootfs_path;
}
