// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import android.content.SharedPreferences
import android.net.Uri
import android.provider.DocumentsContract
import androidx.core.content.edit
import androidx.core.net.toUri
import androidx.preference.PreferenceManager
import java.util.concurrent.locks.ReentrantReadWriteLock
import kotlin.concurrent.read
import kotlin.concurrent.write
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.utils.ExternalContentBootRules
import org.yuzu.yuzu_emu.utils.ExternalContentBootRules.ContainerStamp
import org.yuzu.yuzu_emu.utils.ExternalContentBootRules.ContainerTitles
import org.yuzu.yuzu_emu.utils.ExternalContentBootRules.ContentContainer
import org.yuzu.yuzu_emu.utils.ExternalContentBootRules.ContentRecord
import org.yuzu.yuzu_emu.utils.FileUtil
import org.yuzu.yuzu_emu.utils.Log
import org.yuzu.yuzu_emu.utils.NativeConfig

/**
 * Eden Duo: the game library's update/DLC bookkeeping for GameHelper, so a game that boots before
 * (or without) a full library scan still gets its own update and DLC registered.
 *
 * - The data-import lock keeps an emulator data import from running under a library scan or a
 *   boot.
 * - The content index retains readable title/kind/version observations across incomplete scans;
 *   a boot registers the booting game's containers from it.
 * - For external content folders a boot registers only the containers that carry the booting
 *   title, matched by size and modified time against a stored title list, instead of opening
 *   every container (which over a network share takes minutes).
 */
object GameLibraryContent {
    @PublishedApi
    internal val dataImportLock = ReentrantReadWriteLock()

    inline fun <T> withDataImportRead(action: () -> T): T = dataImportLock.read(action)

    inline fun <T> withDataImportWrite(action: () -> T): T = dataImportLock.write(action)

    private const val KEY_GAME_CONTENT_CONTAINERS = "GameContentContainers"

    // Which base titles each external-content container carries, keyed by container uri and
    // checked against its positive size and modified time. A boot from a cold app (a front-end launch)
    // then registers only the booting game's update/DLC containers instead of opening and parsing
    // every container in every external content folder, which over a network share takes minutes.
    private const val KEY_EXTERNAL_CONTAINER_TITLES = "ExternalContainerTitles"

    // FileSys::GetBaseTitleID: updates (+0x800), DLC (+0x1000 + n) and extra programs of a
    // multi-program title all share their base game's id once the low 13 bits are cleared.
    private const val BASE_TITLE_ID_MASK = 0x1FFFL.inv()

    // Successful readable observations are retained across incomplete scans. Missing paths are
    // evidence, not deletions: only data import clears them; external roots are scoped at use.
    @Volatile
    private var contentContainers: Map<Long, List<ContentContainer>>? = null

    private fun readContentRecords(uri: String): List<ContentRecord> =
        NativeLibrary.getContainerContentRecords(uri).toList().chunked(3).map {
            ContentRecord(it[0], it[1].toInt(), if (it[1] == 1L) it[2] else null)
        }

    /**
     * All library, properties and direct-launch registration paths record readable title/kind/
     * version evidence, even if registration then fails. Call under GameHelper's provider lock.
     * An unreadable replacement never erases the last known selected-title requirement.
     */
    fun registerContainer(
        uri: String,
        gameFolder: Boolean,
        external: Boolean = !gameFolder
    ): Boolean {
        val records = runCatching { readContentRecords(uri) }.getOrDefault(emptyList())
        if (records.isNotEmpty()) {
            val remembered = ExternalContentBootRules.rememberContainer(
                loadContentContainers(), uri, gameFolder, external, records
            )
            if (remembered != contentContainers) {
                contentContainers = remembered
                PreferenceManager.getDefaultSharedPreferences(YuzuApplication.appContext).edit {
                    putString(KEY_GAME_CONTENT_CONTAINERS, Json.encodeToString(remembered))
                }
            }
        }
        return registerFile(ContentContainer(uri, gameFolder))
    }

    private fun registerFile(container: ContentContainer): Boolean = try {
        if (container.gameFolder) {
            NativeLibrary.addGameFolderFileToFilesystemProvider(container.uri)
        } else {
            NativeLibrary.addFileToFilesystemProvider(container.uri)
        }
    } catch (e: Exception) {
        Log.warning("[GameHelper] Cannot register ${container.uri}: ${e.message}")
        false
    }

    /** An emulator data import replaced the library: everything remembered is dropped. */
    fun forget(editor: SharedPreferences.Editor) {
        contentContainers = null
        editor.remove(KEY_GAME_CONTENT_CONTAINERS)
        editor.remove(KEY_EXTERNAL_CONTAINER_TITLES)
    }

