// SPDX-License-Identifier: GPL-3.0-or-later
package org.yuzu.yuzu_emu.duo.nativeplay

import android.content.Context
import org.json.JSONObject

/** Worker-only. A title can have multiple versions, private export generations and original paths. */
internal object NativeStorage {
    data class Game(val titleId: String, val name: String, val bytes: Long, val error: String? = null)
    data class Result(val removed: Int, val busy: Int, val failed: Int)

    private fun rows(context: Context, titleId: String?, purge: Boolean) = buildList<JSONObject> {
        for (engine in NativeEngine.entries) {
            val rows = PreparedNativeGame.managedStorage(context, titleId, purge, engine)
            for (i in 0 until rows.length()) add(rows.getJSONObject(i))
        }
    }.groupBy { it.getString("title_id") }

    fun inventory(context: Context): List<Game> {
        val catalog = context.assets.open(NativeGameCatalog.ASSET).reader().use(NativeGameCatalog::read)
        return rows(context, null, false).map { (id, entries) ->
            val error = entries.firstOrNull { it.has("error") }?.getString("error")
            Game(id, catalog.versions(id).firstOrNull()?.title ?: id,
                entries.fold(0L) { bytes, entry -> Math.addExact(bytes, entry.optLong("bytes", 0L)) }, error)
        }.sortedWith(compareByDescending<Game> { it.bytes }.thenBy { it.name }.thenBy { it.titleId })
    }

    fun purge(context: Context, titleId: String?): Result {
        fun collect(mayDelete: Boolean): Result {
            var removed = 0
            var busy = 0
            var failed = 0
            for (entries in rows(context, titleId, mayDelete).values) {
                when {
                    entries.any { it.has("error") } -> failed++
                    !mayDelete || entries.any { it.getBoolean("busy") } -> busy++
                    else -> removed++
                }
            }
            return Result(removed, busy, failed)
        }

        // Do not queue a destructive request behind an exporter or game. Report what was skipped.
        val export = NativePreparationActivity.exportMutex
        if (!export.tryLock()) return collect(false)
        try {
            val launch = try { NativeSaveSession.acquireStorageMaintenance(context) }
            catch (_: NativeSaveSession.BusyException) { return collect(false) }
            return launch.use { collect(true) }
        } finally { export.unlock() }
    }
}
