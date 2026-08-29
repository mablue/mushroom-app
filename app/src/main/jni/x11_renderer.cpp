/**
 * X11 Renderer - Framebuffer to OpenGL Pipeline
 * 
 * Connects to Xvfb headless X server and renders framebuffer output
 * to Android's OpenGL ES surface.
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <GL/gl.h>
#include <EGL/egl.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <android/log.h>

#define LOG_TAG "Mushroom/X11Renderer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Global state
static Display* g_display = NULL;
static Window g_window = None;
static EGLDisplay g_egl_display = EGL_NO_DISPLAY;
static EGLSurface g_egl_surface = EGL_NO_SURFACE;
static EGLContext g_egl_context = EGL_NO_CONTEXT;
static ANativeWindow* g_native_window = NULL;
static pthread_t g_render_thread = 0;
static atomic_int g_running = 0;
static int g_width = 1024;
static int g_height = 768;

// Framebuffer storage
static unsigned char* g_framebuffer = NULL;
static GLuint g_texture_id = 0;

extern "C" {

int start_x11_renderer(int width, int height, ANativeWindow* window) {
    g_width = width;
    g_height = height;
    g_native_window = window;
    
    // Initialize X display (connect to Xvfb on :1)
    g_display = XOpenDisplay(":1");
    if (!g_display) {
        LOGE("Failed to connect to X server");
        return -1;
    }
    LOGI("Connected to X server");
    
    // Create window
    g_window = XCreateSimpleWindow(
        g_display,
        DefaultRootWindow(g_display),
        0, 0, width, height, 0,
        BlackPixel(g_display, 0),
        BlackPixel(g_display, 0)
    );
    
    XSelectInput(g_display, g_window, ExposureMask | KeyPressMask | ButtonPressMask);
    XMapWindow(g_display, g_window);
    XSync(g_display, False);
    
    // Initialize EGL
    initialize_egl();
    
    // Start render thread
    atomic_store(&g_running, 1);
    pthread_create(&g_render_thread, NULL, render_loop, NULL);
    
    return 0;
}

void stop_x11_renderer() {
    atomic_store(&g_running, 0);
    if (g_render_thread) {
        pthread_join(g_render_thread, NULL);
    }
    
    cleanup_egl();
    
    if (g_window != None) {
        XDestroyWindow(g_display, g_window);
        g_window = None;
    }
    
    if (g_display) {
        XCloseDisplay(g_display);
        g_display = NULL;
    }
    
    free(g_framebuffer);
    g_framebuffer = NULL;
}

void send_key_event(int keycode, int is_press) {
    if (!g_display || !g_window) return;
    
    KeySym keysym = XKeysymForKeycode(g_display, keycode, 0, 0);
    XEvent event;
    memset(&event, 0, sizeof(event));
    
    if (is_press) {
        event.type = KeyPress;
        event.xkey.display = g_display;
        event.xkey.window = g_window;
        event.xkey.keycode = keycode;
        event.xkey.state = 0;
    } else {
        event.type = KeyRelease;
        event.xkey.display = g_display;
        event.xkey.window = g_window;
        event.xkey.keycode = keycode;
        event.xkey.state = 0;
    }
    
    XSendEvent(g_display, g_window, False, KeyPressMask | KeyReleaseMask, &event);
    XFlush(g_display);
}

void send_touch_event(float x, float y, int action) {
    if (!g_display || !g_window) return;
    
    XEvent event;
    memset(&event, 0, sizeof(event));
    
    int button = (action == 0) ? Button1 : ((action == 2) ? Button3 : Button1);
    
    event.type = (action == 2) ? ButtonRelease : ButtonPress;
    event.xbutton.display = g_display;
    event.xbutton.window = g_window;
    event.xbutton.x = (int)x;
    event.xbutton.y = (int)y;
    event.xbutton.button = button;
    event.xbutton.state = 0;
    
    XSendEvent(g_display, g_window, False, ButtonPressMask | ButtonReleaseMask, &event);
    XFlush(g_display);
}

static void initialize_egl() {
    EGLConfig config;
    EGLint num_configs;
    EGLint attribs[] = {
        EGL_SURFACE_TYPE, EGL_NATIVE_WINDOW_BIT,
        EGL_BLUE_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_RED_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_NONE
    };
    
    g_egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_egl_display == EGL_NO_DISPLAY) {
        LOGE("Failed to get EGL display");
        return;
    }
    
    eglInitialize(g_egl_display, NULL, NULL);
    eglChooseConfig(g_egl_display, attribs, &config, 1, &num_configs);
    
    g_egl_context = eglCreateContext(g_egl_display, config, EGL_NO_CONTEXT, NULL);
    g_egl_surface = eglCreateWindowSurface(g_egl_display, config, g_native_window, NULL);
    
    eglMakeCurrent(g_egl_display, g_egl_surface, g_egl_surface, g_egl_context);
    
    // Create texture for framebuffer
    glGenTextures(1, &g_texture_id);
    glBindTexture(GL_TEXTURE_2D, g_texture_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, g_width, g_height, 0, 
                 GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    
    LOGI("EGL initialized, texture created");
}

static void cleanup_egl() {
    if (g_texture_id) {
        glDeleteTextures(1, &g_texture_id);
        g_texture_id = 0;
    }
    
    if (g_egl_surface != EGL_NO_SURFACE) {
        eglDestroySurface(g_egl_display, g_egl_surface);
        g_egl_surface = EGL_NO_SURFACE;
    }
    
    if (g_egl_context != EGL_NO_CONTEXT) {
        eglDestroyContext(g_egl_display, g_egl_context);
        g_egl_context = EGL_NO_CONTEXT;
    }
    
    if (g_egl_display != EGL_NO_DISPLAY) {
        eglTerminate(g_egl_display);
        g_egl_display = EGL_NO_DISPLAY;
    }
}

static void* render_loop(void* arg) {
    while (atomic_load(&g_running)) {
        // Process X events
        XEvent event;
        while (XPending(g_display)) {
            XNextEvent(g_display, &event);
            
            switch (event.type) {
                case ConfigureNotify:
                    // Resize if needed
                    break;
                case Expose:
                    // Redraw needed
                    break;
                default:
                    break;
            }
        }
        
        // Get framebuffer data
        XImage* img = XGetImage(g_display, g_window, 0, 0, 
                                g_width, g_height, AllPlanes, ZPixmap);
        
        if (img) {
            // Upload to texture
            glBindTexture(GL_TEXTURE_2D, g_texture_id);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 
                           g_width, g_height, GL_RGB, GL_UNSIGNED_BYTE,
                           img->data);
            XDestroyImage(img);
            
            // Render to screen
            eglSwapBuffers(g_egl_display, g_egl_surface);
        }
        
        usleep(33000); // ~30 FPS
    }
    
    return NULL;
}

} // extern "C"
