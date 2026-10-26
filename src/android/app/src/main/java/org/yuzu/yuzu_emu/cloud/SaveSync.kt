// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.cloud

import android.content.Context
import java.io.File
import java.io.IOException
import java.util.concurrent.ConcurrentHashMap
import org.json.JSONObject
import org.yuzu.yuzu_emu.utils.Log
import org.yuzu.yuzu_emu.utils.NativeConfig

/**
 * Eden Duo: a game's save folder, kept in step with the one in the user's Drive.
 *
 * One zip per title lives in Drive's hidden app folder; this device remembers, per title, the save
 * it uploaded or downloaded last and the cloud copy it saw then. Every sync compares three hashes -
 * what was synced, what is here now, what is in the cloud now - and that is the whole conflict rule
 * (see [SyncDecision]). Nothing is ever overwritten without the replaced version being kept.
 */
object SaveSync {
    /** Where the local save for [programId] lives: the emulator's own save folder for the game. */
    fun saveDir(context: Context, programId: String): File =
        File(NativeConfig.getSaveDir() + org.yuzu.yuzu_emu.NativeLibrary.getDefaultProfileSaveDataRoot(false), programId)

    enum class Policy { ASK, KEEP_LOCAL, USE_CLOUD }

    sealed class Outcome {
        data class Synced(val action: SyncAction, val remoteTime: Long) : Outcome()
        data object Skipped : Outcome()
        data class Conflict(val remoteTime: Long) : Outcome()
        data class Failed(val message: String) : Outcome()
    }

    private val records = ConcurrentHashMap<String, SyncRecord>()

    private fun stateFile(context: Context): File =
        File(context.filesDir, "cloudsaves/state.json")

    private fun loadRecords(context: Context) {
        if (records.isNotEmpty()) return
        val file = stateFile(context)
        if (!file.isFile) return
        try {
            val json = JSONObject(file.readText())
            for (title in json.keys()) {
                SyncRecord.fromJson(json.getJSONObject(title).toString())?.let { records[title] = it }
            }
        } catch (e: Exception) {
            Log.warning("[CloudSaves] unreadable sync state: $e")
        }
    }

    private fun saveRecord(context: Context, programId: String, record: SyncRecord) {
        records[programId] = record
        val file = stateFile(context)
        file.parentFile?.mkdirs()
        val json = JSONObject()
        for ((title, saved) in records) {
            json.put(title, JSONObject(saved.toJson()))
        }
        file.writeText(json.toString())
    }

    /**
     * Syncs [programId]'s save. Blocking and network-bound: call from a worker thread. With
     * [Policy.ASK], a disagreement is reported and nothing is changed; the caller resolves it with
     * [resolve], which is also how the "use cloud" / "keep this device" answer is applied.
     */
    fun sync(context: Context, programId: String, policy: Policy): Outcome {
        if (!CloudSavesSettings.enabled(context)) return Outcome.Skipped
        if (!CloudAuth.isConfigured(context)) return Outcome.Skipped
        if (CloudAuth.account(context) == null) return Outcome.Skipped

        return try {
            loadRecords(context)
            val dir = saveDir(context, programId)
            val localHash = SaveArchive.hash(dir)
            val hasData = SaveArchive.hasData(dir)
            val remote = DriveClient.findSave(context, programId)
            val action = SyncDecision.decide(records[programId], localHash, remote?.hash, hasData)

            when (action) {
                SyncAction.NONE -> {
                    if (remote?.hash != null) {
                        saveRecord(
                            context, programId,
                            SyncRecord(localHash, remote.hash, remote.id, System.currentTimeMillis())
                        )
                    }
                    Outcome.Synced(SyncAction.NONE, remote?.modifiedTime ?: 0L)
                }

                SyncAction.UPLOAD -> upload(context, programId, dir, localHash, remote?.id)

                SyncAction.DOWNLOAD -> download(context, programId, dir, remote!!)

                SyncAction.CONFLICT -> when (policy) {
                    Policy.ASK -> Outcome.Conflict(remote?.modifiedTime ?: 0L)
                    Policy.KEEP_LOCAL -> keepLocal(context, programId, dir, localHash, remote)
                    Policy.USE_CLOUD -> keepCloud(context, programId, dir, remote!!)
                }
            }
        } catch (e: IOException) {
            Log.warning("[CloudSaves] sync failed for $programId: $e")
            Outcome.Failed(e.message ?: "cloud error")
        } catch (e: Exception) {
            Log.warning("[CloudSaves] sync failed for $programId: $e")
            Outcome.Failed(e.message ?: "cloud error")
        }
    }

