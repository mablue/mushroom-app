/**
 * pty_manager.cpp — PTY (pseudo-terminal) management for the terminal emulator
 *
 * Manages PTY sessions that connect the Android terminal emulator to the
 * shell running inside the chroot environment. Each PTY session provides
 * a bidirectional byte stream between the terminal view and the shell.
 *
 * The implementation:
 *   1. Opens a PTY master/slave pair using posix_openpt()/grantpt()/unlockpt()
 *   2. Forks a child process that runs /bin/sh inside the chroot
 *   3. The child's stdin/stdout/stderr are connected to the PTY slave
 *   4. The parent reads/writes the PTY master from the terminal emulator
 *   5. Terminal emulation (VT100/ANSI parsing) is done in the Java layer
 */

#include <android/log.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <map>
#include <vector>

#include "include/engine.h"

#define TAG "MushroomPTY"

/* ---------- PTY session structure ---------- */

struct PTYSession {
    int master_fd;      /* PTY master fd (used by the terminal emulator) */
    int slave_fd;       /* PTY slave fd (connected to the shell) */
    pid_t child_pid;    /* PID of the shell process */
    bool active;
    int cols;
    int rows;
};

/* ---------- PTY context ---------- */

struct PTYContext {
    std::map<int, PTYSession*> sessions;  /* fd -> session map */
    pthread_mutex_t mutex;
    int next_id;
    char rootfs_path[MAX_ROOTFS_PATH];
};

/* ---------- PTY management ---------- */

/**
 * Create a new PTY session.
 * Returns the master file descriptor, or -1 on error.
 */
static PTYSession* pty_create_session(PTYContext* ctx, int cols, int rows) {
    PTYSession* session = new (std::nothrow) PTYSession();
    if (!session) return nullptr;

    memset(session, 0, sizeof(PTYSession));
    session->cols = cols;
    session->rows = rows;
    session->active = false;
    session->master_fd = -1;
    session->slave_fd = -1;
    session->child_pid = -1;

    /* Open PTY master */
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG,
            "posix_openpt failed: %s", strerror(errno));
        delete session;
        return nullptr;
    }

    /* Grant access to the slave */
    if (grantpt(master) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG,
            "grantpt failed: %s", strerror(errno));
        close(master);
        delete session;
        return nullptr;
    }

    /* Unlock the slave */
    if (unlockpt(master) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG,
            "unlockpt failed: %s", strerror(errno));
        close(master);
        delete session;
        return nullptr;
    }

    /* Get the slave name */
    char* slave_name = ptsname(master);
    if (!slave_name) {
        __android_log_print(ANDROID_LOG_ERROR, TAG,
            "ptsname failed: %s", strerror(errno));
        close(master);
        delete session;
        return nullptr;
    }

    /* Open the slave */
    int slave = open(slave_name, O_RDWR | O_NOCTTY);
    if (slave < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG,
            "Failed to open PTY slave: %s", strerror(errno));
        close(master);
        delete session;
        return nullptr;
    }

    /* Set terminal attributes */
    struct termios tios;
    if (tcgetattr(master, &tios) == 0) {
        cfmakeraw(&tios);
        tios.c_iflag |= IUTF8;
        tios.c_cc[VMIN] = 1;
        tios.c_cc[VTIME] = 0;
        tcsetattr(master, TCSANOW, &tios);
    }

    /* Set window size */
    struct winsize ws;
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    ws.ws_xpixel = cols * 8;
    ws.ws_ypixel = rows * 16;
    ioctl(master, TIOCSWINSZ, &ws);

    session->master_fd = master;
    session->slave_fd = slave;
    session->active = true;

    __android_log_print(ANDROID_LOG_INFO, TAG,
        "PTY session created: master=%d, slave=%s, %dx%d",
        master, slave_name, cols, rows);

    return session;
}

/**
 * Fork a shell process connected to the PTY.
 * The child process runs /bin/sh inside the chroot with the PTY as its
 * controlling terminal.
 */
static int pty_fork_shell(PTYSession* session, const char* rootfs_path) {
    pid_t pid = fork();
    if (pid < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG,
            "fork failed: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* Child process */

        /* Create a new session and become the controlling terminal owner */
        if (setsid() < 0) {
            __android_log_print(ANDROID_LOG_ERROR, TAG,
                "setsid failed: %s", strerror(errno));
        }

        /* Set the PTY as the controlling terminal */
        if (ioctl(session->slave_fd, TIOCSCTTY, 0) < 0) {
            __android_log_print(ANDROID_LOG_WARN, TAG,
                "TIOCSCTTY failed: %s", strerror(errno));
        }

        /* Duplicate PTY slave to stdin/stdout/stderr */
        dup2(session->slave_fd, STDIN_FILENO);
        dup2(session->slave_fd, STDOUT_FILENO);
        dup2(session->slave_fd, STDERR_FILENO);

        /* Close the master fd (child doesn't need it) */
        close(session->master_fd);

        /* Close all other fds */
        for (int i = 3; i < 256; i++) {
            close(i);
        }

        /* Set environment variables */
        setenv("TERM", "xterm-256color", 1);
        setenv("HOME", "/root", 1);
        setenv("USER", "root", 1);
        setenv("SHELL", "/bin/bash", 1);
        setenv("MUSHROOM", "1", 1);

        /* Try to chdir to the rootfs */
        if (rootfs_path && rootfs_path[0]) {
            chdir(rootfs_path);
        }

        /* Execute the shell */
        /* Try bash first, fall back to sh */
        char bash_path[4096];
        snprintf(bash_path, sizeof(bash_path), "%s/bin/bash", rootfs_path ? rootfs_path : "");
        char sh_path[4096];
        snprintf(sh_path, sizeof(sh_path), "%s/bin/sh", rootfs_path ? rootfs_path : "");

        struct stat st;
        if (stat(bash_path, &st) == 0) {
            execl(bash_path, "bash", "--login", nullptr);
        }
        if (stat(sh_path, &st) == 0) {
            execl(sh_path, "sh", nullptr);
        }
        /* Last resort: try /system/bin/sh (Android's shell) */
        execl("/system/bin/sh", "sh", nullptr);

        /* If we get here, execve failed */
        _exit(127);
    }

    /* Parent: close the slave fd, keep the master */
    close(session->slave_fd);
    session->slave_fd = -1;
    session->child_pid = pid;

    __android_log_print(ANDROID_LOG_INFO, TAG,
        "Shell started in PTY: pid=%d", pid);

    return pid;
}

