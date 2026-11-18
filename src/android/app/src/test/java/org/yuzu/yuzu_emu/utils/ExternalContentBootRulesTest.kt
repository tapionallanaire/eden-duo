// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.utils

import java.util.concurrent.Executors
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.yuzu.yuzu_emu.utils.ExternalContentBootRules.ContainerStamp
import org.yuzu.yuzu_emu.utils.ExternalContentBootRules.ContainerTitles
import org.yuzu.yuzu_emu.utils.ExternalContentBootRules.ContentContainer
import org.yuzu.yuzu_emu.utils.ExternalContentBootRules.ContentRecord

class ExternalContentBootRulesTest {
    private val titleA = 0x01007EF00011E000L
    private val titleB = 0x0100000000010000L
    private val stamps = listOf(
        ContainerStamp("update-a", 100, 200),
        ContainerStamp("dlc-a", 300, 400),
        ContainerStamp("update-b", 500, 600)
    )
    private val titles = mapOf(
        "update-a" to listOf(titleA),
        "dlc-a" to listOf(titleA),
        "update-b" to listOf(titleB)
    )

    @Test
    fun coldBootReadsAllOnceAndSubsequentTitleSwitchReadsNone() {
        val first = ExternalContentBootRules.select(titleA, stamps, emptyMap()) {
            titles.getValue(it)
        }
        assertEquals(listOf("update-a", "dlc-a"), first.matches)
        assertEquals(3, first.readCount)
        val next = ExternalContentBootRules.select(titleB, stamps, first.remembered) {
            error("unchanged container must not be reopened: $it")
        }
        assertEquals(listOf("update-b"), next.matches)
        assertEquals(0, next.readCount)
    }

    @Test
    fun serializedCacheSurvivesAColdProcessWithLargeTitleIds() {
        val first = ExternalContentBootRules.select(titleA, stamps, emptyMap()) {
            titles.getValue(it)
        }
        val restored = Json.decodeFromString<Map<String, ContainerTitles>>(
            Json.encodeToString(first.remembered)
        )
        assertEquals(first.remembered, restored)
        val next = ExternalContentBootRules.select(titleB, stamps, restored) {
            error("persistent cache lost its metadata or title precision")
        }
        assertEquals(listOf("update-b"), next.matches)
        assertEquals(0, next.readCount)
    }

    @Test
    fun sizeAndTimestampInvalidateWhileMissingContainerKnowledgeSurvives() {
        val original = ExternalContentBootRules.select(titleA, stamps, emptyMap()) {
            titles.getValue(it)
        }
        val changed = listOf(stamps[0].copy(size = 101), stamps[1].copy(modified = 401))
        val reads = mutableListOf<String>()
        val next = ExternalContentBootRules.select(titleA, changed, original.remembered) {
            reads.add(it)
            listOf(titleB) // Replacing a file can change its title, despite a stable URI.
        }
        assertEquals(listOf("update-a", "dlc-a"), reads)
        assertTrue(next.matches.isEmpty())
        assertEquals(setOf("update-a", "dlc-a", "update-b"), next.remembered.keys)
    }

    @Test
    fun missingAndNonpositiveMetadataNeverSuppressesARead() {
        for ((size, modified) in listOf(0L to 0L, -1L to -1L)) {
            val unknown = listOf(ContainerStamp("unknown", size, modified))
            val first = ExternalContentBootRules.select(titleA, unknown, emptyMap()) {
                listOf(titleA)
            }
            assertEquals(listOf("unknown"), first.matches)
            assertTrue(first.remembered.isEmpty())
            val second = ExternalContentBootRules.select(titleB, unknown, first.remembered) {
                listOf(titleB)
            }
            assertEquals(listOf("unknown"), second.matches)
            assertEquals(1, second.readCount)
        }
    }

    @Test
    fun sizeOnlyAndTimestampOnlyMustRereadEvenWithAnExistingCacheEntry() {
        for ((size, modified) in listOf(1L to 0L, 0L to 1L)) {
            val stamp = listOf(ContainerStamp("one-stamp", size, modified))
            val stored = mapOf("one-stamp" to ContainerTitles(size, modified, listOf(titleA)))
            val result = ExternalContentBootRules.select(titleA, stamp, stored) {
                listOf(titleB) // Same-size replacement or unchanged timestamp, different title.
            }
            assertEquals(1, result.readCount)
            assertTrue(result.matches.isEmpty())
            assertEquals(stored, result.remembered) // Invalid stamps cannot erase required evidence.
            val next = ExternalContentBootRules.select(titleB, stamp, result.remembered) {
                listOf(titleB)
            }
            assertEquals(listOf("one-stamp"), next.matches)
            assertEquals(1, next.readCount)
        }
    }

