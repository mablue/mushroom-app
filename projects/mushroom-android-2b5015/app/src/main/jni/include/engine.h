#ifndef MUSHROOM_ENGINE_H
#define MUSHROOM_ENGINE_H

/**
 * Mushroom Engine — Core header
 * ---------------------------------
 * This header defines the shared constants, types, and function declarations
 * used across all native engine modules.
 *
 * Architecture:
 *   The engine is organized into independent modules that communicate through
 *   the EngineContext struct. Each module has a single init/start/stop lifecycle.
 */

#include <stdint.h>
#include <stdbool.h>
#include <jni.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Version ---------- */
#define MUSHROOM_ENGINE_VERSION "1.0.0"

/* ---------- Constants ---------- */
#define MAX_RESOLUTION_WIDTH  3840
#define MAX_RESOLUTION_HEIGHT 2160
#define MIN_RESOLUTION_WIDTH  640
#define MIN_RESOLUTION_HEIGHT 480
#define MAX_MEMORY_MB         8192
#define MIN_MEMORY_MB         512
#define MAX_ROOTFS_PATH       4096
#define MAX_ERROR_MSG         1024
#define FB_BPP                4  /* RGBA8888 bytes per pixel */

/* ---------- Error codes ---------- */
typedef enum {
    ENGINE_OK = 0,
    ERR_INIT_FAILED = -1,
    ERR_ROOTFS_MISSING = -2,
    ERR_ROOTFS_EXTRACT = -3,
    ERR_MOUNT_FAILED = -4,
    ERR_SECCOMP_FAILED = -5,
    ERR_XVFB_FAILED = -6,
    ERR_PTY_FAILED = -7,
    ERR_SURFACE_INVALID = -8,
    ERR_OUT_OF_MEMORY = -9,
    ERR_INTERNAL = -99,
} EngineError;

/* ---------- Engine state ---------- */
typedef enum {
    STATE_UNINITIALIZED = 0,
    STATE_INITIALIZING,
    STATE_READY,
    STATE_RUNNING,
    STATE_STOPPING,
    STATE_STOPPED,
    STATE_ERROR,
} EngineState;

/* ---------- Resolution ---------- */
typedef struct {
    int width;
    int height;
} Resolution;

/* ---------- Virtual mount entry ---------- */
typedef struct {
    char source[MAX_ROOTFS_PATH];   /* host path */
    char target[MAX_ROOTFS_PATH];   /* guest path */
    char fstype[64];
    bool is_virtual;                /* true = emulated, not bind-mounted */
} MountEntry;

/* ---------- Framebuffer ---------- */
typedef struct {
    int width;
    int height;
    int stride;          /* bytes per row */
    uint8_t* pixels;     /* RGBA8888 pixel data */
    size_t size;         /* total buffer size */
    bool dirty;          /* true when new pixels are available */
} Framebuffer;

/* ---------- Engine configuration ---------- */
typedef struct {
    char engine_path[MAX_ROOTFS_PATH];
    char rootfs_path[MAX_ROOTFS_PATH];
    Resolution display_resolution;
    int memory_limit_mb;
    bool enable_seccomp;
    bool enable_x11;
    bool enable_pty;
    char rootfs_url[512];
    char rootfs_sha256[65];
} EngineConfig;

/* ---------- Engine context (global state) ---------- */
typedef struct {
    EngineState state;
    EngineError last_error;
    char error_msg[MAX_ERROR_MSG];

    EngineConfig config;

    /* Module contexts */
    void* interceptor_ctx;    /* syscall_interceptor */
    void* mount_ctx;          /* mount_ns */
    void* renderer_ctx;       /* x11_renderer */
    void* pty_ctx;            /* pty_manager */
    void* proc_ctx;           /* process_manager */
    void* signal_ctx;         /* signal_handler */

    /* Framebuffer shared with the render loop */
    Framebuffer fb;

    /* Render loop control */
    volatile bool render_loop_active;
    volatile int target_fps;

    /* JNI environment for surface attachment */
    JavaVM* jvm;
    jobject surface_texture;
    jobject surface;
} EngineContext;

/* ---------- Global engine instance ---------- */
extern EngineContext g_engine;

/* ---------- Module lifecycle functions ---------- */

/* syscall_interceptor */
int interceptor_init(EngineContext* ctx);
int interceptor_start(EngineContext* ctx);
void interceptor_stop(EngineContext* ctx);

/* posix_wrappers */
int posix_wrappers_init(EngineContext* ctx);
void* real_dlsym(void* handle, const char* symbol);

/* seccomp_policy */
int seccomp_init(EngineContext* ctx);
int seccomp_apply(EngineContext* ctx);
void seccomp_cleanup(EngineContext* ctx);

/* mount_ns */
int mount_ns_init(EngineContext* ctx);
int mount_ns_start(EngineContext* ctx);
void mount_ns_stop(EngineContext* ctx);

/* fakechroot */
int fakechroot_init(EngineContext* ctx);
char* fakechroot_translate_path(EngineContext* ctx, const char* path);

/* x11_renderer */
int x11_renderer_init(EngineContext* ctx);
int x11_renderer_start(EngineContext* ctx);
void x11_renderer_stop(EngineContext* ctx);
int x11_renderer_get_framebuffer(EngineContext* ctx, Framebuffer* fb);
float x11_renderer_get_fps(EngineContext* ctx);

/* rootfs_manager */
int rootfs_manager_init(EngineContext* ctx);
int rootfs_manager_download(EngineContext* ctx, const char* url, const char* sha256);
int rootfs_manager_extract(EngineContext* ctx);
bool rootfs_manager_is_extracted(EngineContext* ctx);

/* pty_manager */
int pty_manager_init(EngineContext* ctx);
int pty_manager_open(EngineContext* ctx);
int pty_manager_write(EngineContext* ctx, int fd, const uint8_t* data, size_t len);
int pty_manager_read(EngineContext* ctx, int fd, uint8_t* buffer, size_t max_len);
void pty_manager_close(EngineContext* ctx, int fd);

/* signal_handler */
int signal_handler_init(EngineContext* ctx);
void signal_handler_cleanup(EngineContext* ctx);

/* process_manager */
int process_manager_init(EngineContext* ctx);
int process_manager_spawn(EngineContext* ctx, const char* path, char* const argv[], int* out_pid);
int process_manager_wait(EngineContext* ctx, int pid);

/* ---------- Utility functions ---------- */
void engine_set_error(EngineContext* ctx, EngineError err, const char* msg);
const char* engine_state_str(EngineState state);
const char* engine_error_str(EngineError err);

#ifdef __cplusplus
}
#endif

#endif /* MUSHROOM_ENGINE_H */