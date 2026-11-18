// SPDX-License-Identifier: GPL-3.0-or-later
package org.yuzu.yuzu_emu.features

import java.io.File
import java.io.FileNotFoundException
import java.nio.file.Files
import org.junit.Assume.assumeNoException
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class DocumentProviderPathsTest {
    @get:Rule val temporary = TemporaryFolder()

    @Test fun resolvesOnlyTheActualProviderRoot() {
        val root = temporary.newFolder("user")
        val save = File(root, "nand/user/save/progress").apply {
            parentFile!!.mkdirs()
            writeText("progress")
        }
        assertEquals(root.canonicalFile, DocumentProviderPaths.resolve(root, "root"))
        assertEquals(save.canonicalFile, DocumentProviderPaths.resolve(root, "root/nand/user/save/progress"))
        assertEquals("root/nand/user/save/progress", DocumentProviderPaths.documentId(root, save))
        val outside = temporary.newFile("outside")
        for (id in listOf("root/../outside", "rooted/../outside", "other/outside")) {
            refused { DocumentProviderPaths.resolve(root, id) }
        }
        refused { DocumentProviderPaths.documentId(root, outside) }
    }

    @Test fun rejectsNamesThatCouldMoveOrCreateOutsideTheChosenDirectory() {
        val root = temporary.newFolder("user")
        for (name in listOf("", ".", "..", "../outside", "nested/save", "nested\\save", "/outside")) {
            refused { DocumentProviderPaths.child(root, name) }
        }
        assertEquals(File(root, "progress.dat"), DocumentProviderPaths.child(root, "progress.dat"))
    }

    @Test fun rejectsSymlinkTargetsOutsideTheProviderRoot() {
        val root = temporary.newFolder("user")
        val outside = temporary.newFile("outside")
        val link = File(root, "linked-save")
        try {
            Files.createSymbolicLink(link.toPath(), outside.toPath())
        } catch (e: Exception) {
            assumeNoException("This host cannot create test symlinks", e)
            return
        }
        refused { DocumentProviderPaths.resolve(root, "root/linked-save") }
        refused { DocumentProviderPaths.child(root, "linked-save") }
        refused { DocumentProviderPaths.documentId(root, link) }
        val protectedSave = File(root, "nand/user/save/profile/progress").apply {
            parentFile!!.mkdirs()
            writeText("progress")
        }
        val disguised = File(root, "game.nsp")
        Files.createSymbolicLink(disguised.toPath(), protectedSave.toPath())
        assertTrue(DocumentProviderPaths.requiresSaveLease(
            root, disguised, listOf(File(root, "nand/user/save").canonicalPath)
        ))
    }

    @Test fun onlyNonSaveReadsCanBypassTheSessionAndUnknownBoundariesFailClosed() {
        val root = temporary.newFolder("user")
        val protectedTrees = listOf(
            File(root, "nand/user/save").canonicalPath,
            File(root, "nand/system/save").canonicalPath,
            File(root, "custom/user/save").canonicalPath
        )
        for (path in listOf(
            "", "nand", "nand/user", "nand/user/save", "nand/user/save/profile/title/progress",
            "nand/system/save/profile", "nand/user/save.before-transfer/title/1",
            "custom/user/save/profile/title", "config", "config/config.ini"
        )) {
            assertTrue(path, DocumentProviderPaths.requiresSaveLease(root, File(root, path), protectedTrees))
        }
        for (path in listOf("games/title.nsp", "nand/user/Contents/title.nca", "nand/user/save2/title.nsp")) {
            assertFalse(path, DocumentProviderPaths.requiresSaveLease(root, File(root, path), protectedTrees))
        }
        assertTrue(DocumentProviderPaths.requiresSaveLease(root, File(root, "games/title.nsp"), null))
    }

    private fun refused(action: () -> Unit) {
        try {
            action()
            fail("An outside-root document or path name was accepted")
        } catch (_: FileNotFoundException) {
            // The provider must refuse before any read, create, rename, or delete.
        }
    }
}
