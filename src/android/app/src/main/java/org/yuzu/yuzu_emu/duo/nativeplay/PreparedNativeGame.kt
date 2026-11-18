// SPDX-FileCopyrightText: Copyright 2026 Duet Plus contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.nativeplay

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.os.Build
import java.lang.reflect.InvocationTargetException
import java.util.function.BiConsumer
import org.json.JSONObject

internal object PreparedNativeGame {
    data class Estimate(val text: String, val timeText: String, val requiredBytes: Long, val storagePath: String)

    fun available(engine: NativeEngine): Boolean = embeddedEngine(engine) != null

    private fun embeddedEngine(engine: NativeEngine = NativeEngine.PEANUTS): Class<*>? {
        if (Build.VERSION.SDK_INT < 33) return null
        return try {
            Class.forName(when (engine) {
                NativeEngine.PEANUTS -> "dev.nxnative.android.EmbeddedGame"
                NativeEngine.BANANA -> "dev.nxbanana.android.EmbeddedGame"
            })
        } catch (_: ClassNotFoundException) {
            null
        }
    }

    fun watchExits(activity: Activity, onFailure: (String, String) -> Unit) {
        for (kind in NativeEngine.entries) {
            val engine = embeddedEngine(kind) ?: continue
            try {
                engine.getMethod("watchExits", Activity::class.java, BiConsumer::class.java)
                    .invoke(null, activity, BiConsumer<String, String> { path, reason ->
                        onFailure(path, reason)
                    })
            } catch (e: ReflectiveOperationException) {
                org.yuzu.yuzu_emu.utils.Log.warning("Native exit watcher: " + e.message)
            }
        }
    }

    fun stopWatchingExits() {
        for (kind in NativeEngine.entries) {
            val engine = embeddedEngine(kind) ?: continue
            try {
                engine.getMethod("stopWatchingExits").invoke(null)
            } catch (e: ReflectiveOperationException) {
                org.yuzu.yuzu_emu.utils.Log.warning("Native exit watcher: " + e.message)
            }
        }
    }

    fun running(context: Context): Boolean = NativeEngine.entries.any { kind ->
        embeddedEngine(kind)?.getMethod("running", Context::class.java)
            ?.invoke(null, context) == true
    }

    fun launchState(context: Context, token: String): String {
        for (kind in NativeEngine.entries) {
            val engine = embeddedEngine(kind) ?: continue
            val state = engine.getMethod("launchState", Context::class.java, String::class.java)
                .invoke(null, context, token) as String
            if (state.isNotEmpty()) return state
        }
        return ""
    }

    /** Worker-only; installs the effective Duet driver without changing runtime preferences. */
    fun prepareDriver(context: Context, zip: java.io.File?, kind: NativeEngine = NativeEngine.PEANUTS): String {
        if (zip == null) return "system"
        val engine = checkNotNull(embeddedEngine(kind))
        return try {
            zip.inputStream().use { input ->
                engine.getMethod(
                    "prepareDriver", Context::class.java, java.io.InputStream::class.java,
                    String::class.java
                ).invoke(null, context, input, zip.nameWithoutExtension) as String
            }
        } catch (e: InvocationTargetException) {
            throw e.targetException
        }
    }

    /** Worker-only: validating a prepared pack reads its immutable executable snapshot. */
    fun prepare(context: Context, path: String, match: NativeGameSupport.Match): Intent? {
        val engine = embeddedEngine(match.profile.engine) ?: return null
        return try {
            engine.getMethod(
                "prepare", Context::class.java, String::class.java, String::class.java,
                Long::class.javaPrimitiveType, String::class.java
            ).invoke(
                null, context, path, match.profile.programId, match.profile.version,
                match.contentKey
            ) as Intent
        } catch (e: InvocationTargetException) {
            throw e.targetException
        }
    }

    fun compile(
        context: Context,
        path: String,
        inputPath: String,
        match: NativeGameSupport.Match
    ): Intent {
        val engine = checkNotNull(embeddedEngine(match.profile.engine))
        return try {
            engine.getMethod(
                "compile", Context::class.java, String::class.java, String::class.java,
                String::class.java, Long::class.javaPrimitiveType, String::class.java
            ).invoke(
                null, context, path, inputPath, match.profile.programId,
                match.profile.version, match.contentKey
            ) as Intent
        } catch (e: InvocationTargetException) {
            throw e.targetException
        }
    }

