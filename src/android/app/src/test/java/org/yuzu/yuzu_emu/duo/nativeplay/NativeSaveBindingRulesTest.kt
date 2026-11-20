// SPDX-License-Identifier: GPL-3.0-or-later
package org.yuzu.yuzu_emu.duo.nativeplay

import java.io.File
import java.nio.file.Files
import java.nio.file.attribute.PosixFilePermissions
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeFalse
import org.junit.Assume.assumeTrue
import org.junit.Test

class NativeSaveBindingRulesTest {
    private fun rejects(action: () -> Unit) {
        assertTrue(runCatching(action).isFailure)
    }

    @Test fun rejectsChangedProfileAndRootBeforeDelayedSaveAction() {
        NativeSaveBindingRules.requireIdentity("/nand\naccount-A", "/nand\naccount-A")
        rejects { NativeSaveBindingRules.requireIdentity("/nand\naccount-A", "/nand\naccount-B") }
        rejects { NativeSaveBindingRules.requireIdentity("/nand\naccount-A", "/other-nand\naccount-A") }
    }

    @Test fun accountFolderUsesCanonicalWordsRatherThanRawUuidBytes() {
        val words = listOf("7766554433221100", "ffeeddccbbaa9988")
        NativeSaveBindingRules.requireUserWords("FFEEDDCCBBAA99887766554433221100", words)
        rejects { NativeSaveBindingRules.requireUserWords("00112233445566778899AABBCCDDEEFF", words) }
        rejects { NativeSaveBindingRules.requireUserWords("FFEEDDCCBBAA99887766554433221100", words.reversed()) }
    }

    @Test fun onDemandSavesNeedPathsButDoNotInventRetailQuotas() {
        val zero = 0.toBigInteger()
        val one = 1.toBigInteger()
        NativeSaveBindingRules.requirePart(true, zero, zero, "/save")
        NativeSaveBindingRules.requirePart(false, zero, zero, null)
        NativeSaveBindingRules.requirePart(false, zero, one, "/save")
        rejects { NativeSaveBindingRules.requirePart(true, zero, zero, null) }
        rejects { NativeSaveBindingRules.requirePart(false, zero, zero, "/save") }
        rejects { NativeSaveBindingRules.requirePart(false, one, zero, null) }
        rejects { NativeSaveBindingRules.requirePart(true, -one, zero, "/save") }
    }

    private val program = "0100F43008C44000"

    private fun withNand(block: (File, File) -> Unit) {
        val nand = Files.createTempDirectory("native-bcat").toFile().canonicalFile
        try { block(nand, File(nand, "system/save/bcat/$program")) }
        finally { nand.deleteRecursively() }
    }

    @Test fun offlineBcatAdmitsOnlyMissingOrEmptyWithoutChangingSaves() = withNand { nand, cache ->
        val account = File(nand, "user/save/account").apply { parentFile.mkdirs(); writeBytes(byteArrayOf(7)) }
        fun admit(engine: NativeEngine, vararg kinds: String) =
            NativeSaveBindingRules.requireOfflineBcat(engine, kinds.toList(), nand, program)
        admit(NativeEngine.PEANUTS, "bcat")
        assertFalse(File(nand, "system").exists())
        assertTrue(cache.mkdirs())
        admit(NativeEngine.PEANUTS, "bcat")
        rejects { admit(NativeEngine.BANANA, "bcat") }
        rejects { admit(NativeEngine.PEANUTS, "bcat", "temporary") }
        rejects { admit(NativeEngine.PEANUTS, "cache") }
        val data = File(cache, ".delivery").apply { writeBytes(byteArrayOf(42)) }
        rejects { admit(NativeEngine.PEANUTS, "bcat") }
        assertArrayEquals(byteArrayOf(42), data.readBytes())
        assertArrayEquals(byteArrayOf(7), account.readBytes())
        assertTrue(data.delete() && cache.delete())
        cache.writeBytes(byteArrayOf())
        rejects { admit(NativeEngine.PEANUTS, "bcat") }
    }

    @Test fun refusesLinkedBcatAncestorEvenWhenItsTargetIsEmpty() = withNand { nand, cache ->
        assumeTrue('/' == File.separatorChar)
        val target = File(nand, "empty").apply { mkdir() }
        val system = File(nand, "system").toPath()
        Files.createSymbolicLink(system, target.toPath())
        try {
            rejects { NativeSaveBindingRules.requireOfflineBcat(NativeEngine.PEANUTS, listOf("bcat"), nand, program) }
            assertFalse(cache.exists())
        } finally { Files.delete(system) }
    }

    @Test fun refusesUnreadableBcatDirectory() = withNand { nand, cache ->
        assumeTrue(nand.toPath().fileSystem.supportedFileAttributeViews().contains("posix"))
        assertTrue(cache.mkdirs())
        val path = cache.toPath()
        val permissions = Files.getPosixFilePermissions(path)
        try {
            Files.setPosixFilePermissions(path, PosixFilePermissions.fromString("---------"))
            assumeFalse(Files.isReadable(path)) // Privileged test runners bypass mode bits.
            rejects { NativeSaveBindingRules.requireOfflineBcat(NativeEngine.PEANUTS, listOf("bcat"), nand, program) }
        } finally { Files.setPosixFilePermissions(path, permissions) }
    }

    @Test fun physicalPartsRejectEscapesAliasesAndAncestorMappings() {
        val temp = Files.createTempDirectory("native-save-binding").toFile().canonicalFile
        try {
            val root = File(temp, "nand").apply { mkdir() }.canonicalFile
            val account = File(root, "user/account/title").path
            val device = File(root, "system/device/title").path
            NativeSaveBindingRules.requirePhysicalParts(root.path, listOf(account, device))
            rejects { NativeSaveBindingRules.requirePhysicalParts(root.path, listOf(root.path)) }
            rejects { NativeSaveBindingRules.requirePhysicalParts(root.path, listOf(File(temp, "nand-other/title").path)) }
            rejects { NativeSaveBindingRules.requirePhysicalParts(root.path, listOf(File(root, "../escape").path)) }
            rejects { NativeSaveBindingRules.requirePhysicalParts(root.path, listOf(account, account)) }
            rejects { NativeSaveBindingRules.requirePhysicalParts(root.path, listOf(account, "$account/device")) }
            rejects { NativeSaveBindingRules.requirePhysicalParts(root.path, listOf("relative/title")) }
        } finally { temp.deleteRecursively() }
    }
}
