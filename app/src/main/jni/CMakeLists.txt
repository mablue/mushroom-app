cmake_minimum_required(VERSION 3.22.1)
project(mushroom_engine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find packages
find_package(log REQUIRED)
find_package(EGL REQUIRED)
find_package(GLESv2 REQUIRED)
find_library(ANDROID_LIB android)
find_library(LOG_LIB log)
find_library(JNI_LIB jni)
find_library(ZLIB_LIB z)

# Main engine library
add_library(mushroom-engine SHARED
    jni/jni_bridge.cpp
    jni/syscall_interceptor.cpp
    jni/posix_wrappers.cpp
    jni/seccomp_policy.cpp
    jni/mount_ns.cpp
    jni/fakechroot.cpp
    jni/x11_renderer.cpp
    jni/rootfs_manager.cpp
)

target_include_directories(mushroom-engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/jni
)

target_link_libraries(mushroom-engine
    PRIVATE
    EGL
    GLESv2
    ${ANDROID_LIB}
    ${LOG_LIB}
    ${JNI_LIB}
    ${ZLIB_LIB}
    android
    c
    m
)

# Preload library for syscall interception
add_library(mushroom-preload SHARED
    jni/preload_hooks.c
    jni/posix_preload.c
)

target_compile_options(mushroom-preload PRIVATE
    -fPIC
    -shared
    -Wl,-soname,libmushroom-preload.so
)

target_link_libraries(mushroom-preload
    c
    m
)
