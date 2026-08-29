# Mushroom — All-in-One Linux Environment Runner for Android

**Codename:** Mushroom  
**Version:** 1.0.0  
**Architecture:** ARM64 / x86_64  
**Min SDK:** 26 (Android 8.0+)  
**Target SDK:** 34  
**Languages:** Kotlin (UI), C++17 (Native Engine via JNI)

---

## Overview

Mushroom is a standalone Android application that runs a complete Linux environment
directly on your device — **no root, no Termux, no external VNC viewer required**.

The app implements a full software stack from system-call interception to a
graphical X11 desktop, all inside a single APK with post-install RootFS download.

### Key Features

| Feature | Implementation |
|---|---|
| **Syscall Interception** | LD_PRELOAD + libfakechroot wrappers (zero ptrace overhead) |
| **RootFS** | Debian/Ubuntu base, downloaded post-install, stored in app private storage |
| **Display Server** | Xvfb → EGL/OpenGL ES rendering pipeline in Android SurfaceView |
| **Desktop Environment** | XFCE/LXDE (auto-detected inside RootFS) |
| **Terminal Emulator** | Built-in PTY-backed VT100-compatible terminal |
| **Seccomp-BPF** | Kernel-level syscall filtering for dangerous operations |
| **Virtual Mounts** | /proc, /sys, /dev, /tmp emulated without root |
| **Foreground Service** | Keeps Linux session alive in background |

### Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    Android UI (Kotlin)                   │
│  ┌──────────────┐  ┌──────────────┐  ┌───────────────┐ │
│  │  MainActivity │  │ LinuxService │  │  Views (GL)   │ │
│  │  (Toggle UI)  │  │ (Foreground) │  │ Desktop/Term  │ │
│  └──────┬───────┘  └──────┬───────┘  └───────┬───────┘ │
│         │                 │                   │         │
│  ┌──────┴─────────────────┴───────────────────┴──────┐ │
│  │              JNI Bridge (NativeEngine.kt)          │ │
│  └──────────────────────┬────────────────────────────┘ │
├─────────────────────────┼──────────────────────────────┤
│                Native Engine (C++17)                   │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐  │
│  │ Syscall  │ │ Mount NS │ │ X11      │ │ PTY      │  │
│  │ Intercept│ │ Virtual  │ │ Renderer │ │ Manager  │  │
│  ├──────────┤ │ Mounts   │ ├──────────┤ ├──────────┤  │
│  │ Seccomp  │ └──────────┘ │ Xvfb     │ │ Terminal │  │
│  │ BPF      │ ┌──────────┐ │ Framebuf │ │ Emulator │  │
│  └──────────┘ │ RootFS   │ └──────────┘ └──────────┘  │
│               │ Manager  │  ┌──────────┐               │
│               └──────────┘  │ Signal   │               │
│                             │ Handler  │               │
│                             └──────────┘               │
└─────────────────────────────────────────────────────────┘
```

---

## Project Structure

```
mushroom-android/
├── settings.gradle.kts           # Project settings
├── build.gradle.kts              # Root build config
├── gradle.properties             # Gradle properties
├── README.md                     # This file
├── app/
│   ├── build.gradle.kts          # App module build config
│   ├── proguard-rules.pro        # ProGuard rules
│   └── src/main/
│       ├── AndroidManifest.xml   # App manifest
│       ├── res/                   # Resources (layouts, values, drawables)
│       └── java/com/mushroom/android/
│           ├── MushroomApp.kt    # Application class
│           ├── MainActivity.kt   # Main entry point with UI
│           ├── NativeEngine.kt   # JNI bridge (all native methods)
│           ├── LinuxService.kt   # Foreground service
│           └── views/
│               ├── LinuxDesktopView.kt  # OpenGL SurfaceView for X11
│               └── TerminalView.kt      # Built-in terminal emulator
│       └── jni/                  # Native C++ source
│           ├── CMakeLists.txt    # CMake build configuration
│           ├── include/
│           │   └── engine.h      # Master header with all types
│           ├── jni_bridge.cpp    # JNI exports (Java ↔ Native)
│           ├── syscall_interceptor.cpp  # LD_PRELOAD hook system
│           ├── posix_wrappers.cpp       # POSIX function wrappers
│           ├── seccomp_policy.cpp       # Seccomp-BPF filter
│           ├── mount_ns.cpp             # Mount namespace + virtual mounts
│           ├── fakechroot.cpp           # Path translation engine
│           ├── x11_renderer.cpp         # X11 framebuffer → EGL pipeline
│           ├── rootfs_manager.cpp       # Download/verify/extract RootFS
│           ├── pty_manager.cpp          # PTY session management
│           ├── signal_handler.cpp       # Signal handling
│           ├── process_manager.cpp      # Process spawning
│           └── utils.cpp                # Utility functions
└── native-engine/                # Standalone native module (optional)
    ├── build.gradle.kts
    └── src/main/cpp/             # Mirrors app/src/main/jni/
