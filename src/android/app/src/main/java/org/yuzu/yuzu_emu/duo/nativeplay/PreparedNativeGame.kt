// SPDX-FileCopyrightText: Copyright 2026 Duet Plus contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.nativeplay

import android.content.Context
import android.content.Intent
import android.os.Build
import java.lang.reflect.InvocationTargetException

internal object PreparedNativeGame {
    /**
     * Checks the selected program and returns the internal activity for a verified Peanuts pack.
     * Run on a worker because validating a pack reads the original executable.
     */
    fun prepare(context: Context, path: String, programIndex: Int): Intent? {
        if (Build.VERSION.SDK_INT < 33) return null
        val engine = try {
            Class.forName("dev.nxnative.android.EmbeddedGame")
        } catch (_: ClassNotFoundException) {
            return null
        }

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