    @Test
    fun unreadableOrMissingKeysRetriesAndOverlappingRootsReadOnlyOnce() {
        val first = ExternalContentBootRules.select(titleA, stamps + stamps, emptyMap()) {
            if (it == "update-a") listOf(0L) else titles.getValue(it)
        }
        assertEquals(3, first.readCount)
        assertEquals(listOf("dlc-a"), first.matches)
        val next = ExternalContentBootRules.select(titleA, stamps, first.remembered) {
            assertEquals("update-a", it)
            listOf(titleA, titleB, titleA)
        }
        assertEquals(1, next.readCount)
        assertEquals(listOf("update-a", "dlc-a"), next.matches)
        assertEquals(listOf(titleA, titleB), next.remembered.getValue("update-a").titles)
    }

    @Test
    fun emptyListingRetainsRequiredKnowledgeAndReadFailureDoesNotMutateStoredCache() {
        val stored = mapOf("gone" to ContainerTitles(1, 1, listOf(titleA)))
        assertEquals(stored, ExternalContentBootRules.select(titleA, emptyList(), stored) {
            error("empty listing")
        }.remembered)
        try {
            ExternalContentBootRules.select(titleA, stamps, stored) {
                throw IllegalStateException("provider failed")
            }
            error("exception must reach caller for full-scan fallback")
        } catch (_: IllegalStateException) {
            assertEquals(mapOf("gone" to ContainerTitles(1, 1, listOf(titleA))), stored)
        }
    }

    @Test
    fun concurrentSelectionsKeepTitlesAndSnapshotsIndependent() {
        val stored = ExternalContentBootRules.select(titleA, stamps, emptyMap()) {
            titles.getValue(it)
        }.remembered
        val pool = Executors.newFixedThreadPool(4)
        try {
            val requests = (0 until 32).map { i ->
                pool.submit<List<String>> {
                    ExternalContentBootRules.select(
                        if (i % 2 == 0) titleA else titleB, stamps, stored
                    ) { error("cache must remain valid") }.matches
                }
            }
            requests.forEachIndexed { i, result ->
                assertEquals(
                    if (i % 2 == 0) listOf("update-a", "dlc-a") else listOf("update-b"),
                    result.get()
                )
            }
            assertEquals(3, stored.size)
        } finally {
            pool.shutdownNow()
        }
    }

    @Test
    fun requiredSnapshotIncludesBothCachesBeforeColdSelectionAndIgnoresOtherTitlesAndRoots() {
        val known = listOf(ContentContainer("game-update", true))
        val stored = mapOf(
            "external-dlc" to ContainerTitles(1, 1, listOf(titleA)),
            "unrelated" to ContainerTitles(1, 1, listOf(titleB)),
            "removed-root" to ContainerTitles(1, 1, listOf(titleA))
        )
        val required = ExternalContentBootRules.requiredContainers(
            titleA, "base", known, stored, emptySet()
        ) { it != "removed-root" }
        assertEquals(setOf("game-update", "external-dlc"), required.map { it.uri }.toSet())
        // A selector seeing an empty listing must not erase the next cold process's evidence.
        val next = ExternalContentBootRules.select(titleA, emptyList(), stored) { emptyList() }
        assertEquals(required, ExternalContentBootRules.requiredContainers(
            titleA, "base", known, next.remembered, emptySet()
        ) { it != "removed-root" })
    }

    @Test
    fun lostSafGrantFailsRequiredRegistrationEvenWhenProviderWasAlreadyMounted() {
        val required = listOf(ContentContainer("selected-update", false))
        val mounted = mutableSetOf<String>()
        assertTrue(!ExternalContentBootRules.mountContainer("selected-update", mounted) { false })
        assertTrue(mounted.isEmpty()) // Failed scans cannot claim the container is mounted.
        assertTrue(ExternalContentBootRules.mountContainer("selected-update", mounted) { true })
        assertEquals(setOf("selected-update"), mounted)
        assertTrue(ExternalContentBootRules.mountContainer("selected-update", mounted) {
            error("ordinary best-effort mounting may reuse the warm provider")
        })
        val attempted = mutableListOf<String>()
        assertTrue(!ExternalContentBootRules.registerRequired(required) {
            attempted.add(it.uri)
            false // JNI OpenFile failed after a previously successful mount lost its SAF grant.
        })
        assertEquals(listOf("selected-update"), attempted)
        assertTrue(ExternalContentBootRules.registerRequired(emptyList()) {
            error("unrelated missing containers must not be opened")
        })
    }

