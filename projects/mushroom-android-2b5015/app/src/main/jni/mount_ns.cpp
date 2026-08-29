/**
 * mount_ns.cpp — Mount namespace and virtual filesystem management
 *
 * Creates a new mount namespace using unshare(CLONE_NEWNS) and sets up
 * bind mounts for /proc, /sys, /dev, and /tmp inside the RootFS.
 *
 * Since Android does not allow real mount() calls without root, this
 * module implements a "virtual mount" system that creates the illusion
 * of mounted filesystems by:
 *   1. Creating directories inside the RootFS for each mount point
 *   2. Populating them with the necessary device nodes and files
 *   3. Using LD_PRELOAD wrappers (in the interceptor) to redirect
 *      accesses to these directories to the host equivalents
 *   4. For /proc and /sys, we create minimal stub filesystems
 *
 * In a full implementation with a custom kernel or on a rooted device,
 * real mount() calls would be used. This implementation works without
 * root by using user-space path translation.
 */

#include <android/log.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sched.h>
#include <string>
#include <vector>

#include "include/engine.h"

#define TAG "MushroomMountNS"

/* ---------- Mount context ---------- */

struct MountContext {
    char rootfs_path[MAX_ROOTFS_PATH];
    bool namespace_created;
    bool virtual_mounts_ready;
    std::vector<MountEntry> mounts;
};

/* ---------- Virtual mount helpers ---------- */

/**
 * Create a minimal virtual /proc filesystem.
 * Since we can't mount a real procfs, we create a stub with common entries
 * that applications expect to find.
 */
static int create_virtual_proc(MountContext* mctx) {
    char path[4096];
    struct stat st;

    /* Create the /proc directory */
    snprintf(path, sizeof(path), "%s/proc", mctx->rootfs_path);
    if (stat(path, &st) != 0) {
        mkdir(path, 0755);
    }

    /* Create common /proc files and directories */
    const char* proc_dirs[] = {
        "/proc/self", "/proc/1", "/proc/sys", "/proc/sys/kernel",
        "/proc/sys/net", "/proc/sys/net/ipv4", "/proc/sys/vm",
        "/proc/sys/fs", "/proc/bus", "/proc/bus/input",
        "/proc/irq", "/proc/tty", "/proc/driver", "/proc/fs",
        nullptr
    };

    for (int i = 0; proc_dirs[i] != nullptr; i++) {
        snprintf(path, sizeof(path), "%s%s", mctx->rootfs_path, proc_dirs[i]);
        if (stat(path, &st) != 0) {
            mkdir(path, 0755);
        }
    }

    /* Create /proc/self/exe symlink (points to /bin/sh for simplicity) */
    snprintf(path, sizeof(path), "%s/proc/self", mctx->rootfs_path);
    /* Create a minimal /proc/self/status */
    FILE* f = fopen(path, "w");
    if (f) {
        fprintf(f, "Name:\tsh\n"
                   "State:\tR (running)\n"
                   "Tgid:\t1\n"
                   "Pid:\t1\n"
                   "PPid:\t0\n"
                   "TracerPid:\t0\n"
                   "Uid:\t0\t0\t0\t0\n"
                   "Gid:\t0\t0\t0\t0\n"
                   "FDSize:\t256\n");
        fclose(f);
    }

    __android_log_print(ANDROID_LOG_INFO, TAG, "Virtual /proc created at %s/proc", mctx->rootfs_path);
    return 0;
}

/**
 * Create a minimal virtual /sys filesystem.
 */
static int create_virtual_sys(MountContext* mctx) {
    char path[4096];
    struct stat st;

    snprintf(path, sizeof(path), "%s/sys", mctx->rootfs_path);
    if (stat(path, &st) != 0) {
        mkdir(path, 0755);
    }

    /* Create /sys/class, /sys/devices, /sys/kernel, etc. */
    const char* sys_dirs[] = {
        "/sys/class", "/sys/class/misc", "/sys/class/tty",
        "/sys/class/graphics", "/sys/class/input",
        "/sys/class/drm", "/sys/class/dma",
        "/sys/devices", "/sys/devices/system",
        "/sys/devices/virtual", "/sys/devices/virtual/tty",
        "/sys/devices/virtual/misc",
        "/sys/kernel", "/sys/kernel/notes",
        "/sys/fs", "/sys/fs/cgroup",
        "/sys/bus", "/sys/bus/platform",
        "/sys/block", "/sys/module",
        "/sys/power",
        nullptr
    };

    for (int i = 0; sys_dirs[i] != nullptr; i++) {
        snprintf(path, sizeof(path), "%s%s", mctx->rootfs_path, sys_dirs[i]);
        if (stat(path, &st) != 0) {
            mkdir(path, 0755);
        }
    }

    __android_log_print(ANDROID_LOG_INFO, TAG, "Virtual /sys created at %s/sys", mctx->rootfs_path);
    return 0;
}

