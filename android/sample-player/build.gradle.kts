plugins { id("com.android.application") }
android {
    namespace = "io.github.jichuo1.meguviz.media.player"
    compileSdk = 37
    defaultConfig {
        applicationId = "io.github.jichuo1.meguviz.media.player"
        minSdk = 27
        targetSdk = 37
        versionCode = 10201
        versionName = "${project.version}-candidate"
    }
    compileOptions { sourceCompatibility = JavaVersion.VERSION_17; targetCompatibility = JavaVersion.VERSION_17 }
    packaging { jniLibs { keepDebugSymbols += "**/libmeguviz_media_rife_ncnn.so" } }
}
dependencies { implementation(project(":media-rife-jni")) }
