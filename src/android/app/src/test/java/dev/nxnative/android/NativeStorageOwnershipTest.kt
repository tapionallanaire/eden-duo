// SPDX-License-Identifier: GPL-3.0-or-later
package dev.nxnative.android

import java.io.File
import java.io.IOException
import java.nio.file.Files
import org.json.JSONObject
import org.junit.Assert.*
import org.junit.Test

class NativeStorageOwnershipTest {
    @Test fun privateOwnersFindRemovedSourcesAndAllCollectionMembersWithoutAdoptingOtherDirectories() {
        val root = Files.createTempDirectory("storage-owners").toFile()
        try {
            val menu = "0100000000000000"
            val member = "0100000000000001"
            val first = File(root, "removed.nsp").path
            val moved = File(root, "moved.nsp").path
            fun record(dir: String, id: String, path: String, job: Boolean) {
                val identity = JSONObject().put("title_id", id).put("original_path", path)
                File(root, "$dir/identity.json").apply {
                    parentFile.mkdirs()
                    writeText(if (job) JSONObject().put("title_id", id).put("embedded", identity).toString() else identity.toString())
                }
            }
            record("native-peanuts-control/00000000-0000-0000-0000-000000000001", menu, first, true)
            record("native-peanuts-input-control/${"a".repeat(64)}", menu, moved, false)
            record("native-peanuts-input-control/${"b".repeat(64)}", member, first, false)
            record("native-peanuts-control/not-owned", member, "foreign", true)
            assertEquals(mapOf(menu to setOf(first, moved), member to setOf(first)), EmbeddedGame.storageOwners(root, "peanuts"))
        } finally { root.deleteRecursively() }
    }
    @Test fun retainedWorkAndOnlyTheFourRegisteredInputNamesCountOnce() {
        val root = Files.createTempDirectory("storage-count").toFile()
        try {
            val key = "1".repeat(64)
            val id = "0100000000000001"
            val directory = File(root, "inputs/$id/${CompiledFiles.selectionKey(key)}").apply { mkdirs() }
            File(directory, "game.nup").writeBytes(ByteArray(3))
            File(directory, "game.nup.json").writeBytes(ByteArray(5))
            File(directory, "game.nup.part").writeBytes(ByteArray(7))
            File(directory, "game.nup.json.part").writeBytes(ByteArray(11))
            File(directory, "save.dat").writeBytes(ByteArray(127))
            val cache = File(root, "jobs/staging/object").apply { parentFile.mkdirs(); writeBytes(ByteArray(13)) }
            val files = CompiledFiles(root, File(root.parentFile, "gone.nsp").path)
            repeat(2) {
                files.inputFiles(File(directory, "game.nup"), id, key).forEach { files.add(it, true) }
                files.add(cache.parentFile, false)
            }
            assertEquals(26L, files.inputBytes())
            assertEquals(13L, files.bytes())
        } finally { root.deleteRecursively() }
    }


    @Test fun aggregateCannotRemoveAnotherOriginalNestedInsideAnOwnedTree() {
        val root = Files.createTempDirectory("storage-protect").toFile()
        try {
            val owned = File(root, "jobs/one").apply { mkdirs() }
            val original = File(owned, "original.nsp").apply { writeText("keep") }
            val files = CompiledFiles(root, null)
            files.protectOriginal(File(root, "unrelated").path)
            files.protectOriginal(original.toURI().toString())
            assertThrows(IOException::class.java) { files.add(owned, false) }
            assertEquals("keep", original.readText())
            assertFalse(files.present())
        } finally { root.deleteRecursively() }
    }
}
