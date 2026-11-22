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
    // The pinned Peanuts distribution (tools/duo/peanuts.rev): its shader chain version always, its commit when the
    // source is a git checkout. A sanitized release snapshot is another commit with the same tree contents: pass
    // -PduetPeanutsSnapshot=true to check only the chain version there.
    val pin = file("../../tools/duo/peanuts.rev").readLines()
        .filter { it.isNotBlank() && !it.startsWith("#") }
        .associate { it.substringBefore(' ') to it.substringAfter(' ').trim() }
    val wantChain = requireNotNull(pin["dmsh_vk_chain"]) { "tools/duo/peanuts.rev has no dmsh_vk_chain line." }
    val chainHeader = file(source).resolve("dm/gfx/nvn/dmsh_chain_vk.h")
    val haveChain = Regex("""#define DMSH_VK_CHAIN "([0-9a-f]+)"""").find(chainHeader.takeIf { it.isFile }?.readText() ?: "")?.groupValues?.get(1)
    require(haveChain == wantChain) {
        "Peanuts source $source has DMSH_VK_CHAIN ${haveChain ?: "(none)"}, this Duet pins $wantChain (tools/duo/peanuts.rev): " +
            "check out the pinned Peanuts distribution ${pin["commit"]}."
    }
    if (providers.gradleProperty("duetPeanutsSnapshot").orNull != "true") {
        val wantCommit = requireNotNull(pin["commit"]) { "tools/duo/peanuts.rev has no commit line." }
        val head = runCatching {
            val p = ProcessBuilder("git", "-C", file(source).absolutePath, "rev-parse", "HEAD").redirectErrorStream(true).start()
            p.inputStream.bufferedReader().readText().trim().takeIf { p.waitFor() == 0 }
        }.getOrNull()
        require(head == wantCommit) {
            "Peanuts source $source is at ${head ?: "(not a git checkout)"}, this Duet pins $wantCommit (tools/duo/peanuts.rev). " +
                "Check out that commit, or pass -PduetPeanutsSnapshot=true for a sanitized release snapshot of it."
        }
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
