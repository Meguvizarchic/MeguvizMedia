-keep class io.github.jichuo1.meguviz.media.MediaNative { *; }
-keep class io.github.jichuo1.meguviz.media.QcomMotionNative { native <methods>; }
-keepclasseswithmembernames,includedescriptorclasses class io.github.jichuo1.meguviz.media.MediaPlanner { native <methods>; }
-keepclasseswithmembernames,includedescriptorclasses class io.github.jichuo1.meguviz.media.MediaCadence { native <methods>; }
-keepclasseswithmembernames,includedescriptorclasses class io.github.jichuo1.meguviz.media.MediaVsyncGrid { native <methods>; }
-keepclasseswithmembernames,includedescriptorclasses class io.github.jichuo1.meguviz.media.MediaVsyncMeter { native <methods>; }

# Looked up by class and method name from android_jni.cpp.
-keep class io.github.jichuo1.meguviz.media.MediaRetirementService {
    static void requestImmediate();
}
