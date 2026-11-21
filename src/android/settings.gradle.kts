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
    require(app.resolve("src/ondevice/java/dev/nxnative/android/EmbeddedShaderCapture.java").isFile) {
        "This Duet host requires a Peanuts distribution with on-device runtime shader capture."
    }
    val games = file("app/src/main/assets/native_games.tsv").useLines { rows ->
        rows.filter { it.startsWith("peanuts\t") }.map { it.split('\t')[1] }.distinct().toList()
    }
    require(games.isNotEmpty()) { "The embedded Peanuts catalog is empty." }
    gradle.rootProject {
        extensions.extraProperties["nxEmbeddedGames"] = games
    }
    include(":peanutsHost")
    project(":peanutsHost").projectDir = app
}

providers.gradleProperty("duetBananaSourceDir").orNull?.let { source ->
    val app = file(source).resolve("android/app")
    require(app.resolve("src/embedded/AndroidManifest.xml").isFile) {
        "duetBananaSourceDir must point to the source with the embedded runtime."
    }
    val games = file("app/src/main/assets/native_games.tsv").useLines { rows ->
        rows.filter { it.startsWith("banana\t") }.map { it.split('\t')[1] }.distinct().toList()
    }
    require(games.isNotEmpty()) { "The embedded Switch 2 catalog is empty." }
    gradle.rootProject {
        extensions.extraProperties["duetBananaGames"] = games
    }
    include(":bananaHost")
    project(":bananaHost").projectDir = app
}
