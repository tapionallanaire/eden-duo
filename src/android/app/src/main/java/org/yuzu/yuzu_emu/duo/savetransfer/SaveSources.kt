// SPDX-FileCopyrightText: Copyright 2026 tapionallanaire
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.savetransfer

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import java.io.File
import java.io.IOException
import java.io.InputStream
import org.yuzu.yuzu_emu.utils.EmulatorImportConfig
import org.yuzu.yuzu_emu.utils.EmulatorImportSources
import org.yuzu.yuzu_emu.utils.PathUtil

/** An installed app saves can be copied from, and its persisted read grant (a tree URI) if any. */
data class SourceApp(val source: EmulatorImportSources.Source, val grantedTree: String?)

object SaveSources {
    val EDEN_FAMILY = listOf(
        "dev.eden.eden_emulator",
        "dev.eden.eden_emulator.nightly",
        "dev.igawa6.edenduo",
        "dev.igawa6.edenduo.dev"
    )
    const val DUET_PLUS = "com.tapionallanaire.duetplus"

    fun allowed(packageName: String, selfPackage: String, includeDuetPlus: Boolean): Boolean =
        packageName != selfPackage &&
            (packageName in EDEN_FAMILY || includeDuetPlus && packageName == DUET_PLUS)

    fun needsAccess(app: SourceApp): Boolean = app.grantedTree == null

    /** Where an Eden app keeps its saves: the folder holding `user/save`. */
    sealed interface SaveLocation {
        /** Inside the app's own files folder, at this path from its root. */
        data class Own(val path: List<String>) : SaveLocation

        /** Outside it (a folder picked in its settings): a file path or a content:// tree URI. */
        data class Elsewhere(val value: String) : SaveLocation
    }

    /**
     * Reads it from the app's config.ini the way Eden does: save_directory, else nand_directory,
     * else the default `nand`. Without this, an app whose saves moved to the SD card still shows
     * the stale copy left in its own folder.
     */
    fun saveLocation(config: String?, authority: String): SaveLocation {
        fun read(key: String): String = config?.let {
            Regex("""^[ \t]*(?:Paths\\)?$key[ \t]*=[ \t]*"?([^"\r\n]*)"?[ \t]*$""", RegexOption.MULTILINE)
                .find(it)?.groupValues?.get(1)?.trim()
        }.orEmpty()
        val value = read("save_directory").ifBlank { read("nand_directory") }
        if (value.isBlank()) return SaveLocation.Own(listOf("nand"))
        val own = EmulatorImportConfig.internalRelative(value, authority)
            ?: return SaveLocation.Elsewhere(value)
        return SaveLocation.Own(own.split('/').filter { it.isNotEmpty() })
    }

    fun available(context: Context, includeDuetPlus: Boolean): List<SourceApp> {
        val grants = context.contentResolver.persistedUriPermissions.filter { it.isReadPermission }
        return EmulatorImportSources.installed(context)
            .filter { allowed(it.packageName, context.packageName, includeDuetPlus) }
            .map { source ->
                SourceApp(source, grants.firstOrNull { it.uri.authority == source.authority }?.uri?.toString())
            }
    }
}

/**
 * Reads one source app's Switch saves; never writes to them. Saves in the app's own folder are read
 * through its documents provider, saves it keeps elsewhere ([elsewhere]) straight from disk, which
 * takes All files access.
 */
class SourceReader(context: Context, tree: String) {
    private val resolver = context.contentResolver
    private val treeUri: Uri = Uri.parse(tree)
    private val rootId: String = DocumentsContract.getTreeDocumentId(treeUri)

