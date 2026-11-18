// SPDX-License-Identifier: GPL-3.0-or-later
package org.yuzu.yuzu_emu.duo.nativeplay

import java.io.File
import java.nio.file.Files
import org.junit.Assert.assertTrue
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
