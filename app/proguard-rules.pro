# Add project specific ProGuard rules here.
# You can control the set of applied configuration files using the
# proguardFiles setting in build.gradle.
# For more details, see
#   http://developer.android.com/guide/developing/tools/proguard.html

# Keep JNI methods
-keep class com.mushroom.android.NativeEngine { *; }
-keepclassmembers class com.mushroom.android.* {
    native <methods>;
}

# Keep NativeEngine data class
-keep class com.mushroom.android.NativeEngine$EngineState { *; }

# OpenGL/GLES keeps
-dontwarn javax.microedition.khronos.**
-keep class javax.microedition.khronos.** { *; }

# Android NDK keeps
-keep class android.opengl.** { *; }
-keep class android.graphics.** { *; }

# Keep reflection-based accesses
-keepattributes Signature, InnerClasses, EnclosingMethod

# Keep native libraries
-printmapping proguard.map

# Optimize for size in release builds
-optimizationpasses 5
-dontusemixedcaseclassnames
-dontskipnonpubliclibraryclasses
-verbose