    fun beginInputExport(context: Context, path: String, inputPath: String, match: NativeGameSupport.Match): java.io.Closeable {
        val engine = checkNotNull(embeddedEngine(match.profile.engine))
        return try {
            engine.getMethod(
                "beginInputExport", Context::class.java, String::class.java, String::class.java,
                String::class.java, Long::class.javaPrimitiveType, String::class.java
            ).invoke(null, context, path, inputPath, match.profile.programId,
                match.profile.version, match.contentKey) as java.io.Closeable
        } catch (e: InvocationTargetException) { throw e.targetException }
    }
    fun selectStorage(context: Context, match: NativeGameSupport.Match, estimate: Estimate) {
        val engine = checkNotNull(embeddedEngine(match.profile.engine))
        try {
            engine.getMethod("selectStorage", Context::class.java, String::class.java)
                .invoke(null, context, estimate.storagePath)
        } catch (e: InvocationTargetException) {
            throw e.targetException
        }
    }


    fun estimate(context: Context, match: NativeGameSupport.Match): Estimate {
        val engine = checkNotNull(embeddedEngine(match.profile.engine))
        val json = try {
            engine.getMethod(
                "estimate", Context::class.java, String::class.java, Long::class.javaPrimitiveType
            ).invoke(null, context, match.profile.programId, match.inputBytes) as String
        } catch (e: InvocationTargetException) {
            throw e.targetException
        }
        val values = JSONObject(json)
        val text = values.getString("text")
        val requiredBytes = values.getLong("required_bytes")
        val storage = values.getString("storage_path")
        require(text.isNotBlank() && requiredBytes >= match.inputBytes && storage.isNotBlank())
        return Estimate(text, values.getString("time_text"), requiredBytes, storage)
    }

    fun recoverSharedSaves(kind: NativeEngine, accountPath: String?, devicePath: String?) {
        val engine = checkNotNull(embeddedEngine(kind))
        try {
            engine.getMethod("recoverSharedSaves", String::class.java, String::class.java)
                .invoke(null, accountPath, devicePath)
        } catch (e: InvocationTargetException) { throw e.targetException }
    }

    fun openPreparation(context: Context, request: Intent, jobPath: String?, kind: NativeEngine): String {
        val engine = checkNotNull(embeddedEngine(kind))
        return try {
            engine.getMethod("openPreparation", Context::class.java, Intent::class.java, String::class.java)
                .invoke(null, context, request, jobPath) as String
        } catch (e: InvocationTargetException) { throw e.targetException }
    }

    private fun preparationCall(method: String, context: Context, jobPath: String, kind: NativeEngine): Any? {
        val engine = checkNotNull(embeddedEngine(kind))
        return try {
            engine.getMethod(method, Context::class.java, String::class.java)
                .invoke(null, context, jobPath)
        } catch (e: InvocationTargetException) { throw e.targetException }
    }

    fun preparationStatus(context: Context, path: String, kind: NativeEngine) =
        JSONObject(preparationCall("preparationStatus", context, path, kind) as String)

    fun startPreparation(context: Context, path: String, kind: NativeEngine) {
        preparationCall("startPreparation", context, path, kind)
    }

    fun cancelPreparation(context: Context, path: String, kind: NativeEngine) {
        preparationCall("cancelPreparation", context, path, kind)
    }

    fun preparationResult(context: Context, path: String, kind: NativeEngine): Intent? =
        preparationCall("preparationResult", context, path, kind) as Intent?

    fun compiledData(context: Context, path: String, titleId: String, kind: NativeEngine): JSONObject {
        val engine = embeddedEngine(kind)
            ?: return JSONObject().put("present", false).put("bytes", 0L).put("input_bytes", 0L).put("busy", false)
        return try {
            JSONObject(engine.getMethod("compiledData", Context::class.java, String::class.java, String::class.java)
                .invoke(null, context, path, titleId) as String)
        } catch (e: InvocationTargetException) { throw e.targetException }
    }

    fun deleteCompiledData(context: Context, path: String, titleId: String, kind: NativeEngine) {
        val engine = checkNotNull(embeddedEngine(kind))
        try {
            engine.getMethod("deleteCompiledData", Context::class.java, String::class.java, String::class.java)
                .invoke(null, context, path, titleId)
        } catch (e: InvocationTargetException) { throw e.targetException }
    }
}
