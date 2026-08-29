/**
 * Mount Namespace Implementation
 * 
 * Creates isolated mount namespaces for the Linux environment,
 * handling virtual mounts for /proc, /sys, /dev, /tmp.
 */

#include <sys/mount.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sched.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <android/log.h>

#define LOG_TAG "Mushroom/MountNS"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" {
    int setup_mount_namespace(const char* rootfs_path);
    void teardown_mount_namespace();
}

static int g_orig_root_fd = -1;
static char g_rootfs_path[4096] = "";

int setup_mount_namespace(const char* rootfs_path) {
    strncpy(g_rootfs_path, rootfs_path, sizeof(g_rootfs_path) - 1);
    g_rootfs_path[sizeof(g_rootfs_path) - 1] = '\0';
    
    // Save current root directory
    g_orig_root_fd = open("/", O_RDONLY | O_DIRECTORY);
    if (g_orig_root_fd < 0) {
        LOGE("Failed to open root directory: %s", strerror(errno));
        return -1;
    }
    
    // Create necessary directories in rootfs
    const char* dirs[] = {"/proc", "/sys", "/dev", "/tmp", "/run", "/root", NULL};
    for (int i = 0; dirs[i]; i++) {
        char path[4096];
        snprintf(path, sizeof(path), "%s%s", g_rootfs_path, dirs[i]);
        mkdir(path, 0755);
    }
    
    // Create private mount namespace
    if (unshare(CLONE_NEWNS) < 0) {
        LOGE("Failed to create mount namespace: %s", strerror(errno));
        return -1;
    }
    
    // Make all mounts private to prevent propagation
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        LOGE("Failed to make mounts private: %s", strerror(errno));
        return -1;
    }
    
    // Pivot root into the new rootfs
    char old_root[4096];
    snprintf(old_root, sizeof(old_root), "%s/.old_root", g_rootfs_path);
    mkdir(old_root, 0700);
    
    if (pivot_root(g_rootfs_path, old_root) < 0) {
        LOGE("pivot_root failed: %s", strerror(errno));
        // Try fallback: chroot
        if (chroot(g_rootfs_path) < 0) {
            LOGE("chroot failed: %s", strerror(errno));
            return -1;
        }
        chdir("/");
    } else {
        chdir("/");
        umount2("/.old_root", MNT_DETACH);
        rmdir("/.old_root");
    }
    
    // Setup virtual filesystems
    setup_virtual_fs();
    
    LOGI("Mount namespace setup complete");
    return 0;
}

void setup_virtual_fs() {
    // Mount /proc
    if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0) {
        LOGE("Failed to mount /proc: %s", strerror(errno));
    }
    
    // Mount /sys
    if (mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_READONLY, NULL) < 0) {
        LOGE("Failed to mount /sys: %s", strerror(errno));
    }
    
    // Mount /dev as tmpfs
    if (mount("tmpfs", "/dev", "tmpfs", MS_NOSUID | MS_STRICTATIME, "mode=755,size=65536k") < 0) {
        LOGE("Failed to mount /dev: %s", strerror(errno));
    } else {
        // Create essential device nodes
        mknod("/dev/null", S_IFCHR | 0666, makedev(1, 3));
        mknod("/dev/zero", S_IFCHR | 0666, makedev(1, 5));
        mknod("/dev/random", S_IFCHR | 0666, makedev(1, 8));
        mknod("/dev/urandom", S_IFCHR | 0666, makedev(1, 9));
        mknod("/dev/tty", S_IFCHR | 0666, makedev(5, 0));
        mknod("/dev/console", S_IFCHR | 0600, makedev(5, 1));
        
        // Create ptmx for PTY support
        mknod("/dev/ptmx", S_IFCHR | 0666, makedev(5, 2));
        
        // Create tty devices
        for (int i = 0; i < 4; i++) {
            char path[32];
            snprintf(path, sizeof(path), "/dev/tty%d", i);
            mknod(path, S_IFCHR | 0600, makedev(4, i));
        }
    }
    
    // Mount /tmp
    if (mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "size=65536k") < 0) {
        LOGE("Failed to mount /tmp: %s", strerror(errno));
    }
    
    // Mount /run
    if (mount("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV, "size=16m") < 0) {
        LOGE("Failed to mount /run: %s", strerror(errno));
    }
    
    LOGI("Virtual filesystems mounted");
}

void teardown_mount_namespace() {
    // Unmount virtual filesystems
    umount2("/proc", MNT_DETACH);
    umount2("/sys", MNT_DETACH);
    umount2("/dev", MNT_DETACH);
    umount2("/tmp", MNT_DETACH);
    umount2("/run", MNT_DETACH);
    
    // Restore original root
    if (g_orig_root_fd >= 0) {
        fchdir(g_orig_root_fd);
        close(g_orig_root_fd);
        g_orig_root_fd = -1;
    }
}
