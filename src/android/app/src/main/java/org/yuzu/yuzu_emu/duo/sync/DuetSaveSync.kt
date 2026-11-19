// SPDX-FileCopyrightText: Copyright 2026 tapionallanaire
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.sync

import android.content.Context
import androidx.preference.PreferenceManager
import java.io.File
import kotlinx.serialization.json.Json
import org.yuzu.yuzu_emu.duo.nativeplay.NativeSaveSession
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.utils.GameHelper
import org.yuzu.yuzu_emu.utils.PathUtil

/** Old SQLite hash stamps contain no local path: attaching a profile requires explicit approval. */
internal fun approvedLegacySyncRoot(
    approvedIdentity: String?,
    approvedRoot: String?,
    identity: String,
    root: File,
    legacyPaired: Boolean
): File? {
    if (legacyPaired && (approvedIdentity == null || approvedRoot == null)) throw UnpairedSyncException(
        "Choose a profile for the existing Switch sync connection in Save sync settings. Saves stay local until you approve."
    )
    return root.canonicalFile.takeIf { approvedIdentity == identity && approvedRoot == it.path }
}

private val legacySaveUnitId = Regex("[0-9A-Fa-f]{16}")

internal fun legacySyncUnit(root: File, unit: String): File {
    check(unit.matches(legacySaveUnitId)) { "Invalid Switch save unit: $unit" }
    val file = File(root, unit).canonicalFile
    check(file.parentFile == root.canonicalFile) { "Switch save unit leaves its approved profile root." }
    return file
}

internal fun legacySyncUnits(root: File, remote: List<String>): MutableMap<String, File> {
    val local = root.listFiles().orEmpty()
        .filter { it.isDirectory && it.name.matches(legacySaveUnitId) }.map { it.name }
    return (local + remote).distinct().associateWith { legacySyncUnit(root, it) }.toMutableMap()
}

internal fun confirmLegacySyncOwner(
    approvedIdentity: String?,
    approvedRoot: String?,
    capturedIdentity: String,
    capturedRoot: File,
    currentIdentity: String,
    currentRoot: File
) {
    check(capturedIdentity == currentIdentity && capturedRoot.canonicalFile == currentRoot.canonicalFile) {
        "The selected account or save root changed. Open Save sync again before confirming."
    }
    check((approvedIdentity == null && approvedRoot == null) ||
        (approvedIdentity == capturedIdentity && approvedRoot == capturedRoot.canonicalPath)) {
        "The existing Switch peer already belongs to another account/root and cannot be repointed."
    }
}
/**
 * Duet Plus syncs Switch saves only: the `switch/` part of the "calamansi-saves" folder that Calamansi (the main
 * client, every console) shares through the same PC. Its own ports, so both apps can run Syncthing on one device.
 */
object DuetSaveSync {
    const val SWITCH = "switch"

    @Volatile
    private var instance: SaveSync? = null

    private fun preferences(context: Context) = context.getSharedPreferences("save_sync", Context.MODE_PRIVATE)

    private fun selectedRoot(): File {
        val saveRoot = PathUtil.saveDir()
        check(File(saveRoot).isAbsolute && !saveRoot.startsWith("content:")) { "Save sync needs a physical save root." }
        val relative = NativeLibrary.getDefaultProfileSaveDataRoot(false)
        check(relative.isNotEmpty()) { "No selected Switch save profile." }
        return File(saveRoot.trimEnd('/') + "/" + relative.trimStart('/')).canonicalFile
    }

    private fun approvedRoot(context: Context): File? {
        val prefs = preferences(context)
        val legacy = prefs.contains("hub") || prefs.getBoolean("switch_legacy_owner_unknown", false)
        if (legacy && !prefs.contains("switch_owner_identity")) {
            check(prefs.edit().putBoolean("switch_legacy_owner_unknown", true).commit()) { "Cannot preserve legacy pairing ownership." }
        }
        return approvedLegacySyncRoot(
            prefs.getString("switch_owner_identity", null), prefs.getString("switch_owner_root", null),
            NativeSaveSession.identity(context), selectedRoot(), legacy
        )
    }

    private fun preparePairing(context: Context) {
        val prefs = preferences(context)
        // Disconnect/restart must not attach an unapproved profile to an existing shared save slot.
        approvedRoot(context)
        if (prefs.contains("switch_owner_identity")) {
            check(approvedRoot(context) != null) { "This Switch peer belongs to another profile/root. The selected profile stays local-only." }
            return
        }
        val identity = NativeSaveSession.identity(context)
        val root = selectedRoot()
        NativeSaveSession.assertIdentity(context, identity)
        check(prefs.edit().putString("switch_owner_identity", identity)
            .putString("switch_owner_root", root.path).commit()) { "Cannot persist Switch peer ownership." }
    }

