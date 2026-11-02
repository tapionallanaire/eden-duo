// SPDX-FileCopyrightText: Copyright 2026 tapionallanaire
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.sync

import android.content.Context
import java.io.File
import org.yuzu.yuzu_emu.NativeLibrary

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
                SyncConsole(SWITCH, "Switch", 1) {
                    // The profile games run as: nand/user/save/0000000000000000/<user id>.
                    val root = runCatching { NativeLibrary.getDefaultProfileSaveDataRoot(false) }.getOrNull()
                    savesUnder(root?.takeIf { it.isNotEmpty() }?.let(::File), 1)(it)
                }
            ),
            playing = { SWITCH.takeIf { runCatching { NativeLibrary.isRunning() }.getOrDefault(false) } }
        ).also { instance = it }
    }
}
