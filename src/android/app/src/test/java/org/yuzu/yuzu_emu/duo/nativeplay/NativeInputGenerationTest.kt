// SPDX-FileCopyrightText: Copyright 2026 Duet Plus contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.nativeplay

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class NativeInputGenerationTest {
    @get:Rule
    val tmp = TemporaryFolder()

    // The filesystem selector delegates NUP/content validation to JNI in production.
    private fun select(root: File, preferred: File? = null): File =
        NativeGameSupport.selectInputGeneration(root, preferred) { input ->
            input.readText() == "validated snapshot" &&
                File(input.path + ".json").readText() == "validated identity"
        }

    private fun complete(input: File) {
        input.parentFile!!.mkdirs()
        input.writeText("validated snapshot")
        File(input.path + ".json").writeText("validated identity")
    }

    @Test
    fun reusesCompleteSnapshotInsteadOfCopyingAfterAnOrphanedExport() {
        val root = tmp.newFolder("inputs").canonicalFile
        val committed = File(root, "game.nup")
        complete(committed)
        val interrupted = select(tmp.newFolder("other").canonicalFile)
        // A saved path for another content selection must not displace this valid snapshot.
        interrupted.parentFile!!.mkdirs()
        interrupted.writeText("unfinished other content")
        val names = root.list()!!.toSet()

        assertEquals(committed, select(root, interrupted))
        assertEquals(names, root.list()!!.toSet())
        assertEquals("validated snapshot", committed.readText())
        assertEquals("unfinished other content", interrupted.readText())
    }

    @Test
    fun retriesOrphansAndCorruptPairsWithoutReplacingAnyExistingNames() {
        val root = tmp.newFolder("inputs").canonicalFile
        val orphan = File(root, "game.nup").apply { writeText("published without metadata") }
        val orphanMetadata = File(orphan.path + ".json.part").apply { writeText("partial metadata") }
        val corrupt = select(root).also(::complete)
        corrupt.writeText("corrupt snapshot")
        val cancelled = select(root, corrupt)
        cancelled.parentFile!!.mkdirs()
        val part = File(cancelled.path + ".part").apply { writeText("cancelled write") }
        val journal = File(cancelled.path + ".journal").apply { writeText("retained journal") }

        val fresh = select(root, cancelled)

        assertNotEquals(orphan, fresh)
        assertNotEquals(corrupt, fresh)
        assertNotEquals(cancelled, fresh)
        assertFalse(fresh.exists())
        assertEquals("published without metadata", orphan.readText())
        assertEquals("partial metadata", orphanMetadata.readText())
        assertEquals("corrupt snapshot", corrupt.readText())
        assertEquals("validated identity", File(corrupt.path + ".json").readText())
        assertEquals("cancelled write", part.readText())
        assertEquals("retained journal", journal.readText())
    }

    @Test
    fun recreationRetainsItsPristineReservationAndThenReusesTheCompletedGeneration() {
        val root = tmp.newFolder("inputs").canonicalFile
        val reserved = select(root)
        assertEquals(reserved, select(root, File(reserved.path)))
        complete(reserved)
        // A stale extra staging name does not invalidate an independently validated pair.
        val staging = File(reserved.path + ".json.part").apply { writeText("unowned staging") }
        val names = root.list()!!.toSet()

        assertEquals(reserved, select(root))
        assertEquals(reserved, select(root, File(reserved.path)))
        assertEquals(names, root.list()!!.toSet())
        assertEquals("unowned staging", staging.readText())
    }

    @Test
    fun exporterClaimsGenerationExclusivelyWithoutAdoptingAnotherLiveClaim() {
        val root = tmp.newFolder("inputs").canonicalFile
        val input = select(root)
        assertEquals(input, select(root, input))
        assertTrue("The exporter must be able to claim its generation atomically", input.parentFile!!.mkdir())
        val marker = File(input.parentFile, "producer").apply { writeText("first exporter") }

        val competing = select(root, input)
        assertNotEquals(input, competing)
        assertTrue(competing.parentFile!!.mkdir())
        assertFalse(input.parentFile!!.mkdir())
        assertEquals("first exporter", marker.readText())
    }
}
