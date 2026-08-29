/**
 * Main Entry Point - Engine Lifecycle Management
 * 
 * Orchestrates the Linux environment startup, initialization, and shutdown.
 */

#include <jni.h>
#include <android/log.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#define LOG_TAG "Mushroom/Engine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// External functions from other modules
extern "C" {
    int setup_mount_namespace(const char* rootfs_path);
    void teardown_mount_namespace();
    int setup_seccomp_policy();
    int start_x11_renderer(int width, int height, void* window);
    void stop_x11_renderer();
    int download_and_extract_rootfs(const char* dest_path);
    int check_rootfs_exists(const char* path);
}

// Global state
static pthread_t g_main_thread = 0;
static pid_t g_child_pid = 0;
static int g_width = 1024;
static int g_height = 768;
static long g_memory_limit = 512 * 1024 * 1024;  // 512MB default

extern "C" {

int start_linux_environment(const char* rootfs_path, long session_id) {
    LOGI("Starting Linux environment (session: %ld)", session_id);
    
    // Check if rootfs exists
    if (!check_rootfs_exists(rootfs_path)) {
        LOGI("RootFS not found or incomplete, downloading...");
        
        int ret = download_and_extract_rootfs(rootfs_path);
        if (ret != 0) {
            LOGE("Failed to download RootFS");
            return -1;
        }
    }
    
    LOGI("RootFS ready at: %s", rootfs_path);
    
    // Fork child process
    pid_t pid = fork();
    if (pid < 0) {
        LOGE("fork() failed: %s", strerror(errno));
        return -1;
    }
    
    if (pid == 0) {
        // Child process
        // Setup mount namespace
        if (setup_mount_namespace(rootfs_path) != 0) {
            LOGE("Failed to setup mount namespace");
            _exit(1);
        }
        
        // Apply seccomp policy
        setup_seccomp_policy();
        
        // Launch Xvfb on display :1
        const char* xvfb_args[] = {
            "/usr/bin/Xvfb", ":1", "-screen", "0", 
            "1024x768x24", "+extension", NULL
        };
        
        execve("/usr/bin/Xvfb", (char**)xvfb_args, environ);
        
        // If we get here, exec failed
        LOGE("Failed to launch Xvfb: %s", strerror(errno));
        _exit(1);
    }
    
    // Parent process
    g_child_pid = pid;
    LOGI("Child process started with PID: %d", pid);
    
    // Wait for Xvfb to initialize
    usleep(500000);  // 500ms
    
    // Try to connect to X server
    sleep(1);
    
    return 0;
}

void stop_linux_environment(long session_id) {
    LOGI("Stopping Linux environment (session: %ld)", session_id);
    
    if (g_child_pid > 0) {
        kill(g_child_pid, SIGTERM);
        usleep(500000);  // Give time to shutdown gracefully
        
        int status;
        waitpid(g_child_pid, &status, WNOHANG);
        
        if (WIFEXITED(status)) {
            LOGI("Child exited with status: %d", WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            LOGI("Child terminated by signal: %d", WTERMSIG(status));
        }
        
        g_child_pid = 0;
    }
    
    teardown_mount_namespace();
    stop_x11_renderer();
}

void set_resolution(int width, int height) {
    g_width = width;
    g_height = height;
}

void set_memory_limit(long bytes) {
    g_memory_limit = bytes;
}

void update_framebuffer(void* pixels, int width, int height, int stride) {
    // This would be called from Java to update the OpenGL texture
    // Implementation depends on the renderer
}

} // extern "C"