    private fun ownerConfirmation(context: Context): SyncOwnerConfirmation? {
        val prefs = preferences(context)
        if (prefs.contains("switch_owner_identity") ||
            !(prefs.contains("hub") || prefs.getBoolean("switch_legacy_owner_unknown", false))) return null
        val identity = NativeSaveSession.identity(context)
        val root = selectedRoot()
        val hub = prefs.getString("hub", null)
        val user = checkNotNull(NativeLibrary.getCurrentUser()) { "Select an account before connecting save sync." }
        check(user.isNotEmpty()) { "Select an account before connecting save sync." }
        val name = NativeLibrary.getUserUsername(user).orEmpty().ifBlank { user }
        return SyncOwnerConfirmation(
            "Use $name ($user) for this PC's shared Switch saves?\n\n" +
                "The old connection did not record its account. Where this device and the PC have different saves, " +
                "you can choose which copy to keep. Saves present on only one side copy automatically.\n\n" +
                "Native and emulated play already share this profile's local saves. Other profiles and device saves stay local. " +
                "Continuing connects this profile; it does not copy any saves until you sync."
        ) {
            NativeSaveSession.assertIdentity(context, identity)
            check(prefs.getString("hub", null) == hub) { "The paired hub changed. Open Save sync again before confirming." }
            confirmLegacySyncOwner(prefs.getString("switch_owner_identity", null),
                prefs.getString("switch_owner_root", null), identity, root,
                NativeSaveSession.identity(context), selectedRoot())
            get(context).resetSyncHistory(SWITCH)
            check(prefs.edit().putString("switch_owner_identity", identity)
                .putString("switch_owner_root", root.path).remove("switch_legacy_owner_unknown").commit()) {
                "Cannot persist the approved Switch sync profile."
            }
        }
    }

    private fun units(context: Context, remote: List<String>): Map<String, File> {
        val root = approvedRoot(context) ?: return emptyMap()
        val identity = NativeSaveSession.identity(context)
        val result = legacySyncUnits(root, remote)
        // Wire units are save-owner directories inside the approved profile, not game program IDs.
        // Library entries (including NUP-only games) are names, not authority to remap those units.
        NativeSaveSession.assertIdentity(context, identity)
        return result
    }
    /**
     * The game list, for names in the conflict prompt: the live one, or the copy the last scan saved (as Save transfer
     * reads it) when the prompt comes before this launch's scan finishes.
     */
    private fun games(context: Context): List<Game> = GameHelper.cachedGameList.takeIf { it.isNotEmpty() }
        ?: PreferenceManager.getDefaultSharedPreferences(context.applicationContext)
            .getStringSet(GameHelper.KEY_GAMES, emptySet()).orEmpty()
            .mapNotNull { runCatching { Json.decodeFromString<Game>(it) }.getOrNull() }

    fun get(context: Context): SaveSync {
        check(!org.yuzu.yuzu_emu.YuzuApplication.startupDeferred) { "Save sync is deferred until guarded application startup completes." }
        return instance ?: synchronized(this) {
            instance ?: SaveSync(
                context.applicationContext, "Duet Plus", 18385, 22101,
                listOf(
                    SyncConsole(
                        SWITCH, "Switch", 1,
                        units = { units(context.applicationContext, it) },
                        name = { unit -> games(context).find { it.programIdHex.equals(unit, true) }?.title },
                        preparePairing = { preparePairing(context.applicationContext) },
                        unpairedReason = {
                            if (approvedRoot(context.applicationContext) == null)
                                "This profile/root has no Switch peer mapping. Its account and device saves remain local-only."
                            else null
                        },
                        scopeDescription = "Switch sync shares only your approved profile. Other profiles and device save parts remain local-only.",
                        ownerConfirmation = { ownerConfirmation(context.applicationContext) }
                    )
                ),
                playing = { SWITCH.takeIf { runCatching { NativeLibrary.isRunning() }.getOrDefault(false) } },
                exitJob = DuetSaveSyncJob::class.java
            ).also {
                val prefs = preferences(context)
                if (prefs.contains("hub") && !prefs.contains("switch_owner_identity")) {
                    check(prefs.edit().putBoolean("switch_legacy_owner_unknown", true).commit()) {
                        "Cannot preserve the unknown legacy Switch peer owner."
                    }
                }
                instance = it
            }
        }
    }
}

/** Uploads a game's saves when it ends, retried until the PC has them (declared in the manifest). */
class DuetSaveSyncJob : SaveSyncJob() {
    override fun saveSync(context: Context) = DuetSaveSync.get(context)
}
