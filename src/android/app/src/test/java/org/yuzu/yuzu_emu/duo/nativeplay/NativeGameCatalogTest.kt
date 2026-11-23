// SPDX-FileCopyrightText: Copyright 2026 Duet Plus contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.nativeplay

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

class NativeGameCatalogTest {
    private val catalog = File("src/main/assets", NativeGameCatalog.ASSET)
        .reader().use(NativeGameCatalog::read)

    @Test
    fun matchesHogwartsUpdateOnlyWhenAllModulesMatch() {
        val build = NativeGameBuild(
            "0100F7E00C70E000",
            327680,
            mapOf(
                "main" to nsoId("0c00fc37b0610fbdb2cf73aed0cc73e7"),
                "subsdk0" to nsoId("d0c48eef6dc2f1e7ac8b8dd5b5b03a60"),
                "sdk" to nsoId("1ea0b9fe6cd6867b18e374a5d53d7950")
            )
        )

        assertEquals(NativeEngine.PEANUTS, catalog.match(build)?.engine)
        assertEquals("1.0.5", catalog.match(build)?.displayVersion)
        assertNull(catalog.match(build.copy(version = 0)))
        assertNull(catalog.match(build.copy(version = 393216)))
        assertNull(catalog.match(build.copy(moduleBuildIds = build.moduleBuildIds - "sdk")))
        assertNull(catalog.match(build.copy(moduleBuildIds = build.moduleBuildIds + ("sdk" to "0".repeat(64)))))
        assertNull(catalog.match(build.copy(moduleBuildIds = build.moduleBuildIds.mapValues { it.value.take(32) })))
    }

    @Test
    fun recognizesPrivateZaWithoutAcceptingUnmatchedCode() {
        val build = NativeGameBuild(
            "0100F43008C44000",
            393216,
            mapOf(
                "main" to nsoId("b1f12fd919eae86ab8a978317677e64b"),
                "sdk" to nsoId("df7a3960c74c3a4803f826c587153252")
            )
        )
        assertEquals(NativeEngine.PEANUTS, catalog.match(build)?.engine)
        assertNull(catalog.match(build.copy(moduleBuildIds = build.moduleBuildIds - "sdk")))
        assertNull(catalog.match(build.copy(version = 0)))
    }

    @Test
    fun recognizesZa200CodeProfileBeforePreparationChecksItsShaders() {
        val build = NativeGameBuild(
            "0100F43008C44000",
            262144,
            mapOf(
                "main" to nsoId("8a1c86c437394b698397a85f45cc7761"),
                "sdk" to nsoId("df7a3960c74c3a4803f826c587153252")
            )
        )
        assertEquals(NativeEngine.PEANUTS, catalog.match(build)?.engine)
    }

    @Test
    fun acceptsBananaBuildForDonkeyKongBananza() {
        val build = NativeGameBuild(
            "040074A01BF12000",
            458752,
            mapOf(
                "main" to nsoId("e25c90c1c136638885659fd55bf8a03b"),
                "sdk" to nsoId("18265daea2105b31f25346adfbbd09b5")
            )
        )

        assertEquals(NativeEngine.BANANA, catalog.match(build)?.engine)
        assertNull(catalog.match(build.copy(programId = "040057C02159A000")))
    }

    @Test
    fun distinguishesBothWonderProgramsAndExcludesOlderSwitch1Builds() {
        val base = catalog.versions("010015100b514000").single()
        val edition = catalog.versions("040015100B514001").single()

        assertEquals(327680L, base.version)
        assertEquals(NativeEngine.PEANUTS, base.engine)
        assertEquals(NativeEngine.BANANA, edition.engine)
        val editionBuild = NativeGameBuild(
            edition.programId,
            edition.version,
            edition.moduleBuildIds.mapValues { nsoId(it.value) }
        )
        assertEquals(edition, catalog.match(editionBuild))
        assertNull(catalog.match(editionBuild.copy(programId = base.programId, version = base.version)))
    }

    @Test
    fun recognizesIndexedEditionFromItsLibraryApplicationId() {
        assertTrue(catalog.recognizesTitle("040015100B514000"))
        assertTrue(!catalog.recognizesTitle("040015100B514002"))
        assertTrue(catalog.versions("040015100B514000").isEmpty())
    }

    @Test
    fun collectionMembersWithTheSameSdkRequireTheirOwnMain() {
        val menu = "010090C022978000"
        val kh2 = NativeGameBuild(
            "010090C022978003", 0,
            mapOf("main" to nsoId("e1bced80ffd14436980f7490b1d3c3e7"),
                "subsdk1" to nsoId("99e21f98a397434b534c1459fc38a2b8"),
                "sdk" to nsoId("bc204aae7319bab274894f1cc3f5da92"))
        )
        val kh1 = kh2.copy(
            programId = "010090C022978001",
            moduleBuildIds = kh2.moduleBuildIds + ("main" to nsoId("badf2faf580b712464365c4d529d92c9"))
        )
        assertEquals(NativeEngine.PEANUTS, catalog.match(kh1)?.engine)
        assertNull(catalog.match(kh2.copy(programId = kh1.programId)))
        assertNull(catalog.match(kh1.copy(programId = kh2.programId)))
        assertTrue(catalog.recognizesTitle(menu))
        assertTrue(3 in catalog.programIndices)
        assertEquals(NativeEngine.PEANUTS, catalog.match(kh2)?.engine)
        assertNull(catalog.match(kh2.copy(programId = menu)))
        assertNull(catalog.match(kh2.copy(moduleBuildIds = kh2.moduleBuildIds - "subsdk1")))
        assertNull(catalog.match(kh2.copy(version = 65536)))
    }

    @Test
    fun listsMultipleSupportedVersionsAndRejectsUnknownPrograms() {
        assertEquals(listOf("4.0.0", "4.0.1"), catalog.versions("0100152000022000").map { it.displayVersion })
        assertTrue(catalog.versions("0000000000000000").isEmpty())
    }

    @Test
    fun skyFirst107MatchesItsOwnMainWithoutReplacingThePrivateBuilds() {
        val id = "01002C9022770000"
        val build = NativeGameBuild(
            id,
            393216,
            mapOf(
                "main" to nsoId("8963a531fa7f27a52323081ace0f2f6e"),
                "subsdk0" to nsoId("fc537f0b21c880e89d6f247d22a150ca"),
                "sdk" to nsoId("148394139d9d26ca8afd5c9ba5d30601")
            )
        )
        assertEquals(listOf("1.0.4", "1.0.7", "1.0.8"), catalog.versions(id).map { it.displayVersion })
        assertEquals("1.0.7", catalog.match(build)?.displayVersion)
        assertNull(catalog.match(build.copy(version = 458752)))
    }

    private fun nsoId(prefix: String) = prefix.padEnd(64, '0')
}
