/**
 * POSIX Wrappers - System Call Replacements
 * 
 * Implements POSIX function replacements that redirect to real implementations
 * with path translation and sandboxing.
 */

#include <jni.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/mount.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <android/log.h>

#define LOG_TAG "Mushroom/PosixWrappers"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" {
    int wrapper_execve(const char* filename, char* const argv[], char* const envp[]);
    int setup_sandbox(const char* rootfs_path);
    void cleanup_sandbox();
}

static char g_chroot_path[4096] = "";
static int g_orig_root_dir = -1;

// Wrapper for execve that sets up LD_PRELOAD automatically
int wrapper_execve(const char* filename, char* const argv[], char* const envp[]) {
    // Prepend our preload library to LD_PRELOAD
    const char* preload_lib = "/data/data/com.mushroom.android/lib/libmushroom-preload.so";
    
    // Find or create LD_PRELOAD entry in environment
    char** new_envp = envp ? copy_environ(envp) : NULL;
    
    if (new_envp) {
        int found_preload = 0;
        for (char** p = new_envp; *p; p++) {
            if (strncmp(*p, "LD_PRELOAD=", 11) == 0) {
                // Append our library to existing LD_PRELOAD
                char old_value[4096];
                strncpy(old_value, *p + 11, sizeof(old_value) - 1);
                old_value[sizeof(old_value) - 1] = '\0';
                
                char new_value[8192];
                snprintf(new_value, sizeof(new_value), "LD_PRELOAD=%s:%s", 
                         old_value, preload_lib);
                *p = strdup(new_value);
                found_preload = 1;
                break;
            }
        }
        
        if (!found_preload) {
            // Add new LD_PRELOAD entry
            char* entry = strdup(preload_lib);
            if (entry) {
                int count = 0;
                while (new_envp[count]) count++;
                new_envp = realloc(new_envp, sizeof(char*) * (count + 2));
                new_envp[count] = entry;
                new_envp[count + 1] = NULL;
            }
        }
    }
    
    // Execute the command
    pid_t pid = fork();
    if (pid == 0) {
        // Child process - change to chroot and execute
        if (setup_sandbox(g_chroot_path) == 0) {
            execve(filename, argv, new_envp ?: environ);
            _exit(127);
        }
        _exit(1);
    } else if (pid > 0) {
        // Parent process
        waitpid(pid, NULL, 0);
        if (new_envp) free_environ(new_envp);
        return 0;
    }
    
    return -1;
}

// Setup the sandbox environment
int setup_sandbox(const char* rootfs_path) {
    strncpy(g_chroot_path, rootfs_path, sizeof(g_chroot_path) - 1);
    g_chroot_path[sizeof(g_chroot_path) - 1] = '\0';
    
    // Open current directory for restore
    g_orig_root_dir = open("/", O_RDONLY | O_DIRECTORY);
    if (g_orig_root_dir < 0) {
        LOGE("Failed to open current directory: %s", strerror(errno));
        return -1;
    }
    
    // Create necessary directories in rootfs
    const char* dirs[] = {"/proc", "/sys", "/dev", "/tmp", "/root", "/home", NULL};
    for (int i = 0; dirs[i]; i++) {
        char path[4096];
        snprintf(path, sizeof(path), "%s%s", g_chroot_path, dirs[i]);
        mkdir(path, 0755);
    }
    
    // Create /proc symlink to host proc
    char proc_link[4096];
    snprintf(proc_link, sizeof(proc_link), "%s/proc", g_chroot_path);
    // Bind mount /proc into chroot
    mount("none", proc_link, "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
    
    // Create /dev entries
    char dev_link[4096];
    snprintf(dev_link, sizeof(dev_link), "%s/dev", g_chroot_path);
    mount("none", dev_link, "tmpfs", MS_NOSUID | MS_STRICTATIME, "mode=755,size=65536k");
    
    // Create pseudo-devices
    mknod("/dev/null", S_IFCHR | 0666, makedev(1, 3));
    mknod("/dev/zero", S_IFCHR | 0666, makedev(1, 5));
    mknod("/dev/random", S_IFCHR | 0666, makedev(1, 8));
    mknod("/dev/urandom", S_IFCHR | 0666, makedev(1, 9));
    mknod("/dev/tty", S_IFCHR | 0666, makedev(5, 0));
    mknod("/dev/console", S_IFCHR | 0600, makedev(5, 1));
    
    // Create /sys mount
    char sys_link[4096];
    snprintf(sys_link, sizeof(sys_link), "%s/sys", g_chroot_path);
    mount("none", sys_link, "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
    
    // Create /tmp
    char tmp_link[4096];
    snprintf(tmp_link, sizeof(tmp_link), "%s/tmp", g_chroot_path);
    mount("none", tmp_link, "tmpfs", MS_NOSUID | MS_NODEV, "size=65536k");
    
    // Perform pivot_root
    char oldroot[4096];
    snprintf(oldroot, sizeof(oldroot), "%s/.old_root", g_chroot_path);
    mkdir(oldroot, 0700);
    
    if (pivot_root(g_chroot_path, oldroot) != 0) {
        LOGE("pivot_root failed: %s", strerror(errno));
        // Fallback to chroot
        if (chroot(g_chroot_path) != 0) {
            LOGE("chroot failed: %s", strerror(errno));
            return -1;
        }
    }
    
    // Change to root
    chdir("/");
    
    // Unmount old root
    umount2("/.old_root", MNT_DETACH);
    rmdir("/.old_root");
    
    LOGI("Sandbox setup complete");
    return 0;
}

// Cleanup sandbox
void cleanup_sandbox() {
    if (g_orig_root_dir >= 0) {
        fchdir(g_orig_root_dir);
        close(g_orig_root_dir);
        g_orig_root_dir = -1;
    }
    
    // Clean up mounts
    const char* mounts[] = {"/proc", "/sys", "/dev", "/tmp", NULL};
    for (int i = 0; mounts[i]; i++) {
        umount2(mounts[i], MNT_DETACH);
    }
}

// Helper: Copy environment array
char** copy_environ(char* const* src) {
    int count = 0;
    while (src[count]) count++;
    
    char** dest = (char**)malloc(sizeof(char*) * (count + 1));
    if (!dest) return NULL;
    
    for (int i = 0; i < count; i++) {
        dest[i] = strdup(src[i]);
        if (!dest[i]) {
            // Free on failure
            for (int j = 0; j < i; j++) free(dest[j]);
            free(dest);
            return NULL;
        }
    }
    dest[count] = NULL;
    return dest;
}

// Helper: Free environment array
void free_environ(char** envp) {
    if (!envp) return;
    for (char** p = envp; *p; p++) {
        free(*p);
    }
    free(envp);
}