    /** Applies the answer to a conflict [sync] reported: true keeps this device's save. */
    fun resolve(context: Context, programId: String, keepLocal: Boolean): Outcome = try {
        loadRecords(context)
        val dir = saveDir(context, programId)
        val localHash = SaveArchive.hash(dir)
        val remote = DriveClient.findSave(context, programId)
        if (remote == null) {
            if (SaveArchive.hasData(dir)) upload(context, programId, dir, localHash, null) else Outcome.Skipped
        } else if (keepLocal) {
            keepLocal(context, programId, dir, localHash, remote)
        } else {
            keepCloud(context, programId, dir, remote)
        }
    } catch (e: Exception) {
        Outcome.Failed(e.message ?: "cloud error")
    }

    /** This device's save wins; the cloud copy is renamed aside first so it is not lost. */
    private fun keepLocal(
        context: Context,
        programId: String,
        dir: File,
        localHash: String,
        remote: DriveClient.RemoteSave?
    ): Outcome {
        if (remote != null) {
            val stamp = System.currentTimeMillis() / 1000
            DriveClient.rename(context, remote.id, "save_${programId.uppercase()}-conflict-$stamp.zip")
        }
        return upload(context, programId, dir, localHash, null)
    }

    /** The cloud save wins; this device's version is kept in the app's own folder first. */
    private fun keepCloud(
        context: Context,
        programId: String,
        dir: File,
        remote: DriveClient.RemoteSave
    ): Outcome {
        val bytes = DriveClient.download(context, remote.id)
        if (SaveArchive.hasData(dir)) {
            val stamp = System.currentTimeMillis() / 1000
            val aside = File(context.filesDir, "cloudsaves/conflicts/${programId.uppercase()}-$stamp.zip")
            aside.parentFile?.mkdirs()
            aside.writeBytes(SaveArchive.zip(dir))
            Log.info("[CloudSaves] kept this device's save at ${aside.path}")
        }
        SaveArchive.extract(bytes, dir)
        val hash = remote.hash ?: SaveArchive.hash(dir)
        saveRecord(context, programId, SyncRecord(hash, hash, remote.id, System.currentTimeMillis()))
        return Outcome.Synced(SyncAction.DOWNLOAD, remote.modifiedTime)
    }

    private fun upload(
        context: Context,
        programId: String,
        dir: File,
        localHash: String,
        existingId: String?
    ): Outcome {
        val bytes = SaveArchive.zip(dir)
        val uploaded = DriveClient.upload(context, programId, existingId, bytes, localHash)
        saveRecord(
            context, programId,
            SyncRecord(localHash, localHash, uploaded.id, System.currentTimeMillis())
        )
        Log.info("[CloudSaves] uploaded $programId (${bytes.size} bytes)")
        return Outcome.Synced(SyncAction.UPLOAD, uploaded.modifiedTime)
    }

    private fun download(
        context: Context,
        programId: String,
        dir: File,
        remote: DriveClient.RemoteSave
    ): Outcome {
        val bytes = DriveClient.download(context, remote.id)
        SaveArchive.extract(bytes, dir)
        val hash = remote.hash ?: SaveArchive.hash(dir)
        saveRecord(context, programId, SyncRecord(hash, hash, remote.id, System.currentTimeMillis()))
        Log.info("[CloudSaves] downloaded $programId (${bytes.size} bytes)")
        return Outcome.Synced(SyncAction.DOWNLOAD, remote.modifiedTime)
    }
}