```

---

## Configuration

### RootFS Download URL

The RootFS is downloaded from a configurable URL. Edit `rootfs_manager.cpp`:

```cpp
// Default: Debian Bookworm base tarball
// Replace with your own hosted RootFS tarball URL
snprintf(rctx->download_url, sizeof(rctx->download_url),
         "https://your-server.com/rootfs/rootfs.tar.xz");
```

### RootFS SHA256 Verification

Set the expected SHA256 hash in the same file:

```cpp
rctx->expected_sha256[0] = '\0';  // Set to "a1b2c3..." to enable verification
```

### Building Your Own RootFS

To create a minimal RootFS:

```bash
# Using debootstrap
sudo debootstrap --arch=arm64 --variant=minbase bookworm ./rootfs
# Add XFCE desktop
sudo chroot ./rootfs apt-get install -y xfce4 xfce4-goodies xvfb x11vnc
# Create tarball
sudo tar -cJf rootfs.tar.xz -C ./rootfs .
```

---

## Build Instructions

### Prerequisites

- **Android Studio** Hedgehog (2023.1.1+) or later
- **NDK** r26.1 (included via SDK Manager)
- **CMake** 3.22.1+ (included via SDK Manager)
- **Android SDK** 34 (API 34)
- **Kotlin** 1.9.22 plugin
- **AGP** 8.2.2

### Opening in Android Studio

1. Open Android Studio
2. Select **File → Open**
3. Navigate to `mushroom-android/`
4. Click **Open**
5. Wait for Gradle sync to complete
6. If prompted, install any missing SDK/NDK components

### Command-Line Build

```bash
# Clean build
./gradlew clean

# Build debug APK
./gradlew assembleDebug

# Build release APK (with ProGuard)
./gradlew assembleRelease

# Build only the native library
./gradlew :app:externalNativeBuildDebug

# Build for specific ABI
./gradlew :app:externalNativeBuildDebug -Pandroid.abis=arm64-v8a

# Run tests
./gradlew test
```

### Output APK Location

```
app/build/outputs/apk/debug/app-debug.apk
app/build/outputs/apk/release/app-release.apk
```

---

## Deployment to Device

### Requirements

- Android device running **Android 10+ (API 29+)**
- At least **2GB free RAM** (4GB+ recommended)
- At least **1GB free internal storage** (for RootFS)
- **Internet connection** (first run only, for RootFS download)

### Installing via ADB

```bash
# Connect device via USB debugging
adb devices

# Install debug APK
adb install -r app/build/outputs/apk/debug/app-debug.apk

