/**
 * jni_bridge.cpp — JNI bridge between Kotlin and the native C++ engine.
 *
 * This file implements all JNI exports declared in NativeEngine.kt.
 * The bridge is the single entry point from Android Java/Kotlin into the
 * native engine, which manages:
 *   - Syscall interception (LD_PRELOAD wrappers)
 *   - RootFS lifecycle (download, verify, extract)
 *   - Mount namespace (virtual /proc, /sys, /dev, /tmp)
 *   - Xvfb/EGL rendering pipeline
 *   - PTY-based terminal emulation
 *   - Seccomp-BPF policy enforcement
 */

#include <jni.h>
#include <android/log.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>

#include "include/engine.h"

#define TAG "MushroomJNI"

/* Global engine instance */
EngineContext g_engine;

/* JVM reference for surface attachment */
static JavaVM* g_jvm = nullptr;
static pthread_mutex_t g_jni_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Helper: get JNI environment for the current thread */
static JNIEnv* get_jni_env() {
    JNIEnv* env = nullptr;
    if (g_jvm && g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK) {
        return env;
    }
    /* Try to attach if this is a native thread */
    if (g_jvm && g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
        return env;
    }
    return nullptr;
}

/* ---------- JNI implementations ---------- */

extern "C" JNIEXPORT jboolean JNICALL
Java_com_mushroom_android_NativeEngine_nativeInit(
    JNIEnv* env, jobject /*thiz*/,
    jstring engine_path, jstring rootfs_path)
{
    pthread_mutex_lock(&g_jni_mutex);

    __android_log_print(ANDROID_LOG_INFO, TAG, "nativeInit called");

    /* Store JVM reference */
    env->GetJavaVM(&g_jvm);
    g_engine.jvm = g_jvm;

    /* Clear previous state */
    memset(&g_engine, 0, sizeof(EngineContext));
    g_engine.state = STATE_UNINITIALIZED;
    g_engine.last_error = ENGINE_OK;
    g_engine.target_fps = 30;

    /* Copy paths from JNI strings */
    const char* ep = env->GetStringUTFChars(engine_path, nullptr);
    const char* rp = env->GetStringUTFChars(rootfs_path, nullptr);

    if (!ep || !rp) {
        if (ep) env->ReleaseStringUTFChars(engine_path, ep);
        if (rp) env->ReleaseStringUTFChars(rootfs_path, rp);
        pthread_mutex_unlock(&g_jni_mutex);
        return JNI_FALSE;
    }

    strncpy(g_engine.config.engine_path, ep, MAX_ROOTFS_PATH - 1);
    strncpy(g_engine.config.rootfs_path, rp, MAX_ROOTFS_PATH - 1);

    /* Default resolution */
    g_engine.config.display_resolution.width = 1024;
    g_engine.config.display_resolution.height = 768;
    g_engine.config.memory_limit_mb = 2048;
    g_engine.config.enable_seccomp = true;
    g_engine.config.enable_x11 = true;
    g_engine.config.enable_pty = true;

    env->ReleaseStringUTFChars(engine_path, ep);
    env->ReleaseStringUTFChars(rootfs_path, rp);

    /* Initialize all modules */
    g_engine.state = STATE_INITIALIZING;

    int ret = 0;

    ret = interceptor_init(&g_engine);
    if (ret != 0) {
        engine_set_error(&g_engine, ERR_INIT_FAILED, "Interceptor init failed");
        g_engine.state = STATE_ERROR;
        pthread_mutex_unlock(&g_jni_mutex);
        return JNI_FALSE;
    }

    ret = rootfs_manager_init(&g_engine);
    if (ret != 0) {
        engine_set_error(&g_engine, ERR_INIT_FAILED, "RootFS manager init failed");
        g_engine.state = STATE_ERROR;
        pthread_mutex_unlock(&g_jni_mutex);
        return JNI_FALSE;
    }

    ret = mount_ns_init(&g_engine);
    if (ret != 0) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "Mount namespace init non-fatal: %d", ret);
    }

    ret = x11_renderer_init(&g_engine);
    if (ret != 0) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "X11 renderer init non-fatal: %d", ret);
    }

    ret = pty_manager_init(&g_engine);
    if (ret != 0) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "PTY manager init non-fatal: %d", ret);
    }

    ret = signal_handler_init(&g_engine);
    if (ret != 0) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "Signal handler init non-fatal: %d", ret);
    }

    ret = process_manager_init(&g_engine);
    if (ret != 0) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "Process manager init non-fatal: %d", ret);
    }

    g_engine.state = STATE_READY;
    __android_log_print(ANDROID_LOG_INFO, TAG, "Engine initialized, state=READY");

    pthread_mutex_unlock(&g_jni_mutex);
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_mushroom_android_NativeEngine_nativeStart(
    JNIEnv* env, jobject /*thiz*/,
    jint width, jint height, jint mem_limit_mb)
{
    pthread_mutex_lock(&g_jni_mutex);

    __android_log_print(ANDROID_LOG_INFO, TAG, "nativeStart: %dx%d, %dMB", width, height, mem_limit_mb);

    if (g_engine.state != STATE_READY) {
        engine_set_error(&g_engine, ERR_INIT_FAILED, "Engine not initialized");
        pthread_mutex_unlock(&g_jni_mutex);
        return JNI_FALSE;
    }

    /* Update configuration */
    g_engine.config.display_resolution.width = (int)width;
    g_engine.config.display_resolution.height = (int)height;
    g_engine.config.memory_limit_mb = (int)mem_limit_mb;

    /* Allocate framebuffer */
    int fb_size = width * height * FB_BPP;
    g_engine.fb.width = width;
    g_engine.fb.height = height;
    g_engine.fb.stride = width * FB_BPP;
    g_engine.fb.size = fb_size;
    g_engine.fb.pixels = (uint8_t*)malloc(fb_size);
    if (!g_engine.fb.pixels) {
        engine_set_error(&g_engine, ERR_OUT_OF_MEMORY, "Framebuffer allocation failed");
        g_engine.state = STATE_ERROR;
        pthread_mutex_unlock(&g_jni_mutex);
        return JNI_FALSE;
    }
    memset(g_engine.fb.pixels, 0, fb_size);
    g_engine.fb.dirty = false;

    /* 1. Apply seccomp policy */
    if (g_engine.config.enable_seccomp) {
        int ret = seccomp_apply(&g_engine);
        if (ret != 0) {
            __android_log_print(ANDROID_LOG_WARN, TAG, "Seccomp apply (non-fatal): %d", ret);
        }
    }

    /* 2. Create mount namespace */
    int ret = mount_ns_start(&g_engine);
    if (ret != 0) {
        engine_set_error(&g_engine, ERR_MOUNT_FAILED, "Mount namespace start failed");
        g_engine.state = STATE_ERROR;
        pthread_mutex_unlock(&g_jni_mutex);
        return JNI_FALSE;
    }

    /* 3. Start X11 renderer */
    ret = x11_renderer_start(&g_engine);
    if (ret != 0) {
        engine_set_error(&g_engine, ERR_XVFB_FAILED, "X11 renderer start failed");
        g_engine.state = STATE_ERROR;
        pthread_mutex_unlock(&g_jni_mutex);
        return JNI_FALSE;
    }

    /* 4. Start interceptor */
    ret = interceptor_start(&g_engine);
    if (ret != 0) {
        __android_log_print(ANDROID_LOG_WARN, TAG, "Interceptor start (non-fatal): %d", ret);
    }

    g_engine.state = STATE_RUNNING;
    g_engine.render_loop_active = true;

    __android_log_print(ANDROID_LOG_INFO, TAG, "Engine started, state=RUNNING");

    pthread_mutex_unlock(&g_jni_mutex);
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_mushroom_android_NativeEngine_nativeStop(JNIEnv* env, jobject /*thiz*/)
{
    pthread_mutex_lock(&g_jni_mutex);

    __android_log_print(ANDROID_LOG_INFO, TAG, "nativeStop");
    g_engine.state = STATE_STOPPING;
    g_engine.render_loop_active = false;

    /* Stop modules in reverse order */
    x11_renderer_stop(&g_engine);
    mount_ns_stop(&g_engine);
    interceptor_stop(&g_engine);
    signal_handler_cleanup(&g_engine);

    /* Free framebuffer */
    if (g_engine.fb.pixels) {
        free(g_engine.fb.pixels);
        g_engine.fb.pixels = nullptr;
    }
    g_engine.fb.size = 0;

    /* Detach surface */
    if (g_engine.surface) {
        JNIEnv* jni_env = get_jni_env();
        if (jni_env) {
            jni_env->DeleteGlobalRef(g_engine.surface);
        }
        g_engine.surface = nullptr;
    }
    if (g_engine.surface_texture) {
        JNIEnv* jni_env = get_jni_env();
        if (jni_env) {
            jni_env->DeleteGlobalRef(g_engine.surface_texture);
        }
        g_engine.surface_texture = nullptr;
    }

    g_engine.state = STATE_STOPPED;
    __android_log_print(ANDROID_LOG_INFO, TAG, "Engine stopped, state=STOPPED");

    pthread_mutex_unlock(&g_jni_mutex);
}

extern "C" JNIEXPORT void JNICALL
Java_com_mushroom_android_NativeEngine_nativeAttachSurface(
    JNIEnv* env, jobject /*thiz*/, jobject surface)
{
    pthread_mutex_lock(&g_jni_mutex);

    __android_log_print(ANDROID_LOG_INFO, TAG, "nativeAttachSurface");

    if (g_engine.surface) {
        env->DeleteGlobalRef(g_engine.surface);
    }
    g_engine.surface = env->NewGlobalRef(surface);

    __android_log_print(ANDROID_LOG_INFO, TAG, "Surface attached");

    pthread_mutex_unlock(&g_jni_mutex);
}

extern "C" JNIEXPORT void JNICALL
Java_com_mushroom_android_NativeEngine_nativeDetachSurface(JNIEnv* env, jobject /*thiz*/)
{
    pthread_mutex_lock(&g_jni_mutex);

    __android_log_print(ANDROID_LOG_INFO, TAG, "nativeDetachSurface");

    if (g_engine.surface) {
        env->DeleteGlobalRef(g_engine.surface);
        g_engine.surface = nullptr;
    }

    pthread_mutex_unlock(&g_jni_mutex);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_mushroom_android_NativeEngine_nativeGetWidth(JNIEnv* env, jobject /*thiz*/)
{
    return g_engine.fb.width;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_mushroom_android_NativeEngine_nativeGetHeight(JNIEnv* env, jobject /*thiz*/)
{
    return g_engine.fb.height;
}

extern "C" JNIEXPORT void JNICALL
Java_com_mushroom_android_NativeEngine_nativeSendKeyEvent(
    JNIEnv* env, jobject /*thiz*/, jint key_code, jboolean down)
{
    /* Forward to X11 renderer for key injection */
    if (g_engine.renderer_ctx && g_engine.state == STATE_RUNNING) {
        /* In a full implementation, this would call XTest or XSendEvent */
        __android_log_print(ANDROID_LOG_VERBOSE, TAG, "Key event: code=%d, down=%d", key_code, down);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_mushroom_android_NativeEngine_nativeSendPointerMotion(
    JNIEnv* env, jobject /*thiz*/, jint x, jint y)
{
    if (g_engine.renderer_ctx && g_engine.state == STATE_RUNNING) {
        /* Forward pointer motion to X11 */
        __android_log_print(ANDROID_LOG_VERBOSE, TAG, "Pointer motion: %d,%d", x, y);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_mushroom_android_NativeEngine_nativeSendPointerButton(
    JNIEnv* env, jobject /*thiz*/, jint button, jboolean down)
{
    if (g_engine.renderer_ctx && g_engine.state == STATE_RUNNING) {
        __android_log_print(ANDROID_LOG_VERBOSE, TAG, "Pointer button: %d, down=%d", button, down);
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_mushroom_android_NativeEngine_nativeSendTouchEvent(
    JNIEnv* env, jobject /*thiz*/,
    jint x, jint y, jint pointer_id, jint action)
{
    if (g_engine.renderer_ctx && g_engine.state == STATE_RUNNING) {
        __android_log_print(ANDROID_LOG_VERBOSE, TAG, "Touch: %d,%d id=%d action=%d", x, y, pointer_id, action);
    }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_mushroom_android_NativeEngine_nativeIsHealthy(JNIEnv* env, jobject /*thiz*/)
{
    return (g_engine.state == STATE_RUNNING && g_engine.fb.pixels != nullptr) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_mushroom_android_NativeEngine_nativeGetLastError(JNIEnv* env, jobject /*thiz*/)
{
    return env->NewStringUTF(g_engine.error_msg);
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_mushroom_android_NativeEngine_nativeGetFps(JNIEnv* env, jobject /*thiz*/)
{
    if (g_engine.renderer_ctx) {
        return (jfloat)x11_renderer_get_fps(&g_engine);
    }
    return 0.0f;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_mushroom_android_NativeEngine_nativeOpenPty(JNIEnv* env, jobject /*thiz*/)
{
    return (jint)pty_manager_open(&g_engine);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_mushroom_android_NativeEngine_nativeWritePty(
    JNIEnv* env, jobject /*thiz*/, jint fd, jbyteArray data)
{
    jsize len = env->GetArrayLength(data);
    jbyte* buf = env->GetByteArrayElements(data, nullptr);
    if (!buf) return -1;

    int written = pty_manager_write(&g_engine, (int)fd, (const uint8_t*)buf, (size_t)len);

    env->ReleaseByteArrayElements(data, buf, JNI_ABORT);
    return (jint)written;
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_mushroom_android_NativeEngine_nativeReadPty(
    JNIEnv* env, jobject /*thiz*/, jint fd, jint max_size)
{
    uint8_t* buffer = (uint8_t*)malloc((size_t)max_size);
    if (!buffer) return nullptr;

    int bytes_read = pty_manager_read(&g_engine, (int)fd, buffer, (size_t)max_size);
    if (bytes_read <= 0) {
        free(buffer);
        return nullptr;
    }

    jbyteArray result = env->NewByteArray(bytes_read);
    if (result) {
        env->SetByteArrayRegion(result, 0, bytes_read, (const jbyte*)buffer);
    }

    free(buffer);
    return result;
}

extern "C" JNIEXPORT void JNICALL
Java_com_mushroom_android_NativeEngine_nativeClosePty(JNIEnv* env, jobject /*thiz*/, jint fd)
{
    pty_manager_close(&g_engine, (int)fd);
}

/* JNI_OnLoad / OnUnload */
extern "C" JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM* vm, void* /*reserved*/)
{
    __android_log_print(ANDROID_LOG_INFO, TAG, "JNI_OnLoad: Mushroom engine loading");
    g_jvm = vm;
    g_engine.jvm = vm;
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL
JNI_OnUnload(JavaVM* vm, void* /*reserved*/)
{
    __android_log_print(ANDROID_LOG_INFO, TAG, "JNI_OnUnload: Mushroom engine unloading");
    if (g_engine.state == STATE_RUNNING) {
        g_engine.state = STATE_STOPPING;
        g_engine.render_loop_active = false;
        x11_renderer_stop(&g_engine);
        mount_ns_stop(&g_engine);
        interceptor_stop(&g_engine);
    }
    if (g_engine.fb.pixels) {
        free(g_engine.fb.pixels);
        g_engine.fb.pixels = nullptr;
    }
}