    /**
     * Registers the booting game's update/DLC and returns whether every known enabled selected-
     * title container can still be opened/registered. Warm flags optimize discovery only, never
     * completeness. Both persistent caches are captured before any selector runs. Emulation may
     * ignore the result; native Prepare must reject false before reading/exporting a build.
     * Legacy base-title-only records remain conservatively required, since disabled kind/version
     * cannot be established from them. No claim is made about content never observed by this app.
     */
    fun registerContentForBoot(
        gamePath: String,
        programId: String,
        externalContentMounted: Boolean,
        libraryScanCompleted: Boolean,
        externalContentExtensions: Set<String>,
        mountAllExternal: (MutableSet<String>) -> Boolean,
        mountGameFolder: (Uri, MutableSet<String>) -> Unit
    ): Boolean {
        val mountedContainerUris = mutableSetOf<String>()
        val baseTitleId = (programId.toLongOrNull() ?: 0L) and BASE_TITLE_ID_MASK
        val knownBefore = loadContentContainers()[baseTitleId].orEmpty()
        val externalBefore = loadExternalContainerTitles()
        if (!externalContentMounted) {
            // Only this game's containers are registered here, so externalContentMounted stays
            // false and the next library scan still mounts every folder.
            val selective = baseTitleId != 0L &&
                mountExternalContentForTitle(
                    baseTitleId,
                    mountedContainerUris,
                    externalContentExtensions
                )
            if (!selective) {
                runCatching { mountAllExternal(mountedContainerUris) }.onFailure {
                    Log.warning("[GameHelper] Cannot discover external content: ${it.message}")
                }
            }
        }
        val externalCount = mountedContainerUris.size

        val known = loadContentContainers()[baseTitleId].orEmpty()
            .filter { !it.external || inExternalRoots(it.uri) }
        val registered = mutableListOf<String>()
        known.forEach {
            if (it.uri != gamePath && it.uri !in mountedContainerUris) {
                if (registerContainer(it.uri, it.gameFolder, it.external)) {
                    mountedContainerUris.add(it.uri)
                    registered.add(it.uri)
                }
            }
        }
        val beforeSiblings = mountedContainerUris.size
        // Before the first scan, or for a game the scan has no content for (a frontend launch of a
        // game outside the library folders), its own folder is the only place left to look.
        if (!libraryScanCompleted || known.isEmpty()) {
            runCatching { mountGameFolder(Uri.parse(gamePath), mountedContainerUris) }.onFailure {
                Log.warning("[GameHelper] Cannot discover sibling content: ${it.message}")
            }
        }

        Log.info(
            "[GameHelper] Boot content for ${baseTitleId.toString(16).padStart(16, '0')}: " +
                "${registered.size} known container(s) $registered, " +
                "${mountedContainerUris.size - beforeSiblings} folder sibling(s), " +
                "$externalCount external container(s)"
        )
        val required = ExternalContentBootRules.requiredContainers(
            baseTitleId,
            gamePath,
            knownBefore + loadContentContainers()[baseTitleId].orEmpty(),
            externalBefore + loadExternalContainerTitles(),
            NativeLibrary.getDisabledContentAddons(programId).toSet(),
            ::inExternalRoots
        )
        // Never trust mountedContainerUris or either warm flag after SAF permissions can change.
        return ExternalContentBootRules.registerRequired(required, ::registerFile)
    }

    private fun loadContentContainers(): Map<Long, List<ContentContainer>> {
        contentContainers?.let { return it }
        val stored = PreferenceManager.getDefaultSharedPreferences(YuzuApplication.appContext)
            .getString(KEY_GAME_CONTENT_CONTAINERS, null)
        val loaded = stored?.let {
            try {
                Json.decodeFromString<Map<Long, List<ContentContainer>>>(it)
            } catch (_: Exception) {
                null
            }
        } ?: emptyMap()
        contentContainers = loaded
        return loaded
    }

    /**
     * Registers only the external-content containers that carry content for [baseTitleId].
     * Containers already seen with the same size and modified time are matched from the stored
     * title list without being opened; new or changed ones are read once and remembered.
     * Returns false, having registered nothing, when a folder can't be listed, so the caller
     * falls back to registering every container.
     */
    private fun mountExternalContentForTitle(
        baseTitleId: Long,
        mountedContainerUris: MutableSet<String>,
        externalContentExtensions: Set<String>
    ): Boolean {
        val startedAt = System.currentTimeMillis()
        val stamps = mutableListOf<ContainerStamp>()
        val uniqueExternalContentDirs = linkedSetOf<String>()
        NativeConfig.getExternalContentDirs().forEach { externalDir ->
            if (externalDir.isNotEmpty()) {
                uniqueExternalContentDirs.add(externalDir)
            }
        }
        for (externalDir in uniqueExternalContentDirs) {
            val externalDirUri = externalDir.toUri()
            if (!listContainerStamps(externalDirUri, stamps, externalContentExtensions)) {
                return false
            }
        }

        val stored = loadExternalContainerTitles()
        val readRecords = mutableMapOf<String, List<ContentRecord>>()
        val selection = try {
            ExternalContentBootRules.select(baseTitleId, stamps, stored) {
                val records = readContentRecords(it)
                if (records.isNotEmpty()) readRecords[it] = records
                records.map { record -> record.baseTitleId }.distinct()
            }
        } catch (e: Exception) {
            Log.warning("[GameHelper] Cannot read external content titles: ${e.message}")
            return false
        }
        val remembered = selection.remembered.mapValues { (uri, entry) ->
            readRecords[uri]?.let { entry.copy(records = it) } ?: entry
        }
        val matches = selection.matches
        if (remembered != stored) {
            PreferenceManager.getDefaultSharedPreferences(YuzuApplication.appContext).edit {
                putString(KEY_EXTERNAL_CONTAINER_TITLES, Json.encodeToString(remembered))
            }
        }

        matches.forEach {
            if (it !in mountedContainerUris && registerContainer(it, false)) {
                mountedContainerUris.add(it)
            }
        }
        Log.info(
            "[GameHelper] External content for ${baseTitleId.toString(16).padStart(16, '0')}: " +
                "${stamps.size} listed, ${selection.readCount} read, ${matches.size} registered " +
                "in ${System.currentTimeMillis() - startedAt} ms"
        )
        return true
    }

