# Prevent R8 from stripping or renaming PackageManagerProxy.
# This class is invoked via JNI directly from native code (module.cpp).
# Since R8 cannot detect native JNI calls, it would otherwise treat
# this as dead code and remove it, causing a silent runtime crash.
-keep class el.vision.targetedhide.PackageManagerProxy { *; }
