// SPDX-FileCopyrightText: Copyright 2026 Duet Plus contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.nativeplay

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import androidx.annotation.Keep
import androidx.annotation.WorkerThread
import java.io.File
import java.io.IOException
import java.util.UUID
import org.yuzu.yuzu_emu.utils.GameHelper

@Keep
internal fun interface NativePrepareProgress {
    /** False cancels the native bounded-read loop; interruption alone is not cancellation. */
    fun update(completed: Long, total: Long): Boolean
}

internal object NativeGameSupport {
    data class Match(
        val profile: NativeGameProfile,
        val supportedVersions: List<NativeGameProfile>,
        val build: NativeGameBuild,
        val contentKey: String,
        val inputBytes: Long
    ) {
        val programIndex: Int get() = build.programId.takeLast(3).toInt(16)
    }

    data class Identity(val build: NativeGameBuild, val contentKey: String, val inputBytes: Long)

    /** Resolve only program indices represented by shipped profiles; never relabel program zero. */
    fun findGameCandidate(
        context: Context,
        path: String,
        onFailure: ((Throwable) -> Unit)? = null
    ): Match? = runCatching {
        val catalog = context.assets.open(NativeGameCatalog.ASSET).reader().use(NativeGameCatalog::read)
        catalog.programIndices.firstNotNullOfOrNull { findBuildCandidate(context, path, it, onFailure) }
    }.onFailure { onFailure?.invoke(it) }.getOrNull()

    /** Registers and resolves the same content as boot, without starting emulation. */
    fun findBuildCandidate(
        context: Context,
        path: String,
        programIndex: Int,
        onFailure: ((Throwable) -> Unit)? = null
    ): Match? {
        var identified = false
        return runCatching {
            GameHelper.withPrepareContent(path, {
                readPrepareProgramId(path, programIndex)?.also { identified = true }
                    ?: error("Cannot identify selected program")
            }) {
                val identity = readGameBuild(path, programIndex)?.let(::parseIdentity)
                    ?: return@withPrepareContent null
                val catalog = context.assets.open(NativeGameCatalog.ASSET).reader()
                    .use(NativeGameCatalog::read)
                val profile = catalog.match(identity.build) ?: return@withPrepareContent null
                Match(profile, catalog.versions(profile.programId), identity.build,
                    identity.contentKey, identity.inputBytes)
            }
        }.onFailure { if (identified) onFailure?.invoke(it) }.getOrNull()
    }

    /** Reuse only a fully validated snapshot; interrupted generations are never overwritten. */
    @WorkerThread
    fun selectPrepareInput(
        context: Context,
        path: String,
        programIndex: Int,
        expected: Match,
        directory: File,
        preferred: String?
    ): File {
        val root = ownedPrepareOutput(File(directory, "game.nup").path, prepareRoots(context))
            .parentFile ?: throw IOException("Cannot locate Prepare input directory")
        val fields = identityFields(expected)
        return GameHelper.withPrepareContent(path, { expected.build.programId.toULong(16).toString() }) {
            selectInputGeneration(root, preferred?.let(::File)) { input ->
                isPrepareInputCompleteNative(path, programIndex, fields, input.path)
            }
        }
    }

    internal fun selectInputGeneration(
        directory: File,
        preferred: File?,
        reusable: (File) -> Boolean
    ): File {
        val root = directory.canonicalFile
        if (!root.isDirectory && !root.mkdirs()) {
            throw IOException("Cannot create Prepare input directory")
        }
        fun isCandidate(input: File): Boolean {
            val parent = input.parentFile ?: return false
            return input.name == "game.nup" &&
                (parent == root ||
                    (parent.parentFile == root && generationName.matches(parent.name))) &&
                runCatching { input.canonicalFile == input }.getOrDefault(false)
        }
        val saved = preferred?.absoluteFile?.takeIf(::isCandidate)
        val children = root.listFiles() ?: throw IOException("Cannot read Prepare input directory")
        val candidates = sequenceOf(saved, File(root, "game.nup")) +
            children.asSequence().filter { generationName.matches(it.name) }
                .map { File(it, "game.nup") }
        candidates.filterNotNull().distinct().firstOrNull {
            isCandidate(it) && it.isFile && File(it.path + ".json").isFile && reusable(it)
        }?.let { return it }

        // Only the native producer reserves the directory. Never adopt even an empty live claim.
        if (saved != null && saved.parentFile != root && saved.parentFile?.exists() == false) {
            return saved
        }
        val generation = File(root, "generation-${UUID.randomUUID()}")
        return File(generation, "game.nup")
    }

