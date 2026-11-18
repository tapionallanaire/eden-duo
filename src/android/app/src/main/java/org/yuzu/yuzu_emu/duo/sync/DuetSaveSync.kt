// SPDX-FileCopyrightText: Copyright 2026 tapionallanaire
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.sync

import android.content.Context
import androidx.preference.PreferenceManager
import java.io.File
import kotlinx.serialization.json.Json
import org.json.JSONObject
import org.yuzu.yuzu_emu.duo.nativeplay.NativeSaveSession
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.utils.GameHelper
import org.yuzu.yuzu_emu.utils.PathUtil

/** Old SQLite hash stamps contain no local path: an unknown old owner cannot safely be inferred. */
internal fun approvedLegacySyncRoot(
    approvedIdentity: String?,
    approvedRoot: String?,
    identity: String,
    root: File,
    legacyPaired: Boolean
): File? {
    if (legacyPaired && (approvedIdentity == null || approvedRoot == null)) throw UnpairedSyncException(
        "Legacy Switch sync has no recorded original profile. Open Save sync settings to explicitly verify its original account; saves stay local until then."
    )
    return root.canonicalFile.takeIf { approvedIdentity == identity && approvedRoot == it.path }
}

internal fun legacySyncUnit(root: File, unit: String): File {
    check(unit.matches(Regex("[0-9A-Fa-f]{16}"))) { "Invalid Switch save unit: $unit" }
    val file = File(root, unit).canonicalFile
    check(file.parentFile == root.canonicalFile) { "Switch save unit leaves its approved profile root." }
    return file
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
        // Never let Disconnect/restart turn an unverifiable old slot into approval for the current user.
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
        val user = checkNotNull(NativeLibrary.getCurrentUser()) { "Select a real account before verifying legacy sync." }
        check(user.isNotEmpty()) { "Select a real account before verifying legacy sync." }
        val name = NativeLibrary.getUserUsername(user).orEmpty().ifBlank { user }
        return SyncOwnerConfirmation(
            "This legacy peer uses title-only Switch slots and did not record its original account.\n\n" +
                "Selected account: $name\nAccount ID: $user\nAccount save root: ${root.path}\n" +
                "Hub: ${hub ?: "previously paired peer (currently disconnected)"}\n" +
                "Peer scope: switch/<save-owner ID>, account saves only.\n\n" +
                "Confirm only if this is the ORIGINAL account previously paired with this hub. " +
                "Do not use this action to switch the peer to another profile. Other profiles and device saves stay local-only.\n\n" +
                "Confirmation records ownership only. It does not copy, seed, restore or merge any saves."
        ) {
            NativeSaveSession.assertIdentity(context, identity)
            check(prefs.getString("hub", null) == hub) { "The paired hub changed. Open Save sync again before confirming." }
            confirmLegacySyncOwner(prefs.getString("switch_owner_identity", null),
                prefs.getString("switch_owner_root", null), identity, root,
                NativeSaveSession.identity(context), selectedRoot())
            check(prefs.edit().putString("switch_owner_identity", identity)
                .putString("switch_owner_root", root.path).remove("switch_legacy_owner_unknown").commit()) {
                "Cannot persist the explicitly confirmed original Switch account."
            }
        }
    }

    private fun units(context: Context, remote: List<String>): Map<String, File> {
        val root = approvedRoot(context) ?: return emptyMap()
        val identity = NativeSaveSession.identity(context)
        val result = (listUnits(root, 1) + remote).distinct().associateWith { legacySyncUnit(root, it) }.toMutableMap()
        // The existing wire unit is the save owner directory, not a new program/profile/device protocol.
        // Resolve retail metadata when present; never use a generated NUP as authority for save ownership.
        for (game in games(context).filter { it.programIdHex in result.keys && !it.isHomebrew }) {
            check(!game.path.substringBefore('?').endsWith(".nup", true)) {
                "Retail metadata is unavailable for ${game.title}; its Switch peer mapping cannot be verified."
            }
            val binding = GameHelper.withPrepareContent(game.path, { game.programId }) {
                JSONObject(NativeLibrary.getNativeSaveBinding(game.path, game.programIdHex.takeLast(3).toInt(16)))
            }
            NativeSaveSession.assertIdentity(context, identity)
            val owner = binding.getString("save_owner_id")
            check(binding.getInt("format") == 1 && binding.getString("program_id").equals(game.programIdHex, true)) {
                "Switch save metadata does not match ${game.title}."
            }
            if (binding.isNull("account_path")) {
                result.remove(game.programIdHex)
                continue
            }
            val account = File(binding.getString("account_path")).canonicalFile
            check(account == legacySyncUnit(root, owner)) { "Canonical account save is outside its approved peer mapping." }
            if (!owner.equals(game.programIdHex, true)) {
                check(game.programIdHex !in remote) {
                    "The legacy peer slot for ${game.title} differs from its canonical save owner; explicit peer migration is required."
                }
                result.remove(game.programIdHex)
            }
            result[owner] = account
        }
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
                        scopeDescription = "Switch sync shares only the originally paired account. Other profiles and device save parts remain local-only.",
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
