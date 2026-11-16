// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.utils

import android.app.Activity
import android.app.ActivityManager
import android.content.Context
import android.widget.Toast
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R

/**
 * Eden Duo: memory pressure guard.
 *
 * Android kills the emulation process outright when memory runs out (LOW_MEMORY), which looks to
 * the player like a crash. This warns before that happens and can hand back what the renderer is
 * holding: [recover] collects the texture and buffer caches on the GPU thread and returns the heap
 * to the OS, which for a running game is usually a few hundred megabytes.
 */
object MemoryGuard {
    /** Warn below this much free device memory. An 8 GB handheld runs a 4 GB guest plus caches. */
    private const val WARN_BELOW_BYTES = 900L * 1024L * 1024L

    /** Do not re-warn inside this window; the dialog is a nudge, not a nag. */
    private const val WARN_COOLDOWN_MS = 5L * 60L * 1000L

    private var lastWarnMs = 0L

    fun memoryInfo(context: Context): ActivityManager.MemoryInfo? {
        val manager = context.getSystemService(Context.ACTIVITY_SERVICE) as? ActivityManager
            ?: return null
        return ActivityManager.MemoryInfo().also { manager.getMemoryInfo(it) }
    }

    fun availableBytes(context: Context): Long =
        memoryInfo(context)?.availMem ?: Long.MAX_VALUE

    /** True when Android itself says memory is low, or free memory is under our margin. */
    fun isLow(context: Context): Boolean {
        val info = memoryInfo(context) ?: return false
        return info.lowMemory || info.availMem < WARN_BELOW_BYTES
    }

    fun describe(context: Context): String {
        val free = availableBytes(context)
        return if (free == Long.MAX_VALUE) "unknown" else "${free / (1024L * 1024L)} MB free"
    }

    /**
     * Collect the renderer's caches and give the heap back. Returns the free memory gained, in
     * bytes, as the system reports it; 0 when nothing was gained or no game runs.
     */
    fun recover(context: Context): Long {
        val before = availableBytes(context)
        NativeLibrary.recoverMemory()
        Runtime.getRuntime().gc()
        val after = availableBytes(context)
        val gained = after - before
        Log.info("MemoryGuard: recovered ${gained / (1024L * 1024L)} MB (${describe(context)})")
        return if (gained > 0L) gained else 0L
    }

    fun recoverWithToast(context: Context) {
        val gained = recover(context)
        val text = if (gained >= 1024L * 1024L) {
            context.getString(R.string.memory_recovered, gained / (1024L * 1024L))
        } else {
            context.getString(R.string.memory_already_clean, describe(context))
        }
        Toast.makeText(context, text, Toast.LENGTH_LONG).show()
    }

    /**
     * Warn the player, throttled. Offers to free memory right away; whatever they answer, the
     * caches are collected, because that is the cheap half of the remedy.
     */
    fun warnIfLow(
        activity: Activity,
        force: Boolean = false,
        onAfterDialog: (() -> Unit)? = null
    ) {
        if (activity.isFinishing) return
        if (!isLow(activity)) {
            onAfterDialog?.invoke()
            return
        }
        val now = System.currentTimeMillis()
        if (!force && now - lastWarnMs < WARN_COOLDOWN_MS) {
            onAfterDialog?.invoke()
            return
        }
        lastWarnMs = now

        MaterialAlertDialogBuilder(activity)
            .setTitle(R.string.memory_low_title)
            .setMessage(activity.getString(R.string.memory_low_message, describe(activity)))
            .setPositiveButton(R.string.memory_free_now) { _, _ ->
                recoverWithToast(activity)
                onAfterDialog?.invoke()
            }
            .setNeutralButton(R.string.memory_close_game) { _, _ ->
                activity.finish()
            }
            .setNegativeButton(android.R.string.cancel) { _, _ -> onAfterDialog?.invoke() }
            .setOnCancelListener { onAfterDialog?.invoke() }
            .show()
    }
}
