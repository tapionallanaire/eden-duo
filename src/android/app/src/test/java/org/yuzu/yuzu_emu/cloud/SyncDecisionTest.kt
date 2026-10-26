// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.cloud

import org.junit.Assert.assertEquals
import org.junit.Test

/** Eden Duo: the Steam-style three-way rule cloud save sync is built on. */
class SyncDecisionTest {
    private val lastSynced = "hash-1"

    private fun record(local: String = lastSynced, remote: String = lastSynced) =
        SyncRecord(localHash = local, remoteHash = remote, fileId = "file", syncedAt = 0L)

    @Test
    fun firstBackupUploadsTheLocalSave() {
        assertEquals(
            SyncAction.UPLOAD,
            SyncDecision.decide(record = null, localHash = "hash-1", remoteHash = null, hasLocalData = true)
        )
    }

    @Test
    fun emptySavesDoNothing() {
        assertEquals(
            SyncAction.NONE,
            SyncDecision.decide(record = null, localHash = "empty", remoteHash = null, hasLocalData = false)
        )
    }

    @Test
    fun aFreshDevicePullsTheCloudSave() {
        assertEquals(
            SyncAction.DOWNLOAD,
            SyncDecision.decide(record = null, localHash = "empty", remoteHash = "cloud", hasLocalData = false)
        )
    }

    @Test
    fun aFreshDeviceWithItsOwnSaveIsAConflict() {
        assertEquals(
            SyncAction.CONFLICT,
            SyncDecision.decide(record = null, localHash = "here", remoteHash = "cloud", hasLocalData = true)
        )
    }

    @Test
    fun aFreshDeviceThatAlreadyHasTheSameSaveIsSettled() {
        assertEquals(
            SyncAction.NONE,
            SyncDecision.decide(record = null, localHash = "same", remoteHash = "same", hasLocalData = true)
        )
    }

    @Test
    fun playingChangesTheLocalSideAndUploads() {
        assertEquals(
            SyncAction.UPLOAD,
            SyncDecision.decide(record = record(), localHash = "hash-2", remoteHash = lastSynced, hasLocalData = true)
        )
    }

    @Test
    fun anotherDeviceUploadingDownloadsHere() {
        assertEquals(
            SyncAction.DOWNLOAD,
            SyncDecision.decide(record = record(), localHash = lastSynced, remoteHash = "hash-2", hasLocalData = true)
        )
    }

    @Test
    fun bothSidesMovingIsAConflict() {
        assertEquals(
            SyncAction.CONFLICT,
            SyncDecision.decide(record = record(), localHash = "here", remoteHash = "cloud", hasLocalData = true)
        )
    }

    @Test
    fun identicalSavesAreNotAConflictEvenWhenBothMoved() {
        assertEquals(
            SyncAction.NONE,
            SyncDecision.decide(record = record(), localHash = "same", remoteHash = "same", hasLocalData = true)
        )
    }

    @Test
    fun aLocalSaveThatVanishedPullsTheCloudCopyBack() {
        assertEquals(
            SyncAction.DOWNLOAD,
            SyncDecision.decide(record = record(), localHash = "empty", remoteHash = lastSynced, hasLocalData = false)
        )
    }
}
