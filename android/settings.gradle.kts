pluginManagement { repositories { google(); mavenCentral(); gradlePluginPortal() } }
dependencyResolutionManagement { repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS); repositories { google(); mavenCentral() } }
rootProject.name = "MeguvizMediaIdentity"
include(":media-api", ":media-jni", ":sample-processing")
// The API AAR is an input to the composition build and can be built first.
// Keep explicit invalid snapshots visible to the packaging build for validation.
if (providers.gradleProperty("meguviz.compositionDir").isPresent) {
    include(":media-rife-jni", ":sample-player")
}
