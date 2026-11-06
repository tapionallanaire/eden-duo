// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.utils

import android.content.Context
import java.io.File

/**
 * The GPU driver a title's optimal settings pick, by title ID. Apply optimal settings writes it as the game's
 * own driver, fetching it first (DriverResolver's repositories) when this device doesn't have it.
 */
object OptimalDriver {
    private const val TURNIP_V27_R1 = "Turnip_v27.0.0-R1.zip" // StevenMXZ/Adreno-Tools-Drivers v27.0.0-R1

    private val byTitle = mapOf(
        "0100E95004038000" to TURNIP_V27_R1, // Xenoblade Chronicles 2
        "010074F013262000" to TURNIP_V27_R1, // Xenoblade Chronicles 3
    )

    fun forTitle(programIdHex: String): String? = byTitle[programIdHex.uppercase()]

    /** The installed driver zip's path for [fileName], downloaded first if missing; null when that failed. */
    suspend fun ensure(fileName: String, context: Context): String? =
        DriverResolver.ensureDriverAvailable(fileName, context)?.path

    /** Drops the game's shader cache, as the driver pickers do when a game's driver changes. */
    fun wipeShaders(settingsName: String) {
        val user = DirectoryInitialization.userDirectory ?: return
        if (settingsName.isBlank() || '/' in settingsName) return // a blank name is the whole cache
        File(user, "cache/shader/" + settingsName.lowercase()).deleteRecursively()
    }
}
