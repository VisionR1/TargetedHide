# Prevent R8 from stripping or renaming proxy classes.
# These classes are invoked via JNI directly from native code (module.cpp).
# Since R8 cannot detect native JNI calls, it would otherwise treat
# them as dead code and remove them, causing a silent runtime crash.
-keep class el.vision.targetedhide.PackageManagerProxy { *; }
-keep class el.vision.targetedhide.MediaStoreHideProxy { *; }