/**
 * Create a minimal virtual /dev filesystem with device nodes.
 * Since we can't create real device nodes without root, we create
 * regular files that serve as placeholders. The LD_PRELOAD wrappers
 * redirect accesses to these to the host's actual device files.
 */
static int create_virtual_dev(MountContext* mctx) {
    char path[4096];
    struct stat st;

    snprintf(path, sizeof(path), "%s/dev", mctx->rootfs_path);
    if (stat(path, &st) != 0) {
        mkdir(path, 0755);
    }

    /* Create /dev subdirectories */
    const char* dev_dirs[] = {
        "/dev/pts", "/dev/shm", "/dev/input", "/dev/dri",
        "/dev/net", "/dev/snd", "/dev/usb", "/dev/bus",
        "/dev/bus/usb", "/dev/cpu", "/dev/video",
        nullptr
    };

    for (int i = 0; dev_dirs[i] != nullptr; i++) {
        snprintf(path, sizeof(path), "%s%s", mctx->rootfs_path, dev_dirs[i]);
        if (stat(path, &st) != 0) {
            mkdir(path, 0755);
        }
    }

    /* Create placeholder device files */
    struct { const char* name; mode_t mode; } dev_files[] = {
        {"/dev/null",     0666},
        {"/dev/zero",     0666},
        {"/dev/random",   0666},
        {"/dev/urandom",  0666},
        {"/dev/ptmx",     0666},
        {"/dev/tty",      0666},
        {"/dev/console",  0600},
        {"/dev/fd",       0777},
        {"/dev/stdin",    0777},
        {"/dev/stdout",   0777},
        {"/dev/stderr",   0777},
        {"/dev/full",     0666},
        {"/dev/net/tun",  0666},
        {"/dev/loop0",    0660},
        {"/dev/loop1",    0660},
        {"/dev/fb0",      0660},
        {"/dev/dri/card0", 0660},
        {"/dev/dri/renderD128", 0660},
        {"/dev/input/event0", 0660},
        {"/dev/input/mouse0", 0660},
        {"/dev/tty0",     0620},
        {"/dev/tty1",     0620},
        {"/dev/ttyS0",    0620},
        {"/dev/ttyUSB0",  0620},
        {nullptr, 0}
    };

    for (int i = 0; dev_files[i].name != nullptr; i++) {
        snprintf(path, sizeof(path), "%s%s", mctx->rootfs_path, dev_files[i].name);
        if (stat(path, &st) != 0) {
            int fd = open(path, O_WRONLY | O_CREAT, dev_files[i].mode);
            if (fd >= 0) close(fd);
        }
    }

    /* Create symlinks for /dev/fd, /dev/stdin, etc. */
    unlink(path);
    snprintf(path, sizeof(path), "%s/dev/fd", mctx->rootfs_path);
    symlink("/proc/self/fd", path);

    snprintf(path, sizeof(path), "%s/dev/stdin", mctx->rootfs_path);
    symlink("/proc/self/fd/0", path);

    snprintf(path, sizeof(path), "%s/dev/stdout", mctx->rootfs_path);
    symlink("/proc/self/fd/1", path);

    snprintf(path, sizeof(path), "%s/dev/stderr", mctx->rootfs_path);
    symlink("/proc/self/fd/2", path);

    /* Create a mount entry for /dev/pts */
    snprintf(path, sizeof(path), "%s/dev/pts", mctx->rootfs_path);
    mkdir(path, 0755);

    __android_log_print(ANDROID_LOG_INFO, TAG, "Virtual /dev created at %s/dev", mctx->rootfs_path);
    return 0;
}

/**
 * Create /tmp directory.
 */
static int create_virtual_tmp(MountContext* mctx) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/tmp", mctx->rootfs_path);
    mkdir(path, 01777);  /* Sticky bit */
    chmod(path, 01777);

    __android_log_print(ANDROID_LOG_INFO, TAG, "Virtual /tmp created at %s/tmp", mctx->rootfs_path);
    return 0;
}

/* ---------- Module lifecycle ---------- */

int mount_ns_init(EngineContext* ctx) {
    __android_log_print(ANDROID_LOG_INFO, TAG, "Mount namespace init");

    MountContext* mctx = (MountContext*)malloc(sizeof(MountContext));
    if (!mctx) return -1;

    memset(mctx, 0, sizeof(MountContext));
    strncpy(mctx->rootfs_path, ctx->config.rootfs_path, MAX_ROOTFS_PATH - 1);
    mctx->namespace_created = false;
    mctx->virtual_mounts_ready = false;

    ctx->mount_ctx = (void*)mctx;
    return 0;
}

