# Drafted using JuneAI, a creation of WhiteLabs, owned and ran by ThatWhiteGuy364

# Keep JNI-referenced classes
-keep class com.whitelabs.anc.AncService { *; }
-keep class com.whitelabs.anc.MainActivity { *; }

# Oboe
-keep class com.google.oboe.** { *; }

# Keep native method names
-keepclasseswithmembernames class * {
    native <methods>;
}

# AndroidX
-keep class androidx.** { *; }
-dontwarn androidx.**