    @Test
    fun authoritativeDisabledUpdatesAndDlcAreWaivedButLegacyUnknownRecordsAreRequired() {
        val records = listOf(
            ContentRecord(titleA, 1, 65536),
            ContentRecord(titleA, 2)
        )
        val known = listOf(
            ContentContainer("update", true, listOf(records[0])),
            ContentContainer("dlc", false, listOf(records[1])),
            ContentContainer("legacy", false),
            ContentContainer("other-base-build", false, listOf(ContentRecord(titleA, 0)))
        )
        val required = ExternalContentBootRules.requiredContainers(
            titleA, "base", known, emptyMap(), setOf("Update@65536", "DLC")
        ) { true }
        assertEquals(listOf("legacy"), required.map { it.uri })
        assertEquals(listOf("update", "legacy"),
            ExternalContentBootRules.requiredContainers(
                titleA, "base", known, emptyMap(), setOf("Update@131072", "DLC")
            ) { true }.map { it.uri })
        val legacy = Json.decodeFromString<ContentContainer>(
            """{"uri":"missing","gameFolder":true}"""
        )
        assertTrue(legacy.records.isEmpty())
        assertEquals(listOf(legacy), ExternalContentBootRules.requiredContainers(
            titleA, "base", listOf(legacy), emptyMap(), setOf("Update", "DLC")
        ) { true })
    }

    @Test
    fun disabledGameFolderGateIsAnIntentionalSuccessfulNoOpButExternalFailuresStillReject() {
        val required = listOf(
            ContentContainer("game-folder", true),
            ContentContainer("external", false)
        )
        val attempts = mutableListOf<String>()
        assertTrue(!ExternalContentBootRules.registerRequired(required) {
            attempts.add(it.uri)
            it.gameFolder // JNI returns true without opening when the native gate is disabled.
        })
        assertEquals(listOf("game-folder", "external"), attempts)
        assertTrue(ExternalContentBootRules.registerRequired(required.take(1)) { it.gameFolder })
    }

    @Test
    fun failedScanCannotOverwriteKnownMissingSelectedOrOtherTitleEntries() {
        val missing = ContentContainer("missing-update", true,
            listOf(ContentRecord(titleA, 1, 65536)))
        val unrelated = ContentContainer("other-title", false)
        val known = mapOf(titleA to listOf(missing), titleB to listOf(unrelated))
        assertEquals(known, ExternalContentBootRules.rememberContainer(
            known, "missing-update", true, true, emptyList()
        ))
        val discovered = ExternalContentBootRules.rememberContainer(
            known, "new-dlc", false, true, listOf(ContentRecord(titleA, 2))
        )
        assertEquals(listOf("missing-update", "new-dlc"),
            discovered.getValue(titleA).map { it.uri })
        assertEquals(listOf(unrelated), discovered.getValue(titleB))
    }

    @Test
    fun directLaunchRegistrationOutsideExternalRootsRemainsRequired() {
        val known = listOf(ContentContainer("direct-update", false,
            listOf(ContentRecord(titleA, 1, 65536)), external = false))
        assertEquals(known, ExternalContentBootRules.requiredContainers(
            titleA, "base", known, emptyMap(), emptySet()
        ) { false })
    }

    @Test
    fun externalMetadataSurvivesColdSerializationAndWaivesOnlyItsDisabledVersion() {
        val cached = mapOf("disabled" to ContainerTitles(1, 1, listOf(titleA),
            listOf(ContentRecord(titleA, 1, 65536))))
        val restored = Json.decodeFromString<Map<String, ContainerTitles>>(Json.encodeToString(cached))
        assertTrue(ExternalContentBootRules.requiredContainers(
            titleA, "base", emptyList(), restored, setOf("Update@65536")
        ) { true }.isEmpty())
        assertEquals(listOf("disabled"), ExternalContentBootRules.requiredContainers(
            titleA, "base", emptyList(), restored, setOf("Update@131072")
        ) { true }.map { it.uri })
    }

    @Test
    fun configuredDocumentRootsIncludeVolumeRootAndExcludeNeighboringDirectoryNames() {
        assertTrue(ExternalContentBootRules.inDocumentRoot("primary:Updates/a.nsp", "primary:"))
        assertTrue(ExternalContentBootRules.inDocumentRoot("primary:Updates/a.nsp", "primary:Updates"))
        assertTrue(!ExternalContentBootRules.inDocumentRoot("primary:Updates-old/a.nsp", "primary:Updates"))
        assertTrue(!ExternalContentBootRules.inDocumentRoot("other:Updates/a.nsp", "primary:"))
    }
}