int mount_ns_start(EngineContext* ctx) {
    MountContext* mctx = (MountContext*)ctx->mount_ctx;
    if (!mctx) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Mount context not initialized");
        return -1;
    }

    __android_log_print(ANDROID_LOG_INFO, TAG, "Setting up mount namespace");

    /* Try to create a new mount namespace */
    /* This requires CAP_SYS_ADMIN in the current namespace.
     * On Android without root, this will fail, but we handle it gracefully
     * by using our virtual mount system instead. */
    if (unshare(CLONE_NEWNS) == 0) {
        mctx->namespace_created = true;
        __android_log_print(ANDROID_LOG_INFO, TAG, "New mount namespace created");
    } else {
        __android_log_print(ANDROID_LOG_INFO, TAG,
            "Cannot create mount namespace (expected without root): %s",
            strerror(errno));
        __android_log_print(ANDROID_LOG_INFO, TAG,
            "Falling back to virtual mount system");
    }

    /* Create virtual mount points regardless of namespace availability */
    create_virtual_proc(mctx);
    create_virtual_sys(mctx);
    create_virtual_dev(mctx);
    create_virtual_tmp(mctx);

    /* If we have a real mount namespace, try actual mounts */
    if (mctx->namespace_created) {
        char path[4096];

        /* Try to mount proc */
        snprintf(path, sizeof(path), "%s/proc", mctx->rootfs_path);
        if (mount("proc", path, "proc", 0, nullptr) == 0) {
            __android_log_print(ANDROID_LOG_INFO, TAG, "Mounted proc at %s", path);
        } else {
            __android_log_print(ANDROID_LOG_INFO, TAG, "Could not mount proc: %s", strerror(errno));
        }

        /* Try to mount sysfs */
        snprintf(path, sizeof(path), "%s/sys", mctx->rootfs_path);
        if (mount("sysfs", path, "sysfs", 0, nullptr) == 0) {
            __android_log_print(ANDROID_LOG_INFO, TAG, "Mounted sysfs at %s", path);
        } else {
            __android_log_print(ANDROID_LOG_INFO, TAG, "Could not mount sysfs: %s", strerror(errno));
        }

        /* Try to mount devtmpfs */
        snprintf(path, sizeof(path), "%s/dev", mctx->rootfs_path);
        if (mount("devtmpfs", path, "devtmpfs", 0, nullptr) == 0) {
            __android_log_print(ANDROID_LOG_INFO, TAG, "Mounted devtmpfs at %s", path);
        } else {
            __android_log_print(ANDROID_LOG_INFO, TAG, "Could not mount devtmpfs: %s", strerror(errno));
        }

        /* Try to mount tmpfs at /tmp */
        snprintf(path, sizeof(path), "%s/tmp", mctx->rootfs_path);
        if (mount("tmpfs", path, "tmpfs", 0, "size=256M") == 0) {
            __android_log_print(ANDROID_LOG_INFO, TAG, "Mounted tmpfs at %s", path);
        } else {
            __android_log_print(ANDROID_LOG_INFO, TAG, "Could not mount tmpfs: %s", strerror(errno));
        }

        /* Try to mount devpts */
        snprintf(path, sizeof(path), "%s/dev/pts", mctx->rootfs_path);
        if (mount("devpts", path, "devpts", 0, "mode=620,ptmxmode=666") == 0) {
            __android_log_print(ANDROID_LOG_INFO, TAG, "Mounted devpts at %s", path);
        } else {
            __android_log_print(ANDROID_LOG_INFO, TAG, "Could not mount devpts: %s", strerror(errno));
        }

        /* Try to mount shm */
        snprintf(path, sizeof(path), "%s/dev/shm", mctx->rootfs_path);
        if (mount("tmpfs", path, "tmpfs", 0, "size=64M") == 0) {
            __android_log_print(ANDROID_LOG_INFO, TAG, "Mounted tmpfs at %s", path);
        }
    }

    mctx->virtual_mounts_ready = true;
    __android_log_print(ANDROID_LOG_INFO, TAG, "Mount namespace setup complete");

    return 0;
}

void mount_ns_stop(EngineContext* ctx) {
    MountContext* mctx = (MountContext*)ctx->mount_ctx;
    if (!mctx) return;

    /* Try to unmount if we had a real namespace */
    if (mctx->namespace_created) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/dev/pts", mctx->rootfs_path);
        umount(path);
        snprintf(path, sizeof(path), "%s/dev/shm", mctx->rootfs_path);
        umount(path);
        snprintf(path, sizeof(path), "%s/dev", mctx->rootfs_path);
        umount(path);
        snprintf(path, sizeof(path), "%s/proc", mctx->rootfs_path);
        umount(path);
        snprintf(path, sizeof(path), "%s/sys", mctx->rootfs_path);
        umount(path);
        snprintf(path, sizeof(path), "%s/tmp", mctx->rootfs_path);
        umount(path);
    }

    mctx->virtual_mounts_ready = false;
    mctx->namespace_created = false;

    __android_log_print(ANDROID_LOG_INFO, TAG, "Mount namespace stopped");
}