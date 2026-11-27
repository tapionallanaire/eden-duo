// SPDX-FileCopyrightText: Copyright 2026 Duet Plus contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.nativeplay

import android.content.Context

internal object NativePlayPreferences {
    enum class Mode { ASK, EMULATED, NATIVE }

    private const val EMULATED_RESET = "emulated_choice_reset"

    fun mode(context: Context, titleId: String): Mode = runCatching {
        val prefs = context.getSharedPreferences("native_play", Context.MODE_PRIVATE)
        // Once: before 1.4.13 one "Play emulated" tap was saved as EMULATED and hid the choice for
        // good. Those can't be told from a Native settings choice, so every EMULATED goes back to Ask.
        if (!prefs.getBoolean(EMULATED_RESET, false)) {
            prefs.edit().apply {
                prefs.all.filter { it.value == Mode.EMULATED.name }.keys.forEach(::remove)
                putBoolean(EMULATED_RESET, true)
            }.commit()
        }
        Mode.valueOf(prefs.getString(titleId.uppercase(), Mode.ASK.name)!!)
    }.getOrDefault(Mode.ASK)

    fun setMode(context: Context, titleId: String, mode: Mode) {
        context.getSharedPreferences("native_play", Context.MODE_PRIVATE).edit()
            .putString(titleId.uppercase(), mode.name).apply()
    }

    fun rememberProfile(context: Context, titleId: String, profile: NativeGameProfile) {
        context.getSharedPreferences("native_play", Context.MODE_PRIVATE).edit()
            .putString("profile:${titleId.uppercase()}", "${profile.engine.name}:${profile.programId}").apply()
    }

    fun lastProfile(context: Context, titleId: String): Pair<NativeEngine, String>? = runCatching {
        val value = context.getSharedPreferences("native_play", Context.MODE_PRIVATE)
            .getString("profile:${titleId.uppercase()}", null) ?: return null
        val fields = value.split(':')
        require(fields.size == 2 && Regex("[0-9A-F]{16}").matches(fields[1]))
        NativeEngine.valueOf(fields[0]) to fields[1]
    }.getOrNull()
}