    private val generationName =
        Regex("generation-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")

    private fun prepareRoots(context: Context): List<File> =
        (listOf(context.filesDir, context.noBackupFilesDir) +
            context.getExternalFilesDirs(null).filterNotNull()).map { it.canonicalFile }

    private fun ownedPrepareOutput(destination: String, roots: List<File>): File =
        File(destination).canonicalFile.also { output ->
            require(roots.any { output.path.startsWith(it.path + File.separator) }) {
                "Prepare input must be inside app-owned persistent storage"
            }
        }

    /** Blocking worker-only export. The completed NUP remains the runtime's immutable RomFS. */
    @WorkerThread
    fun exportPrepareInput(
        context: Context,
        path: String,
        programIndex: Int,
        expected: Match,
        destination: String,
        progress: NativePrepareProgress
    ): String? = try {
        val roots = prepareRoots(context)
        val output = ownedPrepareOutput(destination, roots)
        require(output.parentFile?.parentFile?.isDirectory == true) {
            "Prepare input root is missing"
        }
        val source = File(path)
        val immutableLocalNup = !path.startsWith("content://") && source.isFile &&
            !source.canWrite() && roots.any {
                source.canonicalPath.startsWith(it.path + File.separator)
            }
        GameHelper.withPrepareContent(path, { expected.build.programId.toULong(16).toString() }) {
            exportPrepareInputNative(path, programIndex, identityFields(expected),
                output.path, progress, immutableLocalNup)
        }
    } catch (e: Exception) {
        e.message ?: "Cannot export effective Prepare input"
    }

    internal fun parseIdentity(fields: Array<String>): Identity? = runCatching {
        require(fields.size >= 5 && Regex("[0-9A-F]{16}").matches(fields[0]))
        val version = fields[1].toLong()
        val bytes = fields[3].toLong()
        require(version in 0..0xFFFFFFFFL && bytes > 0 &&
            Regex("[0-9a-f]{64}").matches(fields[2]))
        val modules = fields.drop(4).associate { field ->
            val pair = field.split('=', limit = 2)
            require(pair.size == 2 && pair[0].isNotEmpty() &&
                Regex("[0-9a-f]{64}").matches(pair[1]))
            pair[0] to pair[1]
        }
        require("main" in modules && modules.size == fields.size - 4)
        Identity(NativeGameBuild(fields[0], version, modules), fields[2], bytes)
    }.getOrNull()

    private fun identityFields(match: Match): Array<String> = arrayOf(
        match.build.programId, match.build.version.toString(), match.contentKey,
        match.inputBytes.toString(),
        *match.build.moduleBuildIds.toSortedMap().map { (name, id) -> "$name=$id" }.toTypedArray()
    )

    // Called synchronously by JNI for every underlying container, including split SAF parts.
    @Keep
    @JvmStatic
    private fun sourceStamp(path: String): String? = try {
        if (path.startsWith("content://")) {
            val columns = arrayOf(DocumentsContract.Document.COLUMN_SIZE,
                DocumentsContract.Document.COLUMN_LAST_MODIFIED)
            org.yuzu.yuzu_emu.YuzuApplication.appContext.contentResolver.query(
                Uri.parse(path), columns, null, null, null
            )?.use { cursor ->
                if (!cursor.moveToFirst() || cursor.isNull(0) || cursor.isNull(1)) null
                else {
                    val size = cursor.getLong(0)
                    val modified = cursor.getLong(1)
                    if (size > 0 && modified > 0) "$size:$modified" else null
                }
            }
        } else {
            val file = File(path)
            if (file.isFile && file.length() > 0 && file.lastModified() > 0)
                "${file.length()}:${file.lastModified()}" else null
        }
    } catch (_: Exception) {
        null
    }

    private external fun readPrepareProgramId(path: String, programIndex: Int): String?

    private external fun readGameBuild(path: String, programIndex: Int): Array<String>?

    private external fun isPrepareInputCompleteNative(
        path: String,
        programIndex: Int,
        expected: Array<String>,
        destination: String
    ): Boolean

    private external fun exportPrepareInputNative(
        path: String,
        programIndex: Int,
        expected: Array<String>,
        destination: String,
        progress: NativePrepareProgress,
        immutableLocalNup: Boolean
    ): String?
}
