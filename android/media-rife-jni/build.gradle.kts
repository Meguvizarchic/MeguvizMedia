import groovy.json.JsonSlurper
import java.security.MessageDigest

plugins { id("com.android.library") }

// A packaging invocation may use a dedicated immutable composition snapshot.
// The native payload and its notice must always come from that same snapshot.
val compositionPath = providers.gradleProperty("meguviz.compositionDir").get()
check(compositionPath.isNotBlank()) { "Provide a non-empty meguviz.compositionDir" }
val composition = rootProject.file(compositionPath).canonicalFile
val compositionManifest = composition.resolve("manifest.json")
fun sha256(file: File): String = MessageDigest.getInstance("SHA-256").digest(file.readBytes())
    .joinToString("") { "%02x".format(it.toInt() and 255) }
fun compositionMaterial(record: Map<*, *>): File {
    val relative = record["path"] as? String ?: error("Missing composition material path")
    check(relative.isNotEmpty() && !File(relative).isAbsolute && ':' !in relative && '\\' !in relative &&
        relative.split('/').none { it.isEmpty() || it == "." || it == ".." }) { "Invalid composition material path" }
    val result = composition.resolve(relative).canonicalFile
    check(result.toPath().startsWith(composition.toPath()) && result.isFile) { "Composition material outside snapshot or missing" }
    check(record["sha256"] == sha256(result)) { "Composition material hash mismatch" }
    (record["size_bytes"] as? Number)?.let { check(it.toLong() == result.length()) { "Composition material size mismatch" } }
    return result
}
check(compositionManifest.isFile) { "Build the canonical composition before assembling the JNI owner" }
val compositionData = JsonSlurper().parse(compositionManifest) as? Map<*, *> ?: error("Invalid composition manifest")
check(compositionData["finalized"] == true) { "Canonical composition is not finalized" }
val lock = compositionMaterial(compositionData["composition_lock"] as? Map<*, *> ?: error("Missing composition lock"))
val lockData = JsonSlurper().parse(lock) as? Map<*, *> ?: error("Invalid composition lock")
val toolchain = lockData["toolchain"] as? Map<*, *> ?: error("Missing composition toolchain")
val openmp = (toolchain["android_static_openmp"] as? String) ?: "OFF"
check(openmp == "ON" || openmp == "OFF") { "Invalid Android OpenMP profile" }
val openmpRecords = (compositionData["notices"] as? List<*>)?.filterIsInstance<Map<*, *>>()
    ?.filter { it["path"] == "notices/llvm-openmp-NOTICE.txt" }.orEmpty()
check(if (openmp == "ON") openmpRecords.size == 1 else openmpRecords.isEmpty()) { "OpenMP notice/profile mismatch" }
if (openmp == "ON") check(toolchain["openmp_notice"] == openmpRecords.single()) { "OpenMP notice differs from composition lock" }
val openmpNotice = if (openmp == "ON") compositionMaterial(openmpRecords.single()) else null
val expectedPayloadPaths = setOf("arm64-v8a", "armeabi-v7a").map {
    "native-payload/$it/libmeguviz_media_rife_ncnn.so"
}.toSet()
val payloadRecords = (compositionData["abis"] as? List<*>)?.map {
    it as? Map<*, *> ?: error("Invalid composition ABI record")
} ?: error("Missing composition ABI records")
check(payloadRecords.size == expectedPayloadPaths.size &&
    payloadRecords.map { it["path"] }.toSet() == expectedPayloadPaths) { "Expected exactly the two canonical native payloads" }
val noticeAssets = layout.buildDirectory.dir("generated/locked-openmp-notice/assets")
val prepareLockedOpenmpNotice = if (openmpNotice != null) tasks.register("prepareLockedOpenmpNotice") {
    inputs.files(compositionManifest, lock, openmpNotice)
    outputs.file(noticeAssets.map { it.file("licenses/llvm-openmp-NOTICE.txt") })
    doLast {
        // Recheck after Gradle configuration so an externally replaced file cannot
        // be packaged under the prior manifest identity.
        compositionMaterial(compositionData["composition_lock"] as Map<*, *>)
        val source = compositionMaterial(openmpRecords.single())
        val target = noticeAssets.get().file("licenses/llvm-openmp-NOTICE.txt").asFile
        target.parentFile.mkdirs()
        target.writeBytes(source.readBytes())
    }
} else null
android {
    namespace = "io.github.jichuo1.meguviz.media.rife.jni"
    compileSdk = 37
    defaultConfig { minSdk = 27; consumerProguardFiles("../media-api/consumer-rules.pro") }
    // These are the already finalized canonical bytes; AGP must not strip them again.
    packaging { jniLibs { keepDebugSymbols += "**/libmeguviz_media_rife_ncnn.so" } }
    compileOptions { sourceCompatibility = JavaVersion.VERSION_17; targetCompatibility = JavaVersion.VERSION_17 }
}
androidComponents.onVariants { variant ->
    variant.sources.java?.addStaticSourceDirectory(file("../../bindings/android/src/main/java").absolutePath)
    variant.sources.jniLibs?.addStaticSourceDirectory(composition.resolve("native-payload").absolutePath)
    if (prepareLockedOpenmpNotice != null) variant.sources.assets?.addStaticSourceDirectory(noticeAssets.get().asFile.absolutePath)
}
tasks.register("verifyCanonicalNativePayload") {
    inputs.file(compositionManifest)
    inputs.dir(composition.resolve("native-payload"))
    doLast {
        val actualPayloadPaths = composition.resolve("native-payload").walkTopDown()
            .filter { it.isFile && it.extension == "so" }.map { it.relativeTo(composition).invariantSeparatorsPath }.toSet()
        check(actualPayloadPaths == expectedPayloadPaths) { "Packaged native payload paths differ from the manifest" }
        payloadRecords.forEach { compositionMaterial(it) }
    }
}
tasks.matching { it.name == "preBuild" }.configureEach {
    dependsOn("verifyCanonicalNativePayload")
    if (prepareLockedOpenmpNotice != null) dependsOn(prepareLockedOpenmpNotice)
}
