// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.utils

import java.io.File
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

/**
 * Eden Duo: the archive rules a warm-up's save restore and cloud sync both stand on. If a restore
 * wrote the wrong files or dropped one, a warm-up would eat a save.
 */
class SaveArchiveTest {
    @get:Rule
    val temporary = TemporaryFolder()

    private fun save(vararg files: Pair<String, String>): File {
        val dir = temporary.newFolder()
        for ((name, content) in files) {
            val file = File(dir, name)
            file.parentFile?.mkdirs()
            file.writeText(content)
        }
        return dir
    }

    @Test
    fun roundTripRestoresEveryFileAndNestedFolder() {
        val source = save(
            "save.dat" to "world",
            "sub/second.bin" to "more",
            "sub/deeper/third.txt" to "deepest"
        )
        val bytes = SaveArchive.zip(source)

        val target = temporary.newFolder()
        SaveArchive.extract(bytes, target)

        assertEquals("world", File(target, "save.dat").readText())
        assertEquals("more", File(target, "sub/second.bin").readText())
        assertEquals("deepest", File(target, "sub/deeper/third.txt").readText())
        assertEquals(SaveArchive.hash(source), SaveArchive.hash(target))
    }

    @Test
    fun extractReplacesRatherThanMerges() {
        val incoming = SaveArchive.zip(save("keep.dat" to "new"))
        val target = save("keep.dat" to "old", "stale.dat" to "remove me")

        SaveArchive.extract(incoming, target)

        assertEquals("new", File(target, "keep.dat").readText())
        assertFalse(File(target, "stale.dat").exists())
    }

    @Test
    fun identicalContentHashesTheSameRegardlessOfWriteOrder() {
        val a = save("one" to "1", "two" to "2")
        val b = save("two" to "2", "one" to "1")
        assertEquals(SaveArchive.hash(a), SaveArchive.hash(b))
    }

    @Test
    fun changedContentChangesTheHash() {
        val before = save("save.dat" to "world")
        val after = save("save.dat" to "worlds")
        assertNotEquals(SaveArchive.hash(before), SaveArchive.hash(after))
    }

    @Test
    fun theSizeMarkerAloneIsNotData() {
        val dir = save(".yuzu_save_size" to "0")
        assertFalse(SaveArchive.hasData(dir))
    }

    @Test(expected = java.io.IOException::class)
    fun extractRefusesAPathThatEscapesTheFolder() {
        val out = java.io.ByteArrayOutputStream()
        ZipOutputStream(out).use { zip ->
            zip.putNextEntry(ZipEntry("../escaped.txt"))
            zip.write("nope".toByteArray())
            zip.closeEntry()
        }
        SaveArchive.extract(out.toByteArray(), temporary.newFolder())
    }

    @Test
    fun emptyFoldersStayEmpty() {
        val dir = temporary.newFolder()
        assertTrue(SaveArchive.files(dir).isEmpty())
        assertFalse(SaveArchive.hasData(dir))
        assertEquals(SaveArchive.hash(dir), SaveArchive.hash(dir))
    }
}