# Or install with specific flags
adb install -g -r app/build/outputs/apk/debug/app-debug.apk
```

### Installing via Android Studio

1. Connect your device via USB
2. Enable **USB Debugging** on the device
3. Select the device in Android Studio's run configuration
4. Click **Run** (green triangle)

---

## First-Run Flow

1. **Launch Mushroom** — The app opens in terminal mode
2. **Tap Start** — The app begins the setup process:
   - **Download RootFS** (first run only, ~150-300MB, may take a few minutes)
   - **Extract RootFS** (~30-60 seconds)
   - **Initialize Engine** (creates virtual mounts, applies seccomp)
   - **Start Linux Environment** — Status shows "Ready"
3. **Choose Desktop Mode** — Tap the Desktop icon in the bottom nav
4. **Select Resolution** — Use the dropdown (800x600, 1024x768, 1280x720)
5. **Adjust Memory** — Use the slider (512MB–4GB, default 2GB)
6. **Use the Linux Desktop** — Touch/pointer input is forwarded to X11
7. **Switch to Terminal** — Tap the Terminal icon for shell access
8. **Stop** — Tap Stop to gracefully shut down the Linux environment

---

## Debugging

### Logcat Filters

```bash
# View all Mushroom-related logs
adb logcat -s MushroomActivity:MushroomService:MushroomNative:MushroomJNI

# View native engine logs
adb logcat -s MushroomJNI:MushroomX11:MushroomPTY:MushroomMountNS

# View all logs with timestamps
adb logcat -v time | grep -i mushroom

# Save logs to file
adb logcat -d > mushroom_logs.txt
```

### Checking RootFS Integrity

```bash
# Check if RootFS is extracted
adb shell
run-as com.mushroom.android
ls -la /data/data/com.mushroom.android/files/rootfs/
cat /data/data/com.mushroom.android/files/rootfs/.mushroom_extracted

# Verify RootFS structure
ls /data/data/com.mushroom.android/files/rootfs/bin/
ls /data/data/com.mushroom.android/files/rootfs/etc/
```

### Testing the Native Engine

```bash
# Check if the native library is loaded
adb logcat -s MushroomJNI | grep "JNI_OnLoad"

# Force crash for stack trace
adb shell am force-stop com.mushroom.android

# Clear app data (resets everything)
adb shell pm clear com.mushroom.android
```

### Common Issues

| Issue | Solution |
|---|---|
| "Native library not loaded" | Ensure NDK is installed and ABI matches device |
| RootFS download fails | Check internet connection; verify URL in rootfs_manager.cpp |
| X11 desktop not rendering | XFCE may not be in the RootFS; install it or use terminal mode |
| PTY allocation fails | Check SELinux policy; try `adb shell setenforce 0` (if rooted) |
| Low memory warnings | Reduce memory slider; close other apps |
| Seccomp policy kills app | Disable seccomp in EngineConfig (enable_seccomp = false) |

---

## Known Limitations

1. **No WiFi passthrough yet** — Network is bridged via Android's NAT, not directly
2. **Limited signal handling** — Some signals may not be forwarded correctly
3. **No GPU acceleration** — X11 rendering is software-based (llvmpipe)
4. **No audio passthrough** — ALSA/PulseAudio devices are not bridged
5. **No USB device passthrough** — USB devices are not accessible from the chroot
6. **SELinux restrictions** — Some operations may be blocked on strict SELinux policies
7. **No sudo** — The chroot runs as the app's UID; no privilege escalation
8. **Single display** — Only one X11 display (:1) is supported
9. **No hardware cursor** — The cursor is rendered in software
10. **No clipboard sync** — Android ↔ Linux clipboard not yet bridged

---

## Security Considerations

- **Seccomp-BPF** blocks dangerous syscalls (ptrace, reboot, module loading)
- **No root access** — app runs entirely within Android's sandbox
- **App storage isolation** — RootFS is stored in app-private storage only
- **No external binaries** — All native code is compiled into the APK
- **Network isolation** — chroot processes run as the app's UID

---

## License

Internal project — Mushroom Engine

---

## Credits

Built with:
- [Android NDK](https://developer.android.com/ndk)
- [Kotlin](https://kotlinlang.org/)
- [Xvfb](https://www.x.org/releases/X11R7.6/doc/man/man1/Xvfb.1.xhtml)
- [Debian](https://www.debian.org/)
- Architecture inspired by fakechroot, proot, and Termux