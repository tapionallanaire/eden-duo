// SPDX-FileCopyrightText: Copyright 2026 tapionallanaire
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.sync

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class SaveSyncBoundaryTest {
    @get:Rule val temporary = TemporaryFolder()

    @Test
    fun aConflictAnswerCannotFollowAProfileOrRootSwitch() {
        val local = temporary.newFile("local").apply { writeText("local progress") }
        val remote = temporary.newFile("remote").apply { writeText("remote progress") }
        val snapshot = SyncConflictSnapshot.capture("account-a/root-a", local, remote)
        assertThrows(IllegalStateException::class.java) {
            snapshot.assertCurrent("account-b/root-a", local, remote)
        }
        assertThrows(IllegalStateException::class.java) {
            snapshot.assertCurrent("account-a/root-b", local, remote)
        }
        assertEquals("local progress", local.readText())
        assertEquals("remote progress", remote.readText())
    }

    @Test
    fun aConflictAnswerRejectsChangedContentEvenWithUnchangedSizeAndTime() {
        val local = temporary.newFile("local").apply { writeText("old"); setLastModified(10_000) }
        val remote = temporary.newFile("remote").apply { writeText("hub") }
        val snapshot = SyncConflictSnapshot.capture("selected", local, remote)
        local.writeText("new")
        local.setLastModified(10_000)
        assertThrows(IllegalStateException::class.java) { snapshot.assertCurrent("selected", local, remote) }
        assertEquals("new", local.readText())
    }

    @Test
    fun conflictAnswerRejectsARepointedUnitAndAcceptsTheUnchangedSelection() {
        val local = temporary.newFile("local").apply { writeText("local") }
        val remote = temporary.newFile("remote").apply { writeText("hub") }
        val snapshot = SyncConflictSnapshot.capture("selected", local, remote)
        snapshot.assertCurrent("selected", local, remote)
        val another = temporary.newFile("another").apply { writeText("local") }
        assertThrows(IllegalStateException::class.java) { snapshot.assertCurrent("selected", another, remote) }
        assertThrows(IllegalStateException::class.java) { snapshot.assertCurrent("selected", null, remote) }
    }

    @Test
    fun changingTheSyncedSaveInvalidatesADelayedAnswer() {
        val local = temporary.newFile("local").apply { writeText("local") }
        val remote = temporary.newFile("remote").apply { writeText("hub") }
        val snapshot = SyncConflictSnapshot.capture("selected", local, remote)
        remote.writeText("new hub progress")
        assertThrows(IllegalStateException::class.java) { snapshot.assertCurrent("selected", local, remote) }
        assertEquals("local", local.readText())
    }

    @Test
    fun dialogSummaryIsAnImmutableSnapshotNotALiveSaveRead() {
        val local = temporary.newFile("local").apply { writeText("local"); setLastModified(10_000) }
        val remote = temporary.newFile("remote").apply { writeText("hub"); setLastModified(20_000) }
        val snapshot = SyncConflictSnapshot.capture("selected", local, remote)
        local.delete()
        remote.delete()
        assertEquals(SyncSaveSummary(10_000, 5), snapshot.localSummary)
        assertEquals(SyncSaveSummary(20_000, 3), snapshot.remoteSummary)
    }

    @Test
    fun remoteUnitCannotEscapeTheApprovedProfileOrAddressADevicePart() {
        val root = temporary.newFolder("profile")
        assertEquals(File(root, "01007EF00011E000").canonicalFile, legacySyncUnit(root, "01007EF00011E000"))
        for (unit in listOf("../other-profile", "/absolute", "01007EF00011E000/device", ".incoming", "")) {
            assertThrows(IllegalStateException::class.java) { legacySyncUnit(root, unit) }
        }
    }

    @Test
    fun aLegacySlotNeverAdoptsTheSelectedProfileWithoutOriginalOwnerEvidence() {
        val root = temporary.newFolder("profile-a")
        assertThrows(IllegalStateException::class.java) { approvedLegacySyncRoot(null, null, "a", root, true) }
        assertNull(approvedLegacySyncRoot(null, null, "a", root, false))
        assertEquals(root.canonicalFile, approvedLegacySyncRoot("a", root.canonicalPath, "a", root, true))
        assertNull(approvedLegacySyncRoot("a", root.canonicalPath, "b", root, true))
        assertNull(approvedLegacySyncRoot("a", root.canonicalPath, "a", File(root, "different-root"), true))
    }

    @Test
    fun explicitLegacyOwnerConfirmationRevalidatesTheSnapshotAndCannotRepointAPinnedPeer() {
        val original = temporary.newFolder("original-account")
        val other = temporary.newFolder("other-account")
        val progress = File(original, "01007EF00011E000").apply { writeText("real local progress") }
        confirmLegacySyncOwner(null, null, "account-a", original, "account-a", original)
        confirmLegacySyncOwner("account-a", original.canonicalPath, "account-a", original, "account-a", original)
        assertThrows(IllegalStateException::class.java) {
            confirmLegacySyncOwner(null, null, "account-a", original, "account-b", original)
        }
        assertThrows(IllegalStateException::class.java) {
            confirmLegacySyncOwner(null, null, "account-a", original, "account-a", other)
        }
        assertThrows(IllegalStateException::class.java) {
            confirmLegacySyncOwner("account-a", original.canonicalPath, "account-b", other, "account-b", other)
        }
        assertEquals("real local progress", progress.readText())
        assertTrue(other.listFiles().orEmpty().isEmpty())
    }
}