    private fun children(id: String): List<SaveTree.Doc> {
        val uri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, id)
        val projection = arrayOf(
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_LAST_MODIFIED,
            DocumentsContract.Document.COLUMN_SIZE
        )
        return resolver.query(uri, projection, null, null, null)?.use { c ->
            buildList {
                while (c.moveToNext()) {
                    add(
                        SaveTree.Doc(
                            id = c.getString(0),
                            name = c.getString(1).orEmpty(),
                            isDir = c.getString(2) == DocumentsContract.Document.MIME_TYPE_DIR,
                            modified = if (c.isNull(3)) 0 else c.getLong(3),
                            size = if (c.isNull(4)) 0 else c.getLong(4)
                        )
                    )
                }
            }
        } ?: throw IOException("Cannot read the source app's files")
    }

    private fun open(id: String): InputStream =
        resolver.openInputStream(DocumentsContract.buildDocumentUriUsingTree(treeUri, id))
            ?: throw IOException("Cannot read a save file")

    /** The app's config.ini, read through its provider; null when it has none yet. */
    private fun config(): String? {
        val dir = children(rootId).firstOrNull { it.isDir && it.name == "config" } ?: return null
        val file = children(dir.id).firstOrNull { !it.isDir && it.name == "config.ini" } ?: return null
        return open(file.id).use { it.reader().readText() }
    }

    private val location = SaveSources.saveLocation(config(), treeUri.authority.orEmpty())

    /** The source's save folder when it's outside the app, as a path on disk. */
    val elsewhere: File? = (location as? SaveSources.SaveLocation.Elsewhere)?.let { (value) ->
        val path = if (value.startsWith("content://")) PathUtil.getPathFromUri(Uri.parse(value)) else value
        File(path ?: throw IOException("Cannot find the source app's save folder $value"))
    }

    private val saveTree = if (elsewhere != null) {
        SaveTree(
            children = { id ->
                File(id).listFiles()?.map { SaveTree.Doc(it.path, it.name, it.isDirectory, it.lastModified(), it.length()) }
                    ?: throw IOException("Cannot read $id")
            },
            open = { File(it).inputStream() },
            base = emptyList()
        )
    } else {
        SaveTree(::children, ::open, (location as SaveSources.SaveLocation.Own).path)
    }
    private val treeRoot = elsewhere?.path ?: rootId

    fun saves(): List<SaveEntry> = saveTree.saves(treeRoot)

    fun copyTitle(entry: SaveEntry, into: File) = saveTree.copyTitle(treeRoot, entry, into)
}

/**
 * A source app's `<base>/user/save/0000000000000000/<profile>/<title>` folders, walked through
 * [children] and [open] so the walk itself doesn't depend on the documents provider.
 */
class SaveTree(
    private val children: (String) -> List<Doc>,
    private val open: (String) -> InputStream,
    private val base: List<String> = listOf("nand")
) {
    data class Doc(val id: String, val name: String, val isDir: Boolean, val modified: Long, val size: Long)

    private fun saveRoot(rootId: String): String? =
        (base + SAVE_PATH).fold(rootId as String?) { id, name ->
            id?.let { parent -> children(parent).firstOrNull { it.isDir && it.name == name }?.id }
        }

    private fun titleFolders(rootId: String): List<Pair<String, Doc>> {
        val root = saveRoot(rootId) ?: return emptyList()
        return children(root).filter { it.isDir }.flatMap { profile ->
            children(profile.id).filter { it.isDir && SaveCatalog.isTitleId(it.name) }.map { profile.name to it }
        }
    }

    /** Every file under [dirId], keyed by its path relative to it. */
    private fun files(dirId: String, prefix: String = "", depth: Int = 0): List<Pair<String, Doc>> {
        if (depth > MAX_DEPTH) throw IOException("A save has too many nested folders")
        return children(dirId).flatMap { doc ->
            if (doc.name.isEmpty() || '/' in doc.name || doc.name == "." || doc.name == "..") {
                throw IOException("Invalid save file name")
            }
            val path = prefix + doc.name
            if (doc.isDir) files(doc.id, "$path/", depth + 1) else listOf(path to doc)
        }
    }

    fun saves(rootId: String): List<SaveEntry> = titleFolders(rootId).map { (profile, title) ->
        // Eden writes its size marker into every save folder, even one the game never wrote to.
        val files = files(title.id).map { it.second }.filter { it.name != SaveCatalog.SIZE_MARKER }
        SaveEntry(
            titleId = title.name.uppercase(),
            profile = profile,
            lastModified = files.maxOfOrNull { it.modified } ?: title.modified,
            bytes = files.sumOf { it.size },
            fileCount = files.size
        )
    }

    /** Copies that game's whole save folder, subfolders included, into [into]. */
    fun copyTitle(rootId: String, entry: SaveEntry, into: File) {
        val (_, title) = titleFolders(rootId).firstOrNull { (profile, doc) ->
            profile == entry.profile && doc.name.equals(entry.titleId, ignoreCase = true)
        } ?: throw IOException("The save for ${entry.titleId} is gone from the source app")
        for ((path, doc) in files(title.id)) {
            val target = File(into, path)
            target.parentFile?.mkdirs()
            open(doc.id).use { src -> target.outputStream().use { src.copyTo(it) } }
            if (doc.modified > 0) target.setLastModified(doc.modified)
        }
    }

    private companion object {
        val SAVE_PATH = listOf("user", "save", "0000000000000000")
        const val MAX_DEPTH = 32
    }
}
