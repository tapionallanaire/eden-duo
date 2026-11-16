// SPDX-FileCopyrightText: Copyright 2026 tapionallanaire
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.savetransfer

import java.io.ByteArrayInputStream
import java.io.File
import java.io.IOException
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class SaveTreeTest {
    @get:Rule
    val tmp = TemporaryFolder()

    private val title = "01007EF00011E000"
    private val profile = "EBABE35DEA2FD45AB95E2BFCBDCCDC30"

    /** An in-memory documents tree: id -> children; files carry their bytes and time. */
    private val dirs = mutableMapOf<String, MutableList<SaveTree.Doc>>()
    private val bytes = mutableMapOf<String, ByteArray>()

    private fun dir(parent: String?, name: String): String {
        val id = if (parent == null) name else "$parent/$name"
        dirs.getOrPut(id) { mutableListOf() }
        if (parent != null) dirs.getValue(parent).add(SaveTree.Doc(id, name, true, 0, 0))
        return id
    }

    private fun file(parent: String, name: String, data: String, modified: Long) {
        val id = "$parent/$name"
        bytes[id] = data.toByteArray()
        dirs.getValue(parent).add(SaveTree.Doc(id, name, false, modified, data.length.toLong()))
    }

    private fun tree() = SaveTree(
        children = { dirs[it] ?: throw IOException("no $it") },
        open = { ByteArrayInputStream(bytes.getValue(it)) }
    )

    /** root/nand/user/save/0000000000000000/<profile>, returning the profile folder id. */
    private fun profileDir(): String {
        var id = dir(null, "root")
        for (name in listOf("nand", "user", "save", "0000000000000000", profile)) id = dir(id, name)
        return id
    }

    @Test
    fun nestedFoldersCountAndCopy() {
        val game = dir(profileDir(), title)
        file(game, "option.sav", "o", 1_000)
        val slot = dir(game, "0")
        file(slot, "game_data.sav", "slot0", 5_000)

        val saves = tree().saves("root")
        assertEquals(listOf(SaveEntry(title, profile, 5_000, 6, 2)), saves)

        val into = tmp.newFolder("into")
        tree().copyTitle("root", saves.single(), into)
        assertEquals("o", File(into, "option.sav").readText())
        assertEquals("slot0", File(into, "0/game_data.sav").readText())
        // Copies keep the source's times, so the two saves compare as the same afterwards.
        assertEquals(5_000, File(into, "0/game_data.sav").lastModified())
    }

    @Test
    fun sizeMarkerAloneIsEmpty() {
        val game = dir(profileDir(), title)
        file(game, ".yuzu_save_size", "12345678", 9_000)
        val saves = tree().saves("root")
        assertEquals(0, saves.single().fileCount)
        assertTrue(SaveCatalog.newestPerTitle(saves).isEmpty())
    }

    @Test
    fun saveFolderFromConfig() {
        val authority = "dev.igawa6.edenduo.user"
        val own = "/storage/emulated/0/Android/data/dev.igawa6.edenduo/files"
        fun location(config: String?) = SaveSources.saveLocation(config, authority)
        assertEquals(SaveSources.SaveLocation.Own(listOf("nand")), location(null))
        // save_directory empty: Eden falls back to the NAND folder.
        assertEquals(
            SaveSources.SaveLocation.Own(listOf("nand")),
            location("[UI]\nPaths\\nand_directory=$own/nand\nPaths\\save_directory=\nPaths\\save_directory\\default=true\n")
        )
        val sd = "content://com.android.externalstorage.documents/tree/DA4E-BBBE%3ARoms%2FSwitch"
        assertEquals(
            SaveSources.SaveLocation.Elsewhere(sd),
            location("[Data%20Storage]\nnand_directory=$own/nand\nsave_directory=\"$sd\"\nsave_directory\\default=false\n")
        )
        assertEquals(
            SaveSources.SaveLocation.Own(listOf("imported", "saves-1")),
            location("[UI]\nPaths\\save_directory=content://$authority/tree/root%2Fimported%2Fsaves-1\n")
        )
    }

    @Test
    fun savesUnderAnotherBase() {
        val root = tmp.newFolder("sd")
        val game = File(root, "user/save/0000000000000000/$profile/$title").apply { mkdirs() }
        File(game, "data.sav").apply { writeText("d"); setLastModified(7_000) }
        val disk = SaveTree(
            children = { id -> File(id).listFiles()!!.map { SaveTree.Doc(it.path, it.name, it.isDirectory, it.lastModified(), it.length()) } },
            open = { File(it).inputStream() },
            base = emptyList()
        )
        assertEquals(listOf(SaveEntry(title, profile, 7_000, 1, 1)), disk.saves(root.path))
    }

    @Test
    fun noSaveFolderMeansNoSaves() {
        dir(null, "root")
        assertTrue(tree().saves("root").isEmpty())
    }
}
