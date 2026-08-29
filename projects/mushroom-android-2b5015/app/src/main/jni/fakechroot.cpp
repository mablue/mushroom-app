/**
 * fakechroot.cpp — Chroot simulation without root privileges
 *
 * This module implements the core path translation logic that makes processes
 * believe they are running in a chroot environment, without requiring actual
 * root permissions or the chroot() syscall.
 *
 * The strategy:
 *   1. The RootFS is extracted to a host directory (e.g., /data/data/.../rootfs)
 *   2. All file operations are intercepted by the LD_PRELOAD layer
 *   3. The interceptor calls fakechroot_translate_path() to convert guest paths
 *      to host paths
 *   4. Guest "/" becomes host "/data/data/.../rootfs"
 *   5. Guest "/usr/bin/ls" becomes host "/data/data/.../rootfs/usr/bin/ls"
 *   6. Special paths (/proc, /sys, /dev, /tmp) are redirected to the
 *      virtual mount points created by mount_ns
 *
 * This approach is inspired by the fakechroot project but optimized for
 * Android's environment and integrated directly into the interceptor.
 */

#include <android/log.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <string>
#include <vector>
#include <map>

#include "include/engine.h"

#define TAG "MushroomFakeChroot"

/* ---------- Fakechroot context ---------- */

struct FakechrootContext {
    /* RootFS path */
    std::string rootfs_path;

    /* Current working directory (guest view) */
    std::string cwd;

    /* Path prefix mappings */
    struct PathMap {
        std::string guest_prefix;
        std::string host_prefix;
    };
    std::vector<PathMap> path_mappings;

    /* Whether path translation is enabled */
    bool enabled;
};

static FakechrootContext* g_fctx = nullptr;

/* ---------- Path translation implementation ---------- */

/**
 * Translate a guest filesystem path to the host equivalent.
 *
 * Rules:
 *   1. If path starts with the rootfs prefix, it's already a host path
 *   2. If path is absolute (/), prepend rootfs_path
 *   3. If path is relative, prepend cwd then rootfs_path
 *   4. Special paths (/proc/self/..., /sys/..., /dev/...) are mapped to
 *      the virtual directories inside the rootfs
 *   5. Paths starting with /data/, /system/, /vendor/, /apex/ are passed
 *      through unchanged (Android system paths)
 */
char* fakechroot_translate_path(EngineContext* ctx, const char* path) {
    if (!ctx || !path || !g_fctx || !g_fctx->enabled) {
        return strdup(path ? path : "");
    }

    std::string input(path);

    /* Don't translate empty paths */
    if (input.empty()) {
        return strdup("");
    }

    /* Check if path is already a host path */
    if (input.find(g_fctx->rootfs_path) == 0) {
        return strdup(path);
    }

    /* Don't translate Android system paths */
    if (input.find("/data/") == 0 || input.find("/system/") == 0 ||
        input.find("/vendor/") == 0 || input.find("/apex/") == 0 ||
        input.find("/mnt/") == 0 || input.find("/storage/") == 0) {
        return strdup(path);
    }

    /* Don't translate /proc/self/...  or /proc/1/... (these are emulated) */
    if (input.find("/proc/") == 0) {
        /* Map to rootfs/proc */
        std::string result = g_fctx->rootfs_path + input;
        return strdup(result.c_str());
    }

    /* Don't translate /sys/... (these are emulated) */
    if (input.find("/sys/") == 0) {
        std::string result = g_fctx->rootfs_path + input;
        return strdup(result.c_str());
    }

    /* Don't translate /dev/... (these are emulated) */
    if (input.find("/dev/") == 0) {
        std::string result = g_fctx->rootfs_path + input;
        return strdup(result.c_str());
    }

    /* Handle /tmp */
    if (input.find("/tmp") == 0) {
        std::string result = g_fctx->rootfs_path + input;
        return strdup(result.c_str());
    }

    /* Handle absolute paths */
    if (input[0] == '/') {
        if (input == "/") {
            return strdup(g_fctx->rootfs_path.c_str());
        }
        std::string result = g_fctx->rootfs_path + input;
        return strdup(result.c_str());
    }

    /* Handle relative paths: prepend cwd, then rootfs */
    if (g_fctx->cwd.empty() || g_fctx->cwd == "/") {
        std::string result = g_fctx->rootfs_path + "/" + input;
        return strdup(result.c_str());
    }

    std::string result = g_fctx->rootfs_path + g_fctx->cwd + "/" + input;

    /* Normalize the path (remove /../ and /./ sequences) */
    /* This is a simple normalization - a full implementation would
     * resolve all .. and . components properly */
    {
        std::string normalized;
        size_t pos = 0;
        while (pos < result.length()) {
            if (result.substr(pos, 3) == "/../") {
                /* Go up one directory */
                size_t prev_slash = normalized.rfind('/', normalized.length() - 2);
                if (prev_slash != std::string::npos) {
                    normalized.erase(prev_slash);
                }
                pos += 3;
            } else if (result.substr(pos, 2) == "/." &&
                       (pos + 2 >= result.length() || result[pos + 2] == '/')) {
                /* Skip /./ */
                pos += 2;
            } else {
                normalized += result[pos];
                pos++;
            }
        }
        result = normalized;
    }

    return strdup(result.c_str());
}

