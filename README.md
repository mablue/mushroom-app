# Mushroom Android - Linux Environment for Android

A complete Android application that runs a full Linux environment without requiring root, Termux, or an external VNC.

## Project Overview

**Project name**: Mushroom (codename)  
**Goal**: Build a standalone Android application (APK) that runs a full Linux environment  
**Tech stack**: Kotlin/Java (UI layer) + C++17/Rust (core interception engine via JNI)  
**Supported architectures**: ARM64 (arm64-v8a), x86_64  
**Min SDK**: 26 (Android 8.0)  
**Target SDK**: 34 (Android 14)

---

## Core Architecture

### 1. Syscall Interception Layer

**Implementation**: LD_PRELOAD + libfakechroot-style user-space wrapper functions

- **No ptrace**: Avoid ptrace context-switch overhead
- **Dynamic hooking**: Resolve real function pointers at runtime with `dlsym(RTLD_NEXT, ...)`
- **Path translation**: Map guest OS paths to the host RootFS directory

Intercepted syscalls:
- `open` / `openat` — file opening; translate paths to RootFS
- `stat` / `fstat` / `lstat` — file status queries
- `mount` — create virtual mount points
- `chdir` — change working directory
- `mkdir` — create directories
- `execve` — program execution; automatically inject LD_PRELOAD

### 2. RootFS Management

- **First-run download**: Download a minimal Debian/Ubuntu base from the network
- **Verification**: SHA256 checksum to ensure integrity
- **Storage location**: App private storage (`/data/data/com.mushroom.android/files/rootfs`)
- **Virtual mount points**:
  - `/proc` → mount procfs
  - `/sys` → mount sysfs (read-only)
  - `/dev` → tmpfs + fake device nodes
  - `/tmp` → tmpfs

### 3. Integrated Display Server & UI

- **Xvfb**: Headless X server running on display :1
- **OpenGL ES rendering pipeline**:
  1. Xvfb draws into a framebuffer
  2. JNI bridge transfers pixel data
  3. Upload pixels to an OpenGL ES texture
  4. Show via a SurfaceTexture in an Android SurfaceView

- **Touch/keyboard event injection**: Forward events using the XRecord extension and XSendEvent

### 4. System Services

- **Foreground Service**: Keep the Linux session running in the foreground
- **Persistent notification**: Show service status
- **UI control overlay**:
  - Start/Stop button
  - Resolution selector (800x600, 1024x768, 1280x720)
  - Memory limit slider (64MB ~ 1024MB)

---

## Project Structure

```
mushroom-android/
├── settings.gradle.kts           # Gradle settings
├── build.gradle.kts              # Root build configuration
├── gradle.properties             # Build properties (NDK version, etc.)
├── app/
│   ├── build.gradle.kts          # App module config
│   └── src/main/
│       ├── AndroidManifest.xml   # Permissions and component registration
│       ├── java/com/mushroom/android/
│       │   ├── MainActivity.kt            # Main Activity (UI entry)
│       │   ├── LinuxService.kt            # Foreground Service
│       │   ├── NativeEngine.kt            # JNI wrapper
│       │   └── views/
│       │       ├── LinuxDesktopView.kt    # OpenGL ES desktop view
│       │       └── TerminalView.kt        # Built-in terminal emulator
│       ├── jni/
│       │   ├── CMakeLists.txt                   # Native build config
│       │   ├── jni_bridge.cpp                   # JNI exports
│       │   ├── syscall_interceptor.cpp          # Syscall hook framework
│       │   ├── posix_wrappers.cpp               # POSIX wrapper functions
│       │   ├── seccomp_policy.cpp               # BPF filter compilation
│       │   ├── mount_ns.cpp                     # Mount namespace setup
│       │   ├── fakechroot.cpp                   # chroot emulation helper
│       │   ├── x11_renderer.cpp                 # Xvfb → EGL rendering pipeline
│       │   ├── rootfs_manager.cpp               # RootFS download/verify/extract
│       │   ├── engine_main.cpp                  # Lifecycle coordination
│       │   └── preload_hooks.c                  # LD_PRELOAD library source
│       └── res/
│           ├── layout/activity_main.xml
│           ├── values/{strings,colors,themes}.xml
│           ├── menu/bottom_navigation.xml
│           └── xml/{backup_rules,data_extraction_rules}.xml
└── README.md                                # This file
```

