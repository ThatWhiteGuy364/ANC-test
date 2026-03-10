// Drafted using JuneAI, a creation of WhiteLabs, owned and ran by ThatWhiteGuy364

pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "WhiteLabsANC"
include(":app")
