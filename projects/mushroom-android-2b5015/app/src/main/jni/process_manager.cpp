/**
 * process_manager.cpp — Process spawning and management
 *
 * Manages the lifecycle of processes spawned inside the Linux environment.
 * Provides functions for:
 *   1. Forking and executing programs inside the chroot
 *   2. Setting up the environment (LD_PRELOAD, PATH, etc.)
 *   3. Managing process group membership
 *   4. Waiting for process termination
 *   5. Resource limits (memory, file descriptors)
 *
 * All child processes are spawned with the LD_PRELOAD library loaded
 * to ensure path translation works transparently.
 */

#include <android/log.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <string>
#include <vector>
#include <map>

#include "include/engine.h"

#define TAG "MushroomProc"

/* ---------- Process context ---------- */

struct ProcessContext {
    std::map<pid_t, std::string> tracked_processes;
    int max_processes;
    char engine_path[MAX_ROOTFS_PATH];
};

/* ---------- Process spawning ---------- */

/**
 * Spawn a process inside the chroot environment.
 * Automatically sets LD_PRELOAD and chdirs to the rootfs.
 *
 * @param ctx Engine context
 * @param path Path to the executable (guest view)
 * @param argv Argument vector
 * @param out_pid Output PID
 * @return 0 on success, -1 on error
 */
int process_manager_spawn(EngineContext* ctx, const char* path,
                          char* const argv[], int* out_pid) {
    if (!ctx || !path) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG,
            "Fork failed: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* Child process */

        /* Set environment variables */
        char libpath[4096];
        snprintf(libpath, sizeof(libpath), "%s/libmushroom-engine.so",
                 ctx->config.engine_path);

        setenv("LD_PRELOAD", libpath, 1);
        setenv("MUSHROOM_ROOTFS", ctx->config.rootfs_path, 1);
        setenv("MUSHROOM_ENGINE", ctx->config.engine_path, 1);
        setenv("DISPLAY", ":1", 1);
        setenv("HOME", "/root", 1);
        setenv("TERM", "xterm-256color", 1);

        /* Set PATH to include common directories */
        setenv("PATH",
               "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
               1);

        /* Chdir to the rootfs */
        chdir(ctx->config.rootfs_path);

        /* Translate the path (guest -> host) */
        std::string translated;
        if (path[0] == '/') {
            translated = std::string(ctx->config.rootfs_path) + path;
        } else {
            /* Search PATH for the executable */
            translated = path;
        }

        /* Execute the program */
        execve(translated.c_str(), argv, environ);

        /* If execve fails, try with /system/bin/sh -c */
        if (errno == ENOENT || errno == EACCES) {
            char cmd[4096];
            std::string args;
            if (argv) {
                for (int i = 1; argv[i] != nullptr; i++) {
                    if (i > 1) args += " ";
                    args += argv[i];
                }
            }
            snprintf(cmd, sizeof(cmd), "%s %s", path, args.c_str());
            execl("/system/bin/sh", "sh", "-c", cmd, nullptr);
        }

        /* If we get here, everything failed */
        _exit(127);
    }

    /* Parent */
    if (out_pid) *out_pid = (int)pid;

    ProcessContext* pctx = (ProcessContext*)ctx->proc_ctx;
    if (pctx) {
        pctx->tracked_processes[pid] = path ? std::string(path) : "unknown";
    }

    __android_log_print(ANDROID_LOG_INFO, TAG,
        "Spawned process %d: %s", pid, path ? path : "unknown");
    return 0;
}

/**
 * Wait for a process to terminate.
 *
 * @param ctx Engine context
 * @param pid Process ID to wait for
 * @return Exit status, or -1 on error
 */
int process_manager_wait(EngineContext* ctx, int pid) {
    int status;
    pid_t result = waitpid((pid_t)pid, &status, 0);
    if (result < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG,
            "waitpid(%d) failed: %s", pid, strerror(errno));
        return -1;
    }

    ProcessContext* pctx = (ProcessContext*)ctx->proc_ctx;
    if (pctx) {
        pctx->tracked_processes.erase(pid);
    }

    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

/**
 * Set resource limits for child processes.
 * Limits the memory usage of the Linux environment.
 */
int process_manager_set_limits(EngineContext* ctx, int memory_mb) {
    struct rlimit rl;

    /* Set virtual memory limit (address space) */
    rl.rlim_cur = (rlim_t)memory_mb * 1024 * 1024;
    rl.rlim_max = (rlim_t)memory_mb * 1024 * 1024;
    if (setrlimit(RLIMIT_AS, &rl) != 0) {
        __android_log_print(ANDROID_LOG_WARN, TAG,
            "Failed to set AS limit: %s", strerror(errno));
    }

    /* Set data segment size limit */
    rl.rlim_cur = (rlim_t)memory_mb * 512 * 1024;
    rl.rlim_max = (rlim_t)memory_mb * 512 * 1024;
    if (setrlimit(RLIMIT_DATA, &rl) != 0) {
        __android_log_print(ANDROID_LOG_WARN, TAG,
            "Failed to set DATA limit: %s", strerror(errno));
    }

    /* Set stack size limit */
    rl.rlim_cur = 8 * 1024 * 1024;  /* 8MB stack */
    rl.rlim_max = 8 * 1024 * 1024;
    if (setrlimit(RLIMIT_STACK, &rl) != 0) {
        __android_log_print(ANDROID_LOG_WARN, TAG,
            "Failed to set STACK limit: %s", strerror(errno));
    }

    /* Set number of file descriptors */
    rl.rlim_cur = 4096;
    rl.rlim_max = 4096;
    if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
        __android_log_print(ANDROID_LOG_WARN, TAG,
            "Failed to set NOFILE limit: %s", strerror(errno));
    }

    __android_log_print(ANDROID_LOG_INFO, TAG,
        "Resource limits set: %dMB memory, 8MB stack, 4096 fds", memory_mb);
    return 0;
}

/* ---------- Module lifecycle ---------- */

int process_manager_init(EngineContext* ctx) {
    __android_log_print(ANDROID_LOG_INFO, TAG, "Process manager init");

    ProcessContext* pctx = (ProcessContext*)malloc(sizeof(ProcessContext));
    if (!pctx) return -1;

    memset(pctx, 0, sizeof(ProcessContext));
    pctx->max_processes = 64;
    strncpy(pctx->engine_path, ctx->config.engine_path, MAX_ROOTFS_PATH - 1);

    ctx->proc_ctx = (void*)pctx;
    return 0;
}