/**
 * Set the current working directory (guest view).
 */
void fakechroot_set_cwd(const char* cwd) {
    if (g_fctx) {
        g_fctx->cwd = cwd ? std::string(cwd) : "/";
    }
}

/**
 * Get the current working directory (guest view).
 */
const char* fakechroot_get_cwd() {
    if (!g_fctx) return "/";
    return g_fctx->cwd.c_str();
}

/**
 * Add a custom path mapping.
 * This allows mapping specific guest paths to different host paths.
 * For example, /home → /data/.../rootfs/home.
 */
void fakechroot_add_mapping(const char* guest, const char* host) {
    if (!g_fctx || !guest || !host) return;
    FakechrootContext::PathMap mapping;
    mapping.guest_prefix = guest;
    mapping.host_prefix = host;
    g_fctx->path_mappings.push_back(mapping);
}

/* ---------- Module lifecycle ---------- */

int fakechroot_init(EngineContext* ctx) {
    __android_log_print(ANDROID_LOG_INFO, TAG, "Fakechroot init");

    if (!g_fctx) {
        g_fctx = new (std::nothrow) FakechrootContext();
        if (!g_fctx) return -1;
    }

    g_fctx->rootfs_path = ctx->config.rootfs_path;
    g_fctx->cwd = "/";
    g_fctx->enabled = false;

    /* Add default path mappings */
    fakechroot_add_mapping("/bin", (g_fctx->rootfs_path + "/bin").c_str());
    fakechroot_add_mapping("/usr", (g_fctx->rootfs_path + "/usr").c_str());
    fakechroot_add_mapping("/etc", (g_fctx->rootfs_path + "/etc").c_str());
    fakechroot_add_mapping("/lib", (g_fctx->rootfs_path + "/lib").c_str());
    fakechroot_add_mapping("/opt", (g_fctx->rootfs_path + "/opt").c_str());
    fakechroot_add_mapping("/var", (g_fctx->rootfs_path + "/var").c_str());
    fakechroot_add_mapping("/home", (g_fctx->rootfs_path + "/home").c_str());
    fakechroot_add_mapping("/root", (g_fctx->rootfs_path + "/root").c_str());
    fakechroot_add_mapping("/sbin", (g_fctx->rootfs_path + "/sbin").c_str());
    fakechroot_add_mapping("/run", (g_fctx->rootfs_path + "/run").c_str());

    __android_log_print(ANDROID_LOG_INFO, TAG, "Fakechroot initialized with rootfs: %s",
                        g_fctx->rootfs_path.c_str());
    return 0;
}

/**
 * Enable path translation. Called when the engine enters the running state.
 */
void fakechroot_enable(EngineContext* ctx) {
    if (g_fctx) {
        g_fctx->enabled = true;
        __android_log_print(ANDROID_LOG_INFO, TAG, "Fakechroot path translation enabled");
    }
}

/**
 * Disable path translation.
 */
void fakechroot_disable(EngineContext* ctx) {
    if (g_fctx) {
        g_fctx->enabled = false;
        __android_log_print(ANDROID_LOG_INFO, TAG, "Fakechroot path translation disabled");
    }
}