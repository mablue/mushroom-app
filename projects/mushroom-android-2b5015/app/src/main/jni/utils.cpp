/**
 * utils.cpp — Utility functions for the Mushroom engine
 */

#include <android/log.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include "include/engine.h"

#define TAG "MushroomUtils"

EngineContext g_engine;

void engine_set_error(EngineContext* ctx, EngineError err, const char* msg) {
    if (!ctx) return;
    ctx->last_error = err;
    if (msg) {
        strncpy(ctx->error_msg, msg, MAX_ERROR_MSG - 1);
    } else {
        ctx->error_msg[0] = '\0';
    }
    __android_log_print(ANDROID_LOG_ERROR, TAG, "Error [%d]: %s", err, msg ? msg : "");
}

const char* engine_state_str(EngineState state) {
    switch (state) {
        case STATE_UNINITIALIZED: return "UNINITIALIZED";
        case STATE_INITIALIZING:  return "INITIALIZING";
        case STATE_READY:         return "READY";
        case STATE_RUNNING:       return "RUNNING";
        case STATE_STOPPING:      return "STOPPING";
        case STATE_STOPPED:       return "STOPPED";
        case STATE_ERROR:         return "ERROR";
        default:                  return "UNKNOWN";
    }
}

const char* engine_error_str(EngineError err) {
    switch (err) {
        case ENGINE_OK:           return "OK";
        case ERR_INIT_FAILED:     return "Initialization failed";
        case ERR_ROOTFS_MISSING:  return "RootFS not found";
        case ERR_ROOTFS_EXTRACT:  return "RootFS extraction failed";
        case ERR_MOUNT_FAILED:    return "Mount failed";
        case ERR_SECCOMP_FAILED:  return "Seccomp policy failed";
        case ERR_XVFB_FAILED:     return "Xvfb failed";
        case ERR_PTY_FAILED:      return "PTY allocation failed";
        case ERR_SURFACE_INVALID: return "Invalid surface";
        case ERR_OUT_OF_MEMORY:   return "Out of memory";
        case ERR_INTERNAL:        return "Internal error";
        default:                  return "Unknown error";
    }
}