---

## Build & Deployment Guide

### Environment setup

```bash
# Install required tools on Debian/Ubuntu
sudo apt-get install android-studio cmake libcurl4-openssl-dev libarchive-dev libssl-dev

# Or with Homebrew (macOS)
brew install --cask android-studio cmake
```

### Android Studio configuration

1. **Open the project**: File → Open → select `mushroom-android` directory
2. **Configure NDK**:
   - Settings → SDK Tools → Check "Android NDK"
   - Recommended version: r26.1.10909125
3. **Configure JDK**: Settings → Build → Java Compiler → use JDK 17

### Build commands

```bash
# Debug APK
./gradlew assembleDebug

# Release APK (requires signing config)
./gradlew assembleRelease

# Build only a specific ABI
./gradlew assembleDebug -Pabi=arm64-v8a
```

### Deploy to device

```bash
# Connect device and enable USB debugging
adb devices

# Install APK
adb install app/build/outputs/apk/debug/app-debug.apk

# View logs
adb logcat -s Mushroom/*
```

---

## First-run Flow

1. Launch the app
2. Check RootFS: if missing, show "Downloading RootFS..."
3. Download RootFS (≈200MB) — network required
4. Verify SHA256 checksum
5. User selects resolution and memory limit
6. Tap "Start Linux"
7. Background service starts: fork subprocess and set up namespaces
8. Xvfb initializes: headless X server starts
9. Rendering begins: SurfaceView displays the Linux desktop

---

## Configuration

### Change RootFS source

Edit `jni/rootfs_manager.cpp`:

```cpp
// lines ~15-16
#define ROOTFS_URL "https://your-server.com/custom-debian.tar.gz"
#define ROOTFS_SHA256 "your-sha256-hash-here"
```

### Default resolution

Edit the `startService()` method in `MainActivity.kt`.

### Memory limit calculation

```kotlin
val mb = progress * 64  // SeekBar max=16, each step = 64MB
val bytes = mb * 1024 * 1024
```

---

## Debugging Tips

### Logcat filters

```bash
# All Mushroom-related logs
adb logcat -s Mushroom/*

# Specific modules
adb logcat -s Mushroom/JNI
adb logcat -s Mushroom/Syscall
adb logcat -s Mushroom/MountNS
adb logcat -s Mushroom/X11Renderer
adb logcat -s Mushroom/RootFS
```

### Check RootFS integrity

```bash
# List extracted files
adb shell run-as com.mushroom.android ls -la files/rootfs/
adb shell run-as com.mushroom.android ls -la files/rootfs/bin/
adb shell run-as com.mushroom.android ls -la files/rootfs/usr/
```

### Manual chroot test

```bash
# Enter app private storage
adb shell run-as com.mushroom.android sh

# Try to start a shell
cd files/rootfs
./bin/sh
```

### Common troubleshooting

| Problem | Possible cause | Solution |
|--------|----------------|---------|
| RootFS download fails | Network issue / invalid URL | Check network and ensure ROOTFS_URL is reachable |
| SHA256 mismatch | Corrupted download | Re-download and update the expected hash |
| Xvfb fails to start | missing libX11 | Ensure the RootFS includes Xorg |
| Black screen | EGL initialization failed | Verify device OpenGL ES support |
| Touch unresponsive | XRecord extension not enabled | Restart Xvfb with `-extension RECORD` |

---

## Known limitations

1. **Network passthrough**: Disabled by default for security
2. **Signal handling**: Some Unix signals are not fully forwarded
3. **Performance overhead**: User-space interception adds ~10% overhead
4. **Desktop environment**: XFCE/LXDE need to be packaged into the RootFS
5. **Audio**: No PulseAudio/PipeWire support yet

---

## References

- fakechroot: https://github.com/dex4er/fakechroot
- user-mode Linux: https://user-mode-linux.sourceforge.io/
- Xvfb: X Virtual Framebuffer
- seccomp-bpf: Secure Computing Berkeley Packet Filter
- NDK OpenGL ES: https://developer.android.com/ndk/guides/opengl

---

## License

MIT License

---

## Credits

- Inspiration: Linux Deploy, AnLinux, termux-api  
- Technical references: fakechroot, user-mode Linux  
- Graphics rendering approach: Xvfb + EGL interoperability
