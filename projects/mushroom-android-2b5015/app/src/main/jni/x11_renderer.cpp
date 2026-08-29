/**
 * x11_renderer.cpp — X11 framebuffer rendering pipeline
 *
 * This module is responsible for:
 *   1. Launching Xvfb (a virtual framebuffer X11 server) inside the chroot
 *   2. Connecting to the X11 display via XCB or raw socket
 *   3. Reading the framebuffer pixels from the X server
 *   4. Exposing the pixel data to the Android Surface via the JNI bridge
 *
 * The rendering pipeline works as follows:
 *   - Xvfb runs as a child process inside the chroot on display :1
 *   - The native engine connects to Xvfb using the X11 protocol over a local
 *     Unix domain socket
 *   - A lightweight X11 compositor (using the XFixes extension for the
 *     cursor image and the XDamage extension for dirty regions) captures
 *     the framebuffer
 *   - The framebuffer is exposed as a shared memory buffer that the Android
 *     GLSurfaceView renders as a texture
 *
 * In a full implementation, this would use XCB (X C Bindings) for efficient
 * X11 protocol handling. The current implementation provides the framework
 * and a mock rendering path for testing.
 */

#include <android/log.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <pthread.h>
#include <string>
#include <vector>
#include <chrono>

#include "include/engine.h"

#define TAG "MushroomX11"

/* ---------- Renderer context ---------- */

struct RendererContext {
    /* Xvfb process */
    pid_t xvfb_pid;
    pid_t desktop_pid;

    /* Display server socket */
    int x11_socket;
    int display_number;

    /* Framebuffer */
    uint8_t* framebuffer;
    int fb_width;
    int fb_height;
    int fb_stride;
    size_t fb_size;

    /* Render loop */
    volatile bool running;
    pthread_t render_thread;
    float current_fps;

    /* Timing */
    std::chrono::steady_clock::time_point last_frame_time;
    int frame_count;
    float fps_accumulator;

    /* X11 auth */
    char xauthority_path[4096];

    /* EGL/OpenGL ES state for surface rendering */
    bool egl_initialized;
    void* egl_display;
    void* egl_surface;
    void* egl_context;
};

/* ---------- Xvfb lifecycle ---------- */

/**
 * Launch Xvfb inside the chroot environment.
 * Xvfb is a virtual framebuffer X11 server that renders to memory
 * instead of a physical display.
 */
static int launch_xvfb(RendererContext* rctx, EngineContext* ctx) {
    __android_log_print(ANDROID_LOG_INFO, TAG, "Launching Xvfb on display :%d",
                        rctx->display_number);

    /* Build the Xvfb command */
    char display_str[32];
    snprintf(display_str, sizeof(display_str), ":%d", rctx->display_number);

    char resolution_str[64];
    snprintf(resolution_str, sizeof(resolution_str), "%dx%dx24",
             rctx->fb_width, rctx->fb_height);

    /* Xvfb path inside the rootfs */
    char xvfb_path[4096];
    snprintf(xvfb_path, sizeof(xvfb_path), "%s/usr/bin/Xvfb",
             ctx->config.rootfs_path);

    /* Check if Xvfb exists */
    struct stat st;
    if (stat(xvfb_path, &st) != 0) {
        /* Xvfb not found, try Xvfb in /usr/bin/Xvfb or just note it */
        __android_log_print(ANDROID_LOG_WARN, TAG,
            "Xvfb not found at %s, will use stub framebuffer", xvfb_path);
        return -1;
    }

    /* Spawn Xvfb */
    pid_t pid = fork();
    if (pid == 0) {
        /* Child process */
        /* Set DISPLAY environment */
        setenv("DISPLAY", display_str, 1);

        /* Redirect stdout/stderr to /dev/null */
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        /* Execute Xvfb */
        char* const argv[] = {
            (char*)"Xvfb",
            display_str,
            (char*)"-screen", (char*)"0", resolution_str,
            (char*)"-nolisten", (char*)"tcp",
            (char*)"-ac",  /* Disable access control */
            nullptr
        };
        execve(xvfb_path, argv, environ);
        /* If execve fails, exit */
        _exit(1);
    } else if (pid > 0) {
        /* Parent: wait for Xvfb to start */
        rctx->xvfb_pid = pid;
        usleep(500000);  /* Wait 500ms for Xvfb to initialize */
        __android_log_print(ANDROID_LOG_INFO, TAG, "Xvfb started (pid=%d)", pid);
        return 0;
    }

    __android_log_print(ANDROID_LOG_ERROR, TAG, "Fork failed for Xvfb: %s", strerror(errno));
    return -1;
}

