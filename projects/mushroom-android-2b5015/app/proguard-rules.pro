# Add project specific ProGuard rules here.
-keep class com.mushroom.android.** { *; }
-keep class com.mushroom.android.NativeEngine { *; }
-keep class com.mushroom.android.NativeEngine$* { *; }
-keepnames class com.mushroom.android.** { *; }
-keep class com.mushroom.android.views.** { *; }

# Keep JNI native methods
-keepclasseswithmembernames class * {
    native <methods>;
}

# Keep OpenGL ES classes
-keep class javax.microedition.khronos.** { *; }
-keep class android.opengl.** { *; }