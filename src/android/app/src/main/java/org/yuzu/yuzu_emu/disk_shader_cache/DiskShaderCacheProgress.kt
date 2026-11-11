// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.yuzu.yuzu_emu.disk_shader_cache

import androidx.annotation.Keep
import androidx.lifecycle.ViewModelProvider
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.activities.EmulationActivity
import org.yuzu.yuzu_emu.model.EmulationViewModel
import org.yuzu.yuzu_emu.utils.Log

@Keep
object DiskShaderCacheProgress {
    private lateinit var emulationViewModel: EmulationViewModel

    private fun prepareViewModel() {
        emulationViewModel =
            ViewModelProvider(
                NativeLibrary.sEmulationActivity.get() as EmulationActivity
            )[EmulationViewModel::class.java]
    }

    @JvmStatic
    fun loadProgress(stage: Int, progress: Int, max: Int) {
        val emulationActivity = NativeLibrary.sEmulationActivity.get()
        if (emulationActivity == null) {
            Log.error("[DiskShaderCacheProgress] EmulationActivity not present")
            return
        }

        emulationActivity.runOnUiThread {
            when (LoadCallbackStage.values()[stage]) {
                LoadCallbackStage.Prepare -> prepareViewModel()
                LoadCallbackStage.Build -> {
                    // Eden Duo: the last report a compile run gives is its final built/total, which
                    // is what its toast says (see EmulationFragment).
                    org.yuzu.yuzu_emu.utils.ShaderRuns.lastBuilt = progress
                    org.yuzu.yuzu_emu.utils.ShaderRuns.lastTotal = max
                    emulationViewModel.updateProgress(
                        emulationActivity.getString(R.string.building_shaders),
                        progress,
                        max
                    )
                }

                LoadCallbackStage.Complete -> {
                    // Eden Duo: a compile run is over once the plan (and, with Compile max, the
                    // shader combinations) is compiled and the driver cache is written; stop the
                    // game, which has only shown its loading frames, through the normal stop path.
                    if (org.yuzu.yuzu_emu.utils.ShaderRuns.compilingNow) {
                        Log.info("[ShaderRuns] compile finished, stopping the run")
                        android.os.Handler(android.os.Looper.getMainLooper()).postDelayed(
                            { NativeLibrary.stopEmulation() },
                            COMPILE_STOP_DELAY_MS
                        )
                    }
                }
            }
        }
    }

    /**
     * Eden Duo: how long a compile run lets the game live after the plan is compiled. A few
     * presented frames settle the GPU thread and the second screen, which their teardown expects.
     */
    private const val COMPILE_STOP_DELAY_MS = 1200L

    // Equivalent to VideoCore::LoadCallbackStage
    enum class LoadCallbackStage {
        Prepare, Build, Complete
    }
}
