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
    fun listsMultipleSupportedVersionsAndRejectsUnknownPrograms() {
        assertEquals(listOf("4.0.0", "4.0.1"), catalog.versions("0100152000022000").map { it.displayVersion })
        assertTrue(catalog.versions("0000000000000000").isEmpty())
    }

    private fun nsoId(prefix: String) = prefix.padEnd(64, '0')
}
