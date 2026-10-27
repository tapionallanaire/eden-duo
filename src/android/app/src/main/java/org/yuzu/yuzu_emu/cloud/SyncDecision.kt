// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.cloud

/** What a title's last successful sync left behind, and what its save looked like then. */
data class SyncRecord(
    val localHash: String,
    val remoteHash: String,
    val fileId: String?,
    val syncedAt: Long
) {
    fun toJson(): String = org.json.JSONObject()
        .put("localHash", localHash)
        .put("remoteHash", remoteHash)
        .put("fileId", fileId ?: "")
        .put("syncedAt", syncedAt)
        .toString()

    companion object {
        fun fromJson(text: String): SyncRecord? = try {
            val json = org.json.JSONObject(text)
            SyncRecord(
                localHash = json.getString("localHash"),
                remoteHash = json.getString("remoteHash"),
                fileId = json.optString("fileId").ifEmpty { null },
                syncedAt = json.optLong("syncedAt")
            )
        } catch (e: Exception) {
            null
        }
    }
}

/** What a sync should do, from three facts only: last sync, local save now, cloud save now. */
enum class SyncAction { NONE, UPLOAD, DOWNLOAD, CONFLICT }

/**
 * Eden Duo: the Steam-style decision.
 *
 * A side changed when its hash no longer matches what the last sync recorded for it. One side
 * changed: that side wins. Neither: nothing to do. Both: the saves fight, unless they happen to be
 * byte-identical, which happens when two devices already synced the same content.
 */
object SyncDecision {
    fun decide(
        record: SyncRecord?,
        localHash: String,
        remoteHash: String?,
        hasLocalData: Boolean
    ): SyncAction {
        if (remoteHash == null) {
            // Nothing in the cloud yet: the first backup, unless there is nothing to back up.
            return if (hasLocalData) SyncAction.UPLOAD else SyncAction.NONE
        }
        if (!hasLocalData) {
            return SyncAction.DOWNLOAD
        }
        if (record == null) {
            // A cloud save and a local save this device never synced: the user must choose.
            return if (localHash == remoteHash) SyncAction.NONE else SyncAction.CONFLICT
        }
        if (localHash == remoteHash) {
            return SyncAction.NONE
        }
        val localChanged = localHash != record.localHash
        val remoteChanged = remoteHash != record.remoteHash
        return when {
            localChanged && !remoteChanged -> SyncAction.UPLOAD
            !localChanged -> SyncAction.DOWNLOAD
            else -> SyncAction.CONFLICT
        }
    }
}
