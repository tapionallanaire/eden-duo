// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.utils

import android.content.Context
import java.io.File
import org.yuzu.yuzu_emu.model.Game

/**
 * Eden Duo: warm-up runs.
 *
 * A game's shader cache only grows while the game runs, so "generate them upfront" means running a
 * title on purpose: it boots, plays its own intro or attract loop, and every shader it draws is
 * compiled and cached. The run is time-boxed and its saves are put back afterwards, so a warm-up
 * cannot cost the player progress.
 *
 * The request travels in the launch intent, which is also what makes it visible to the emulation
 * thread (see EmulationFragment).
 */
object WarmUp {
    const val EXTRA_MINUTES = "org.yuzu.yuzu_emu.WARMUP_MINUTES"
    const val EXTRA_BACKUP = "org.yuzu.yuzu_emu.WARMUP_BACKUP"
    const val EXTRA_SHADERS_BEFORE = "org.yuzu.yuzu_emu.WARMUP_SHADERS_BEFORE"

    /** Offered lengths, in minutes. Long enough to reach gameplay from a title screen. */
    val DURATIONS = intArrayOf(5, 10, 15)

    /** Where Eden keeps a title's compiled shaders on Android. */
    fun shaderDir(game: Game): File = File(
        DirectoryInitialization.userDirectory + "/cache/shader/" + game.settingsName.lowercase()
    )

    fun shaderCount(game: Game): Int =
        shaderDir(game).walkTopDown().count { it.isFile }

    private fun backups(context: Context): File = File(context.filesDir, "warmup")

    /**
     * Zips the title's save folder before a warm-up run. Returns null when the game has no save
     * yet, which is the same as nothing to protect.
     */
    fun backupSaves(context: Context, game: Game): File? {
        val saveDir = File(game.saveDir)
        if (!SaveArchive.hasData(saveDir)) return null
        val file = File(backups(context), "${game.programId.uppercase()}-${System.currentTimeMillis()}.zip")
        file.parentFile?.mkdirs()
        file.writeBytes(SaveArchive.zip(saveDir))
        Log.info("[WarmUp] saved ${game.programId} to ${file.path}")
        return file
    }

    /** Puts the backed-up save back. Returns true when something was restored. */
    fun restoreSaves(context: Context, game: Game, backup: File?): Boolean {
        if (backup == null || !backup.isFile) return false
        SaveArchive.extract(backup.readBytes(), File(game.saveDir))
        backup.delete()
        Log.info("[WarmUp] restored ${game.programId} from ${backup.path}")
        return true
    }
}
