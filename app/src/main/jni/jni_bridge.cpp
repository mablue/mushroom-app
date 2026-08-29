#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define LOG_TAG "Mushroom/JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Global state
static ANativeWindow* g_window = nullptr;
static pthread_t g_engine_thread = 0;
static bool g_running = false;
static long g_session_id = 0;

extern "C" {
    int start_linux_environment(const char* rootfs_path, long session_id);
    void stop_linux_environment(long session_id);
    void set_resolution(int width, int height);
    void set_memory_limit(long bytes);
    void update_framebuffer(void* pixels, int width, int height, int stride);
}

extern "C" {

JNIEXPORT void JNICALL
Java_com_mushroom_android_NativeEngine_attachSurface(JNIEnv* env, jobject thiz, jobject surface) {
    if (surface == nullptr) {
        LOGE("Null surface passed");
        return;
    }
    
    g_window = ANativeWindow_fromSurface(env, surface);
    if (g_window) {
        LOGI("Attached native window: %p", g_window);
        int32_t w = ANativeWindow_getWidth(g_window);
        int32_t h = ANativeWindow_getHeight(g_window);
        LOGI("Window size: %dx%d", w, h);
        set_resolution(w, h);
    } else {
        LOGE("Failed to attach native window");
    }
}

JNIEXPORT void JNICALL
Java_com_mushroom_android_NativeEngine_detachSurface(JNIEnv* env, jobject thiz) {
    if (g_window) {
        ANativeWindow_release(g_window);
        g_window = nullptr;
        LOGI("Detached native window");
    }
}

JNIEXPORT jboolean JNICALL
Java_com_mushroom_android_NativeEngine_startEngine(JNIEnv* env, jobject thiz, jstring rootfs_path_j, jlong session_id) {
    if (g_running) {
        LOGW("Engine already running");
        return JNI_FALSE;
    }
    
    const char* rootfs_path = env->GetStringUTFChars(rootfs_path_j, nullptr);
    if (!rootfs_path) {
        LOGE("Failed to get rootfs path string");
        return JNI_FALSE;
    }
    
    g_session_id = session_id;
    g_running = true;
    
    int result = start_linux_environment(rootfs_path, session_id);
    
    env->ReleaseStringUTFChars(rootfs_path_j, rootfs_path);
    
    if (result == 0) {
        LOGI("Linux environment started successfully");
        return JNI_TRUE;
    } else {
        LOGE("Failed to start Linux environment: %d", result);
        g_running = false;
        return JNI_FALSE;
    }
}

JNIEXPORT jboolean JNICALL
Java_com_mushroom_android_NativeEngine_stopEngine(JNIEnv* env, jobject thiz, jlong session_id) {
    if (!g_running) {
        LOGW("Engine not running");
        return JNI_FALSE;
    }
    
    stop_linux_environment(session_id);
    g_running = false;
    LOGI("Linux environment stopped");
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_com_mushroom_android_NativeEngine_setResolution(JNIEnv* env, jobject thiz, jint width, jint height) {
    set_resolution(width, height);
    LOGI("Resolution set to: %dx%d", width, height);
}

JNIEXPORT void JNICALL
Java_com_mushroom_android_NativeEngine_setMemoryLimit(JNIEnv* env, jobject thiz, jlong bytes) {
    set_memory_limit(bytes);
    LOGI("Memory limit set to: %ld bytes", bytes);
}

JNIEXPORT void JNICALL
Java_com_mushroom_android_NativeEngine_updateFramebuffer(JNIEnv* env, jobject thiz, jint tex_id, jint width, jint height, jint stride) {
    // This would be called from Java to update the OpenGL texture
    // Implementation in x11_renderer.cpp
}

JNIEXPORT void JNICALL
Java_com_mushroom_android_NativeEngine_sendKeyEvent(JNIEnv* env, jobject thiz, jint key_code, jboolean is_key_down) {
    // Send key event to X server
    // Implementation needed in x11_renderer.cpp
}

JNIEXPORT void JNICALL
Java_com_mushroom_android_NativeEngine_sendTouchEvent(JNIEnv* env, jobject thiz, jint event_id, jfloat x, jfloat y, jint action) {
    // Send touch event to X server
    // Implementation needed in x11_renderer.cpp
}

} // extern "C"
