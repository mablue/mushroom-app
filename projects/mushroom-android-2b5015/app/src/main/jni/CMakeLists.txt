cmake_minimum_required(VERSION 3.22.1)
project("mushroom-engine" CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Visibility flags for the shared library
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fvisibility=hidden -fvisibility-inlines-hidden -Wall -Wextra")

# Find system libraries
find_library(LOG_LIB log)
find_library(Z_LIB z)
find_library(EGL_LIB egl)
find_library(GLESv2_LIB GLESv2)
find_library(ANDROID_LIB android)

# Source files
set(ENGINE_SOURCES
    jni_bridge.cpp
    syscall_interceptor.cpp
    posix_wrappers.cpp
    seccomp_policy.cpp
    mount_ns.cpp
    fakechroot.cpp
    x11_renderer.cpp
    rootfs_manager.cpp
    pty_manager.cpp
    signal_handler.cpp
    process_manager.cpp
)

# Build the shared library
add_library(
    mushroom-engine
    SHARED
    ${ENGINE_SOURCES}
)

# Link libraries
target_link_libraries(
    mushroom-engine
    ${LOG_LIB}
    ${Z_LIB}
    ${EGL_LIB}
    ${GLESv2_LIB}
    ${ANDROID_LIB}
    dl
)

# Include directories
target_include_directories(
    mushroom-engine
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Platform-specific defines
target_compile_definitions(
    mushroom-engine
    PRIVATE
    $<$<CONFIG:Debug>:DEBUG_BUILD>
    MUSHROOM_ENGINE
    MUSHROOM_VERSION="1.0.0"
)

# ABI-specific settings
if(ANDROID_ABI STREQUAL "arm64-v8a")
    target_compile_options(mushroom-engine PRIVATE -march=armv8-a+crypto)
elseif(ANDROID_ABI STREQUAL "x86_64")
    target_compile_options(mushroom-engine PRIVATE -msse4.2)
endif()