// SPDX-FileCopyrightText: Copyright 2026 tapionallanaire
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.sync

import android.content.Context
import java.io.File
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.utils.GameHelper
import org.yuzu.yuzu_emu.utils.NativeConfig

/**
 * Duet Plus syncs Switch saves only: the `switch/` part of the "calamansi-saves" folder that Calamansi (the main
 * client, every console) shares through the same PC. Its own ports, so both apps can run Syncthing on one device.
 */
object DuetSaveSync {
    const val SWITCH = "switch"

    @Volatile
    private var instance: SaveSync? = null

    fun get(context: Context): SaveSync = instance ?: synchronized(this) {
        instance ?: SaveSync(
            context.applicationContext, "Duet Plus", 18385, 22101,
            listOf(
                SyncConsole(
                    SWITCH, "Switch", 1,
                    units = {
                        // The profile games run as: <save dir>/user/save/0000000000000000/<user id>, the root relative
                        // to the save dir as in Save transfer.
                        val root = runCatching {
                            NativeLibrary.getDefaultProfileSaveDataRoot(false).takeIf { r -> r.isNotEmpty() }
                                ?.let { r -> File(NativeConfig.getSaveDir() + r) }
                        }.getOrNull()
                        savesUnder(root, 1)(it)
                    },
                    name = { unit -> GameHelper.cachedGameList.find { it.programIdHex.equals(unit, true) }?.title }
                )
            ),
            playing = { SWITCH.takeIf { runCatching { NativeLibrary.isRunning() }.getOrDefault(false) } }
        ).also { instance = it }
    }
}