    private fun loadExternalContainerTitles(): Map<String, ContainerTitles> {
        val stored = PreferenceManager.getDefaultSharedPreferences(YuzuApplication.appContext)
            .getString(KEY_EXTERNAL_CONTAINER_TITLES, null) ?: return emptyMap()
        return try {
            Json.decodeFromString<Map<String, ContainerTitles>>(stored)
        } catch (_: Exception) {
            emptyMap()
        }
    }

    /**
     * Compare document identities without querying the provider: a lost SAF grant must not turn
     * a configured path into an apparently removed root. A tree grant's identity also handles
     * opaque document ids; sub-document roots use the walks' slash-descendant convention.
     */
    private fun inExternalRoots(path: String): Boolean {
        val uri = Uri.parse(path)
        return NativeConfig.getExternalContentDirs().filter { it.isNotEmpty() }.any { root ->
            val rootUri = Uri.parse(root)
            if (uri.scheme == "content" && rootUri.scheme == "content") {
                runCatching {
                    val rootId = if (FileUtil.isRootTreeUri(rootUri)) {
                        DocumentsContract.getTreeDocumentId(rootUri)
                    } else {
                        DocumentsContract.getDocumentId(rootUri)
                    }
                    val id = DocumentsContract.getDocumentId(uri)
                    val sameTree = FileUtil.isRootTreeUri(rootUri) &&
                        runCatching { DocumentsContract.getTreeDocumentId(uri) == rootId }
                            .getOrDefault(false)
                    uri.authority == rootUri.authority &&
                        (sameTree || ExternalContentBootRules.inDocumentRoot(id, rootId))
                }.getOrDefault(false)
            } else {
                val rootPath = (rootUri.path ?: root).trimEnd('/')
                val filePath = uri.path ?: path
                filePath == rootPath || filePath.startsWith("$rootPath/")
            }
        }
    }

    /**
     * Collects the nsp/xci files under [folderUri] with their size and modified time, walking as
     * deep as GameHelper's external content walk does. [folderUri] is a tree root or a document
     * inside a tree, as FileUtil.listFiles accepts. Returns false if any folder query fails.
     */
    private fun listContainerStamps(
        folderUri: Uri,
        stamps: MutableList<ContainerStamp>,
        externalContentExtensions: Set<String>
    ): Boolean {
        return try {
            val folderDocumentId = if (FileUtil.isRootTreeUri(folderUri)) {
                DocumentsContract.getTreeDocumentId(folderUri)
            } else {
                DocumentsContract.getDocumentId(folderUri)
            }
            collectContainerStamps(folderUri, folderDocumentId, 3, stamps, externalContentExtensions)
        } catch (e: Exception) {
            Log.warning("[GameHelper] Cannot list external content folder: ${e.message}")
            false
        }
    }

    private fun collectContainerStamps(
        treeUri: Uri,
        parentDocumentId: String,
        depth: Int,
        stamps: MutableList<ContainerStamp>,
        externalContentExtensions: Set<String>
    ): Boolean {
        if (depth <= 0) {
            return true
        }
        val columns = arrayOf(
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_SIZE,
            DocumentsContract.Document.COLUMN_LAST_MODIFIED
        )
        val childrenUri =
            DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, parentDocumentId)
        val folders = mutableListOf<String>()
        val cursor = YuzuApplication.appContext.contentResolver
            .query(childrenUri, columns, null, null, null) ?: return false
        cursor.use {
            while (cursor.moveToNext()) {
                val documentId = cursor.getString(0)
                val name = cursor.getString(1) ?: ""
                if (cursor.getString(2) == DocumentsContract.Document.MIME_TYPE_DIR) {
                    folders.add(documentId)
                } else if (externalContentExtensions.contains(
                        name.substringAfterLast('.', "").lowercase()
                    )
                ) {
                    val uri = DocumentsContract.buildDocumentUriUsingTree(treeUri, documentId)
                    val size = if (cursor.isNull(3)) 0L else cursor.getLong(3)
                    val modified = if (cursor.isNull(4)) 0L else cursor.getLong(4)
                    stamps.add(ContainerStamp(uri.toString(), size, modified))
                }
            }
        }
        return folders.all {
            collectContainerStamps(treeUri, it, depth - 1, stamps, externalContentExtensions)
        }
    }
}