/* ---------- Module lifecycle ---------- */

int pty_manager_init(EngineContext* ctx) {
    __android_log_print(ANDROID_LOG_INFO, TAG, "PTY manager init");

    PTYContext* pctx = new (std::nothrow) PTYContext();
    if (!pctx) return -1;

    pthread_mutex_init(&pctx->mutex, nullptr);
    pctx->next_id = 1;
    strncpy(pctx->rootfs_path, ctx->config.rootfs_path, MAX_ROOTFS_PATH - 1);

    ctx->pty_ctx = (void*)pctx;
    return 0;
}

int pty_manager_open(EngineContext* ctx) {
    PTYContext* pctx = (PTYContext*)ctx->pty_ctx;
    if (!pctx) return -1;

    pthread_mutex_lock(&pctx->mutex);

    /* Create a PTY session (80x24 default terminal size) */
    PTYSession* session = pty_create_session(pctx, 80, 24);
    if (!session) {
        pthread_mutex_unlock(&pctx->mutex);
        return -1;
    }

    /* Fork a shell inside the PTY */
    int ret = pty_fork_shell(session, pctx->rootfs_path);
    if (ret < 0) {
        close(session->master_fd);
        delete session;
        pthread_mutex_unlock(&pctx->mutex);
        return -1;
    }

    /* Register the session */
    int fd = session->master_fd;
    pctx->sessions[fd] = session;

    pthread_mutex_unlock(&pctx->mutex);

    __android_log_print(ANDROID_LOG_INFO, TAG,
        "PTY opened: fd=%d, pid=%d", fd, session->child_pid);
    return fd;
}

int pty_manager_write(EngineContext* ctx, int fd, const uint8_t* data, size_t len) {
    PTYContext* pctx = (PTYContext*)ctx->pty_ctx;
    if (!pctx || !data) return -1;

    pthread_mutex_lock(&pctx->mutex);

    auto it = pctx->sessions.find(fd);
    if (it == pctx->sessions.end() || !it->second->active) {
        pthread_mutex_unlock(&pctx->mutex);
        return -1;
    }

    ssize_t written = write(fd, data, len);

    pthread_mutex_unlock(&pctx->mutex);

    if (written < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG,
            "PTY write error: %s", strerror(errno));
        return -1;
    }

    return (int)written;
}

int pty_manager_read(EngineContext* ctx, int fd, uint8_t* buffer, size_t max_len) {
    PTYContext* pctx = (PTYContext*)ctx->pty_ctx;
    if (!pctx || !buffer) return -1;

    pthread_mutex_lock(&pctx->mutex);

    auto it = pctx->sessions.find(fd);
    if (it == pctx->sessions.end() || !it->second->active) {
        pthread_mutex_unlock(&pctx->mutex);
        return -1;
    }

    ssize_t n = read(fd, buffer, max_len);

    pthread_mutex_unlock(&pctx->mutex);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        __android_log_print(ANDROID_LOG_ERROR, TAG,
            "PTY read error: %s", strerror(errno));
        return -1;
    }

    return (int)n;
}

void pty_manager_close(EngineContext* ctx, int fd) {
    PTYContext* pctx = (PTYContext*)ctx->pty_ctx;
    if (!pctx) return;

    pthread_mutex_lock(&pctx->mutex);

    auto it = pctx->sessions.find(fd);
    if (it != pctx->sessions.end()) {
        PTYSession* session = it->second;

        /* Kill the shell process */
        if (session->child_pid > 0) {
            kill(session->child_pid, SIGTERM);
            /* Wait for the process to exit */
            int status;
            waitpid(session->child_pid, &status, WNOHANG);
        }

        /* Close the master fd */
        if (session->master_fd >= 0) {
            close(session->master_fd);
        }
        if (session->slave_fd >= 0) {
            close(session->slave_fd);
        }

        session->active = false;
        delete session;
        pctx->sessions.erase(it);

        __android_log_print(ANDROID_LOG_INFO, TAG, "PTY closed: fd=%d", fd);
    }

    pthread_mutex_unlock(&pctx->mutex);
}