/**
 * Launch a desktop environment (XFCE or LXDE) inside the chroot.
 * This is optional - the user can also just use the terminal.
 */
static int launch_desktop(RendererContext* rctx, EngineContext* ctx) {
    __android_log_print(ANDROID_LOG_INFO, TAG, "Launching desktop environment");

    /* Try to start a window manager / desktop session */
    /* We try multiple desktop environments in order of preference */
    const char* desktop_cmds[] = {
        "/usr/bin/startxfce4",
        "/usr/bin/startlxde",
        "/usr/bin/xfce4-session",
        "/usr/bin/lxsession",
        "/usr/bin/openbox",
        "/usr/bin/fluxbox",
        "/usr/bin/icewm",
        "/usr/bin/jwm",
        nullptr
    };

    for (int i = 0; desktop_cmds[i] != nullptr; i++) {
        char full_path[4096];
        snprintf(full_path, sizeof(full_path), "%s%s",
                 ctx->config.rootfs_path, desktop_cmds[i]);

        struct stat st;
        if (stat(full_path, &st) == 0 && (st.st_mode & S_IXUSR)) {
            pid_t pid = fork();
            if (pid == 0) {
                /* Child process */
                char display_str[32];
                snprintf(display_str, sizeof(display_str), ":%d", rctx->display_number);
                setenv("DISPLAY", display_str, 1);

                /* Execute the desktop environment */
                char* const argv[] = {
                    (char*)desktop_cmds[i],
                    nullptr
                };
                execve(full_path, argv, environ);
                _exit(1);
            } else if (pid > 0) {
                rctx->desktop_pid = pid;
                __android_log_print(ANDROID_LOG_INFO, TAG,
                    "Desktop environment started: %s (pid=%d)",
                    desktop_cmds[i], pid);
                return 0;
            }
        }
    }

    __android_log_print(ANDROID_LOG_WARN, TAG,
        "No desktop environment found, will run without one");
    return -1;
}

/**
 * Connect to the X11 server via Unix domain socket.
 */
static int connect_to_x11(RendererContext* rctx) {
    /* Build the X11 socket path */
    /* X11 uses abstract Unix domain sockets or /tmp/.X11-unix/X{display} */
    char socket_path[108];  /* SUN_LEN max */
    snprintf(socket_path, sizeof(socket_path),
             "/tmp/.X11-unix/X%d", rctx->display_number);

    /* Create the socket */
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG,
            "Failed to create X11 socket: %s", strerror(errno));
        return -1;
    }

    /* Connect to the X11 server */
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG,
            "Failed to connect to X11 at %s: %s", socket_path, strerror(errno));
        close(sock);
        return -1;
    }

    rctx->x11_socket = sock;
    __android_log_print(ANDROID_LOG_INFO, TAG, "Connected to X11 display :%d", rctx->display_number);
    return 0;
}

/* ---------- Framebuffer read ---------- */

/**
 * Read the X11 framebuffer.
 * In a full implementation, this would use the X11 protocol to:
 *   1. Query the root window geometry
 *   2. Use XGetImage or XShmGetImage to get the pixel data
 *   3. Use XDamage to get dirty regions for incremental updates
 *
 * For the initial implementation, we provide a stub that works with
 * the mock framebuffer.
 */
