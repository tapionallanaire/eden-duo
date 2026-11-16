// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.utils

import java.io.File
import org.yuzu.yuzu_emu.model.Game

/**
 * Eden Duo: shader runs.
 *
 * Two jobs that need the game running, unlike a plain boot:
 *
 * - **Record**: play with it on and every graphics pipeline the game builds for the first time is
 *   written to the title's `recording.bin`, a plan file with a line per pipeline in `recording.log`.
 *   That is how effects only play reveals become something a later compile can warm.
 * - **Compile max**: a compile run that also builds every vertex/fragment shader combination the
 *   title's plan implies, not only the pairs it has drawn, so more of what the game will ask for
 *   later is already compiled.
 *
 * Both travel in the launch intent; the emulation thread reads them before `run()` (see
 * EmulationFragment).
 */
object ShaderRuns {
    /** Launch extra: keep this session's newly built pipelines as the title's recording. */
    const val EXTRA_RECORD = "org.yuzu.yuzu_emu.SHADER_RECORD"

    /** Launch extra: this run only compiles the title's shaders, it does not play the game. */
    const val EXTRA_COMPILE = "org.yuzu.yuzu_emu.SHADER_COMPILE"

    /** Launch extra: the compile builds shader combinations too (Compile max). */
    const val EXTRA_MAX = "org.yuzu.yuzu_emu.SHADER_COMPILE_MAX"

    /**
     * True while a compile run is on. The shader cache loader watches it to stop the game once the
     * plan has been compiled (DiskShaderCacheProgress).
     */
    @Volatile
    var compilingNow = false

    /** True while a recording run is on, for the UI. */
    @Volatile
    var recordingNow = false

    /** The shader cache loader's last report: a compile run's final built/total. */
    @Volatile
    var lastBuilt = 0

    @Volatile
    var lastTotal = 0

    fun recordingLog(game: Game): File =
        File(cacheDir(game), "recording.log")

    fun recordingPlan(game: Game): File =
        File(cacheDir(game), "recording.bin")

    /** Pipelines the last recording captured, from the one line the renderer writes per pipeline. */
    fun recordingCount(game: Game): Int {
        val file = recordingLog(game)
        if (!file.isFile) return 0
        return try {
            file.useLines { lines -> lines.count { it.isNotBlank() } }
        } catch (e: Exception) {
            Log.warning("[ShaderRuns] unreadable ${file.path}: $e")
            0
        }
    }

    private fun cacheDir(game: Game): File = File(
        DirectoryInitialization.userDirectory + "/cache/shader/" + game.settingsName.lowercase()
    )
}
