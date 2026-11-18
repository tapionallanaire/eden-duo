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

    /**
     * Set by the launch dialog when the player answers yes to "compile shaders first": this boot
     * finds the game's shaders in its files and builds them on the loading screen.
     */
    @Volatile
    var precacheNext = false

    /**
     * True from the start of a "compile shaders first" boot until the shader load reports it is
     * complete. The game counts as started before its shaders load, which would hide the loading
     * card and leave a black screen for the minutes the compile takes.
     */
    @Volatile
    var precacheBoot = false

    /** Removes the "Preparing shaders" screen once such a boot has finished (set by EmulationFragment). */
    @Volatile
    var onPrecacheBootDone: (() -> Unit)? = null

    /** Built / total of such a boot, for the "Preparing shaders" screen (set by EmulationFragment). */
    @Volatile
    var onPrecacheProgress: ((Int, Int) -> Unit)? = null

    /**
     * Switch 2 titles (program IDs 04...) always have programs to compile; a Switch 1 title only
     * with a pipeline list or a shader list, its own or a shipped one.
     */
    fun canPrecache(context: android.content.Context, game: Game): Boolean {
        if (game.programIdHex.startsWith("04")) return true
        val dir = WarmUp.shaderDir(game)
        val id = game.programIdHex.uppercase()
        return File(dir, "plan.bin").exists() || File(dir, "peanuts.tsv.zst").exists() ||
            listOf("shader_plans/$id.bin", "shader_lists/peanuts/$id.tsv.zst").any { asset ->
                try {
                    context.assets.open(asset).close()
                    true
                } catch (_: java.io.IOException) {
                    false
                }
            }
    }

    /** The scan's "done" marker: the boot-time precache already ran for this title on this driver. */
    fun precacheDone(game: Game): Boolean {
        val dir = WarmUp.shaderDir(game)
        // A skipped compile leaves its pair file without the marker: ask again next time.
        return File(dir, "warm-compute.done").exists() &&
            (!File(dir, "warm-pairs.bin").exists() || File(dir, "warm-pairs.done").exists())
    }

    /**
     * Copies the pipeline list shipped for this title (no game code in it) next to its shader
     * cache, where the boot fills it from the game's own files. Without one, the title's shader
     * list from Peanuts Native (shader keys only), when there is one.
     */
    fun installShippedPlan(context: android.content.Context, game: Game) {
        val dir = WarmUp.shaderDir(game)
        val id = game.programIdHex.uppercase()
        if (File(dir, "plan.bin").exists()) return
        if (copyAsset(context, "shader_plans/$id.bin", File(dir, "plan.bin"))) return
        val list = File(dir, "peanuts.tsv.zst")
        if (!list.exists()) copyAsset(context, "shader_lists/peanuts/$id.tsv.zst", list)
    }

    /** False when the app ships no such asset. */
    private fun copyAsset(context: android.content.Context, asset: String, target: File): Boolean =
        try {
            context.assets.open(asset).use { input ->
                target.parentFile?.mkdirs()
                target.outputStream().use { input.copyTo(it) }
            }
            true
        } catch (_: java.io.IOException) {
            false
        }

    /**
     * Asks once per title whether to compile its shaders before it starts, then calls [launch].
     * Dismissing the question calls [cancel] instead, so its owner can leave a pending launch.
     */
    fun askThenLaunch(context: android.content.Context, game: Game?, cancel: () -> Unit, launch: () -> Unit) {
        val preferences = androidx.preference.PreferenceManager.getDefaultSharedPreferences(context)
        val declinedKey = "shader_precache_declined_" + (game?.programIdHex ?: "")
        if (game == null || !canPrecache(context, game) || precacheDone(game) ||
            preferences.getBoolean(declinedKey, false)
        ) {
            launch()
            return
        }
        com.google.android.material.dialog.MaterialAlertDialogBuilder(context)
            .setTitle(org.yuzu.yuzu_emu.R.string.shader_precache_title)
            .setMessage(org.yuzu.yuzu_emu.R.string.shader_precache_message)
            .setPositiveButton(org.yuzu.yuzu_emu.R.string.shader_precache_yes) { _, _ ->
                installShippedPlan(context, game)
                precacheNext = true
                launch()
            }
            .setNegativeButton(org.yuzu.yuzu_emu.R.string.shader_precache_no) { _, _ ->
                preferences.edit().putBoolean(declinedKey, true).apply()
                launch()
            }
            .setNeutralButton(android.R.string.cancel) { _, _ -> cancel() }
            .setOnCancelListener { cancel() }
            .show()
    }

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
