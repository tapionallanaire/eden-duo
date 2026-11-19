// SPDX-FileCopyrightText: Copyright 2026 tapionallanaire
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.sync

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class SaveMergeTest {
    @get:Rule val temporary = TemporaryFolder()

    @Test
    fun eitherConflictChoiceKeepsTheReplacedProgressAsABackup() {
        for (keepLocal in listOf(true, false)) {
            val local = temporary.newFile("local-$keepLocal").apply { writeText("device progress") }
            val remote = temporary.newFile("remote-$keepLocal").apply { writeText("synced progress") }
            val backups = temporary.newFolder("backups-$keepLocal")
            var agreedHash: String? = null
            val state = object : SyncState {
                override fun hash(unit: String, side: String, file: File) = org.yuzu.yuzu_emu.duo.sync.hash(file)
                override fun base(unit: String) = agreedHash
                override fun synced(unit: String, hash: String, state: String) { agreedHash = hash }
                override fun conflict(unit: String) = error("Unexpected unresolved conflict")
            }

            resolveConflict("game", local, remote, backups, keepLocal, state)

            val selected = if (keepLocal) "device progress" else "synced progress"
            val replaced = if (keepLocal) "synced progress" else "device progress"
            assertEquals(selected, local.readText())
            assertEquals(selected, remote.readText())
            assertEquals(replaced, backups.walkTopDown().single { it.isFile }.readText())
            assertEquals(hash(local), agreedHash)
        }
    }
}