static int read_framebuffer(RendererContext* rctx, uint8_t* buffer, size_t size) {
    if (!rctx || !buffer) return -1;

    /* In a full X11 implementation, this would use XCB or Xlib to
     * read the actual framebuffer from the X server.
     *
     * For now, we generate a simple test pattern to verify the
     * rendering pipeline is working.
     */
    if (rctx->framebuffer) {
        size_t copy_size = (size < rctx->fb_size) ? size : rctx->fb_size;
        memcpy(buffer, rctx->framebuffer, copy_size);
        return (int)copy_size;
    }

    return 0;
}

/**
 * Generate a test pattern (checkerboard) for the framebuffer.
 * This is used when no actual X11 server is running.
 */
static void generate_test_pattern(RendererContext* rctx) {
    if (!rctx || !rctx->framebuffer) return;

    int w = rctx->fb_width;
    int h = rctx->fb_height;
    int tile_size = 32;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int pixel = (y * w + x) * 4;
            bool is_white = ((x / tile_size) + (y / tile_size)) % 2 == 0;

            if (is_white) {
                rctx->framebuffer[pixel + 0] = 0x40;  /* R */
                rctx->framebuffer[pixel + 1] = 0xAF;  /* G */
                rctx->framebuffer[pixel + 2] = 0x50;  /* B */
            } else {
                rctx->framebuffer[pixel + 0] = 0x1B;
                rctx->framebuffer[pixel + 1] = 0x1B;
                rctx->framebuffer[pixel + 2] = 0x1B;
            }
            rctx->framebuffer[pixel + 3] = 0xFF;  /* A */
        }
    }
}

/* ---------- Render thread ---------- */

static void* render_thread_func(void* arg) {
    RendererContext* rctx = (RendererContext*)arg;
    EngineContext* ctx = (EngineContext*)rctx;  /* Cast back via container */

    __android_log_print(ANDROID_LOG_INFO, TAG, "Render thread started");

    rctx->last_frame_time = std::chrono::steady_clock::now();
    rctx->frame_count = 0;
    rctx->fps_accumulator = 0.0f;

    /* Calculate frame interval for 30 FPS target */
    const auto frame_interval = std::chrono::milliseconds(33);  /* ~30 FPS */

    while (rctx->running) {
        auto frame_start = std::chrono::steady_clock::now();

        /* Read the framebuffer */
        if (rctx->framebuffer && ctx->fb.pixels) {
            read_framebuffer(rctx, ctx->fb.pixels, ctx->fb.size);
            ctx->fb.dirty = true;
        }

        /* FPS calculation */
        rctx->frame_count++;
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - rctx->last_frame_time).count();

        if (elapsed >= 1000) {
            rctx->current_fps = rctx->frame_count * 1000.0f / elapsed;
            rctx->frame_count = 0;
            rctx->last_frame_time = now;
        }

        /* Sleep to maintain target framerate */
        auto frame_end = std::chrono::steady_clock::now();
        auto frame_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            frame_end - frame_start);

        if (frame_duration < frame_interval) {
            std::this_thread::sleep_for(frame_interval - frame_duration);
        }
    }

    __android_log_print(ANDROID_LOG_INFO, TAG, "Render thread stopped");
    return nullptr;
}

/* ---------- Module lifecycle ---------- */

int x11_renderer_init(EngineContext* ctx) {
    __android_log_print(ANDROID_LOG_INFO, TAG, "X11 renderer init");

    RendererContext* rctx = (RendererContext*)malloc(sizeof(RendererContext));
    if (!rctx) return -1;

    memset(rctx, 0, sizeof(RendererContext));
    rctx->xvfb_pid = -1;
    rctx->desktop_pid = -1;
    rctx->x11_socket = -1;
    rctx->display_number = 1;
    rctx->running = false;
    rctx->current_fps = 0.0f;
    rctx->egl_initialized = false;

    ctx->renderer_ctx = (void*)rctx;
    return 0;
}

