// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

pluginManagement {
    repositories {
        gradlePluginPortal()
        google()
        mavenCentral()
    }
}

@Suppress("UnstableApiUsage")
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

include(":app")

include("Eden")
project(":Eden").projectDir = file("../..")

providers.gradleProperty("duetPeanutsSourceDir").orNull?.let { source ->
    val app = file(source).resolve("android/app")
    require(app.resolve("src/embedded/AndroidManifest.xml").isFile) {
        "duetPeanutsSourceDir must point to the Peanuts source with the embedded host."
    }
    include(":peanutsHost")
    project(":peanutsHost").projectDir = app
}
