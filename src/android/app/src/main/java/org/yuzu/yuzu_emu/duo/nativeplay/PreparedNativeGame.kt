// SPDX-FileCopyrightText: Copyright 2026 Duet Plus contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.nativeplay

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.os.Build
import java.lang.reflect.InvocationTargetException
import java.util.function.BiConsumer

internal object PreparedNativeGame {
    private fun embeddedEngine(): Class<*>? {
        if (Build.VERSION.SDK_INT < 33) return null
        return try {
            Class.forName("dev.nxnative.android.EmbeddedGame")
        } catch (_: ClassNotFoundException) {
            null
        }
    }

    fun watchExits(activity: Activity, onFailure: (String, String) -> Unit) {
        val engine = embeddedEngine() ?: return
        try {
            engine.getMethod("watchExits", Activity::class.java, BiConsumer::class.java)
                .invoke(null, activity, BiConsumer<String, String> { path, reason ->
                    onFailure(path, reason)
                })
        } catch (e: ReflectiveOperationException) {
            org.yuzu.yuzu_emu.utils.Log.warning("Native exit watcher: " + e.message)
        }
    }

    fun stopWatchingExits() {
        val engine = embeddedEngine() ?: return
        try {
            engine.getMethod("stopWatchingExits").invoke(null)
        } catch (e: ReflectiveOperationException) {
            org.yuzu.yuzu_emu.utils.Log.warning("Native exit watcher: " + e.message)
        }
    }

    /**
     * Checks the selected program and returns the internal activity for a verified Peanuts pack.
     * Run on a worker because validating a pack reads the original executable.
     */
    fun prepare(context: Context, path: String, programIndex: Int): Intent? {
        val engine = embeddedEngine() ?: return null

        val profile = NativeGameSupport.findBuildCandidate(context, path, programIndex)?.profile
            ?: return null
        if (profile.engine != NativeEngine.PEANUTS) return null
        val prepare = engine.getMethod(
            "prepare", Context::class.java, String::class.java, String::class.java,
            Long::class.javaPrimitiveType
        )
        return try {
            prepare.invoke(null, context, path, profile.programId, profile.version) as Intent
        } catch (e: InvocationTargetException) {
            throw e.targetException
        }
    }
}