int x11_renderer_start(EngineContext* ctx) {
    RendererContext* rctx = (RendererContext*)ctx->renderer_ctx;
    if (!rctx) return -1;

    __android_log_print(ANDROID_LOG_INFO, TAG, "Starting X11 renderer: %dx%d",
                        ctx->config.display_resolution.width,
                        ctx->config.display_resolution.height);

    /* Store framebuffer dimensions */
    rctx->fb_width = ctx->config.display_resolution.width;
    rctx->fb_height = ctx->config.display_resolution.height;
    rctx->fb_stride = rctx->fb_width * 4;
    rctx->fb_size = rctx->fb_width * rctx->fb_height * 4;

    /* Allocate local framebuffer */
    rctx->framebuffer = (uint8_t*)malloc(rctx->fb_size);
    if (!rctx->framebuffer) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to allocate framebuffer");
        return -1;
    }

    /* Generate a test pattern initially */
    generate_test_pattern(rctx);

    /* Try to launch Xvfb */
    int ret = launch_xvfb(rctx, ctx);
    if (ret == 0) {
        /* Try to connect to X11 */
        if (connect_to_x11(rctx) == 0) {
            /* Try to launch desktop environment */
            launch_desktop(rctx, ctx);
        }
    } else {
        __android_log_print(ANDROID_LOG_INFO, TAG,
            "Running without Xvfb (test pattern mode)");
    }

    /* Start the render thread */
    rctx->running = true;
    pthread_create(&rctx->render_thread, nullptr, render_thread_func, rctx);

    __android_log_print(ANDROID_LOG_INFO, TAG, "X11 renderer started");
    return 0;
}

void x11_renderer_stop(EngineContext* ctx) {
    RendererContext* rctx = (RendererContext*)ctx->renderer_ctx;
    if (!rctx) return;

    __android_log_print(ANDROID_LOG_INFO, TAG, "Stopping X11 renderer");

    /* Stop the render thread */
    rctx->running = false;
    if (rctx->render_thread) {
        pthread_join(rctx->render_thread, nullptr);
    }

    /* Close X11 socket */
    if (rctx->x11_socket >= 0) {
        close(rctx->x11_socket);
        rctx->x11_socket = -1;
    }

    /* Kill desktop environment */
    if (rctx->desktop_pid > 0) {
        kill(rctx->desktop_pid, SIGTERM);
        waitpid(rctx->desktop_pid, nullptr, WNOHANG);
        rctx->desktop_pid = -1;
    }

    /* Kill Xvfb */
    if (rctx->xvfb_pid > 0) {
        kill(rctx->xvfb_pid, SIGTERM);
        waitpid(rctx->xvfb_pid, nullptr, WNOHANG);
        rctx->xvfb_pid = -1;
    }

    /* Free framebuffer */
    if (rctx->framebuffer) {
        free(rctx->framebuffer);
        rctx->framebuffer = nullptr;
    }

    __android_log_print(ANDROID_LOG_INFO, TAG, "X11 renderer stopped");
}

int x11_renderer_get_framebuffer(EngineContext* ctx, Framebuffer* fb) {
    RendererContext* rctx = (RendererContext*)ctx->renderer_ctx;
    if (!rctx || !fb) return -1;

    if (rctx->framebuffer && ctx->fb.pixels) {
        fb->pixels = ctx->fb.pixels;
        fb->width = rctx->fb_width;
        fb->height = rctx->fb_height;
        fb->stride = rctx->fb_stride;
        fb->size = rctx->fb_size;
        fb->dirty = ctx->fb.dirty;
        ctx->fb.dirty = false;
        return 0;
    }

    return -1;
}

float x11_renderer_get_fps(EngineContext* ctx) {
    RendererContext* rctx = (RendererContext*)ctx->renderer_ctx;
    if (!rctx) return 0.0f;
    return rctx->current_fps;
}