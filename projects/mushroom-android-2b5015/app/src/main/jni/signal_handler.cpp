/**
 * signal_handler.cpp — Signal handling for the Linux environment
 *
 * Manages signal delivery between the Android process and the Linux
 * environment processes. Key responsibilities:
 *   1. Set up signal handlers for SIGCHLD (child process termination)
 *   2. Forward SIGTERM/SIGINT to child processes
 *   3. Handle SIGWINCH for terminal resize
 *   4. Ignore SIGPIPE to prevent crashes from broken pipes
 *   5. Reap zombie processes
 */

#include <android/log.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <pthread.h>

#include "include/engine.h"

#define TAG "MushroomSignal"

/* ---------- Signal context ---------- */

struct SignalContext {
    bool initialized;
    struct sigaction old_sigchld;
    struct sigaction old_sigint;
    struct sigaction old_sigterm;
    struct sigaction old_sigwinch;

    /* Callback for when a child process exits */
    void (*child_exit_callback)(pid_t pid, int status);
};

static SignalContext* g_signal_ctx = nullptr;

/* ---------- Signal handlers ---------- */

/**
 * SIGCHLD handler: reap zombie processes and notify the engine.
 */
static void sigchld_handler(int sig, siginfo_t* info, void* context) {
    /* Reap all terminated children */
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        __android_log_print(ANDROID_LOG_INFO, TAG,
            "Child process %d exited with status %d", pid, WEXITSTATUS(status));

        if (g_signal_ctx && g_signal_ctx->child_exit_callback) {
            g_signal_ctx->child_exit_callback(pid, status);
        }
    }
}

/**
 * SIGINT/SIGTERM handler: forward termination signals to child processes.
 */
static void sigterm_handler(int sig, siginfo_t* info, void* context) {
    __android_log_print(ANDROID_LOG_INFO, TAG,
        "Received signal %d, forwarding to children", sig);

    /* Send the signal to our process group */
    kill(0, sig);
}

/**
 * SIGWINCH handler: handle terminal resize.
 */
static void sigwinch_handler(int sig, siginfo_t* info, void* context) {
    /* The engine will query the new terminal size on the next frame */
    __android_log_print(ANDROID_LOG_VERBOSE, TAG, "SIGWINCH received");
}

/**
 * SIGPIPE handler: ignore SIGPIPE to prevent crashes.
 */
static void sigpipe_handler(int sig, siginfo_t* info, void* context) {
    /* Silently ignore SIGPIPE */
}

/* ---------- Module lifecycle ---------- */

int signal_handler_init(EngineContext* ctx) {
    __android_log_print(ANDROID_LOG_INFO, TAG, "Signal handler init");

    if (!g_signal_ctx) {
        g_signal_ctx = new (std::nothrow) SignalContext();
        if (!g_signal_ctx) return -1;
    }

    memset(g_signal_ctx, 0, sizeof(SignalContext));

    /* Ignore SIGPIPE by default */
    struct sigaction sa_pipe;
    memset(&sa_pipe, 0, sizeof(sa_pipe));
    sa_pipe.sa_sigaction = sigpipe_handler;
    sa_pipe.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPIPE, &sa_pipe, nullptr);

    /* Set up SIGCHLD handler */
    struct sigaction sa_chld;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_sigaction = sigchld_handler;
    sa_chld.sa_flags = SA_SIGINFO | SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, &g_signal_ctx->old_sigchld);

    /* Set up SIGTERM handler */
    struct sigaction sa_term;
    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_sigaction = sigterm_handler;
    sa_term.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGTERM, &sa_term, &g_signal_ctx->old_sigterm);

    /* Set up SIGINT handler */
    struct sigaction sa_int;
    memset(&sa_int, 0, sizeof(sa_int));
    sa_int.sa_sigaction = sigterm_handler;
    sa_int.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGINT, &sa_int, &g_signal_ctx->old_sigint);

    /* Set up SIGWINCH handler */
    struct sigaction sa_winch;
    memset(&sa_winch, 0, sizeof(sa_winch));
    sa_winch.sa_sigaction = sigwinch_handler;
    sa_winch.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGWINCH, &sa_winch, &g_signal_ctx->old_sigwinch);

    g_signal_ctx->initialized = true;
    ctx->signal_ctx = (void*)g_signal_ctx;

    __android_log_print(ANDROID_LOG_INFO, TAG, "Signal handlers installed");
    return 0;
}

void signal_handler_cleanup(EngineContext* ctx) {
    if (!g_signal_ctx) return;

    /* Restore original signal handlers */
    sigaction(SIGCHLD, &g_signal_ctx->old_sigchld, nullptr);
    sigaction(SIGTERM, &g_signal_ctx->old_sigterm, nullptr);
    sigaction(SIGINT, &g_signal_ctx->old_sigint, nullptr);
    sigaction(SIGWINCH, &g_signal_ctx->old_sigwinch, nullptr);

    /* Restore SIGPIPE to default */
    signal(SIGPIPE, SIG_DFL);

    g_signal_ctx->initialized = false;
    ctx->signal_ctx = nullptr;

    __android_log_print(ANDROID_LOG_INFO, TAG, "Signal handlers restored");
}

/**
 * Register a callback for child process exit events.
 */
void signal_handler_set_child_callback(void (*callback)(pid_t, int)) {
    if (g_signal_ctx) {
        g_signal_ctx->child_exit_callback = callback;
    }
}