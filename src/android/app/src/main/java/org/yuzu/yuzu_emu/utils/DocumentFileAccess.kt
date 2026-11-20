// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
package org.yuzu.yuzu_emu.utils

import android.net.Uri
import android.provider.DocumentsContract
import android.util.LruCache
import org.yuzu.yuzu_emu.YuzuApplication

/** Resolve native VFS suffixes below a granted SAF tree without assuming filesystem paths. */
object DocumentFileAccess {
    private val resolver get() = YuzuApplication.appContext.contentResolver
    private const val ID = DocumentsContract.Document.COLUMN_DOCUMENT_ID
    private const val NAME = DocumentsContract.Document.COLUMN_DISPLAY_NAME
    private const val MIME = DocumentsContract.Document.COLUMN_MIME_TYPE
    private const val SIZE = DocumentsContract.Document.COLUMN_SIZE
    private data class TreePath(val base: Uri, val names: List<String>)
    private data class Child(
        val name: String,
        val uri: Uri,
        val directory: Boolean,
        val size: Long = -1
    )
    // Native VFS operations repeatedly resolve the same children. Cache document identities,
    // not file contents or existence, and invalidate after mutations.
    private val resolved = LruCache<String, Uri>(8192)

    private fun split(path: String): TreePath {
        val uri = Uri.parse(path)
        require(uri.scheme == "content")
        val parts = uri.pathSegments
        val document = parts.indexOf("document")
        val tree = parts.indexOf("tree")
        if (tree < 0) return TreePath(uri, emptyList())
        val index = if (document >= 0) document else tree
        val base = DocumentsContract.buildDocumentUriUsingTree(uri, parts[index + 1])
        val names = parts.drop(index + 2)
        require(names.none { it.isEmpty() || it == "." || it == ".." || '/' in it || '\\' in it })
        return TreePath(base, names)
    }

    private fun children(parent: Uri, prefix: String? = null): List<Child> {
        val childUri = DocumentsContract.buildChildDocumentsUriUsingTree(parent, DocumentsContract.getDocumentId(parent))
        return resolver.query(childUri, arrayOf(ID, NAME, MIME, SIZE), null, null, null)?.use { cursor ->
            buildList {
                while (cursor.moveToNext()) {
                    val name = cursor.getString(1)
                    val uri = DocumentsContract.buildDocumentUriUsingTree(parent, cursor.getString(0))
                    if (prefix != null) resolved.put("$prefix/${Uri.encode(name)}", uri)
                    val size = if (cursor.isNull(3)) -1L else cursor.getLong(3)
                    add(
                        Child(
                            name,
                            uri,
                            cursor.getString(2) == DocumentsContract.Document.MIME_TYPE_DIR,
                            size
                        )
                    )
                }
            }
        } ?: error("Cannot list folder")
    }

    /** Cache key of a path, built exactly as [resolve] builds it while walking. */
    private fun key(tree: TreePath): String =
        tree.names.fold(tree.base.toString()) { prefix, name -> "$prefix/${Uri.encode(name)}" }

    /** Drop a mutated path and everything cached below it; other identities stay valid. */
    private fun evict(tree: TreePath) {
        val key = key(tree)
        resolved.remove(key)
        resolved.snapshot().keys.forEach { if (it.startsWith("$key/")) resolved.remove(it) }
    }

    private fun resolve(path: String, createDirectories: Boolean = false): Uri? {
        val tree = split(path)
        var current = tree.base
        var prefix = tree.base.toString()
        for (name in tree.names) {
            val key = "$prefix/${Uri.encode(name)}"
            current = resolved.get(key)
                ?: children(current, prefix).firstOrNull { it.name == name }?.uri
                ?: if (createDirectories) DocumentsContract.createDocument(resolver, current,
                    DocumentsContract.Document.MIME_TYPE_DIR, name) ?: return null else return null
            resolved.put(key, current)
            prefix = key
        }
        return current
    }

    /**
     * One column of [uri], or null when the row or the column is missing. A provider that is not
     * a documents provider (a frontend's FileProvider: ES-DE hands games over as
     * content://org.es_de.frontend.files/...) answers only the openable columns, so the column
     * asked for may be absent from the cursor rather than null.
     */
    private fun value(uri: Uri, column: String): String? =
        resolver.query(uri, arrayOf(column), null, null, null)?.use {
            val index = it.getColumnIndex(column)
            if (index >= 0 && it.moveToFirst()) it.getString(index) else null
        }

    /** True when the provider answers a row for [uri], whatever columns it has. */
    private fun hasRow(uri: Uri): Boolean =
        resolver.query(uri, arrayOf(ID), null, null, null)?.use { it.count > 0 } ?: false

    fun exists(path: String): Boolean = runCatching {
        resolve(path)?.let { hasRow(it) } ?: false
    }.getOrDefault(false)

    fun isDirectory(path: String): Boolean = runCatching {
        resolve(path)?.let { value(it, MIME) == DocumentsContract.Document.MIME_TYPE_DIR } ?: false
    }.getOrDefault(false)

    fun size(path: String): Long = runCatching {
        resolve(path)?.let { value(it, DocumentsContract.Document.COLUMN_SIZE)?.toLongOrNull() } ?: 0L
    }.getOrDefault(0L)

    internal fun sourceStamp(path: String): String? {
        val uri = resolve(path) ?: return null
        val modified = DocumentsContract.Document.COLUMN_LAST_MODIFIED
        return resolver.query(uri, arrayOf(SIZE, modified), null, null, null)?.use { cursor ->
            if (!cursor.moveToFirst()) return null
            val bytes = cursor.getLong(cursor.getColumnIndexOrThrow(SIZE))
            val time = cursor.getLong(cursor.getColumnIndexOrThrow(modified))
            if (bytes > 0 && time > 0) "$bytes:$time" else null
        }
    }

    fun name(path: String): String = runCatching {
        resolve(path)?.let { value(it, NAME) } ?: ""
    }.getOrDefault("")

    fun parent(path: String): String = runCatching {
        val parts = split(path)
        if (parts.names.isEmpty()) path else path.substringBeforeLast('/')
    }.getOrDefault(path)

    fun open(path: String, mode: String): Int = runCatching {
        val uri = resolve(path) ?: return -1
        resolver.openFileDescriptor(uri, mode)?.detachFd() ?: -1
    }.getOrDefault(-1)

    fun entries(path: String): Array<String> = runCatching {
        val uri = resolve(path) ?: return emptyArray()
        // Fill the cache so per-child lookups after a listing need no extra queries.
        // "d:<name>" for folders, "f:<size>/<name>" for files ("f:/<name>" when the size is
        // unknown), so native code learns file sizes without opening every file.
        children(uri, key(split(path))).map { child ->
            if (child.directory) {
                "d:" + child.name
            } else {
                "f:" + (if (child.size >= 0) child.size.toString() else "") + "/" + child.name
            }
        }.toTypedArray()
    }.getOrDefault(emptyArray())

    /** Read metadata beneath the actual native VFS path, including appended SAF child names. */
    fun companionPackages(path: String): List<String?> {
        val root = resolve(path) ?: return emptyList()
        return children(root).filter { it.directory && !it.name.startsWith(".") }.mapNotNull { folder ->
            val contents = children(folder.uri)
            val dualscreen = contents.firstOrNull { it.name == "dualscreen" && it.directory }
                ?: return@mapNotNull null
            if (children(dualscreen.uri).none { it.name == "manifest.json" && !it.directory }) {
                return@mapNotNull null
            }
            // A wrapper retains unknown metadata as a real installed manifest.
            listOf(runCatching {
                val metadata = contents.firstOrNull { it.name == "package.json" && !it.directory }
                    ?: return@runCatching null
                resolver.openInputStream(metadata.uri)?.use { stream ->
                    val output = java.io.ByteArrayOutputStream()
                    val buffer = ByteArray(8192)
                    while (output.size() <= 1048576) {
                        val count = stream.read(buffer)
                        if (count < 0) break
                        output.write(buffer, 0, count)
                    }
                    if (output.size() > 1048576) null else output.toString("UTF-8")
                }
            }.getOrNull())
        }.flatten()
    }

    private fun deleteRecursively(uri: Uri): Boolean {
        if (value(uri, MIME) == DocumentsContract.Document.MIME_TYPE_DIR) {
            for (child in children(uri)) if (!deleteRecursively(child.uri)) return false
        }
        return DocumentsContract.deleteDocument(resolver, uri)
    }

    /** 0 mkdirs, 1 new file, 2 delete file, 3 empty directory, 4 recursive directory, 5 rename. */
    fun operate(path: String, other: String, operation: Int): Boolean = runCatching {
        val parts = split(path)
        try {
            when (operation) {
                0 -> resolve(path, createDirectories = true)?.let {
                    value(it, MIME) == DocumentsContract.Document.MIME_TYPE_DIR
                } ?: false
                1 -> {
                    require(parts.names.isNotEmpty())
                    if (exists(path)) false else {
                        val parent = resolve(parent(path), createDirectories = true) ?: return false
                        DocumentsContract.createDocument(resolver, parent, "application/octet-stream", parts.names.last()) != null
                    }
                }
                2, 3, 4 -> {
                    require(parts.names.isNotEmpty()) // Never delete a granted root.
                    val uri = resolve(path) ?: return true
                    val directory = value(uri, MIME) == DocumentsContract.Document.MIME_TYPE_DIR
                    if ((operation == 2 && directory) || (operation != 2 && !directory) ||
                        (operation == 3 && children(uri).isNotEmpty())) false
                    else if (operation == 4) deleteRecursively(uri) else DocumentsContract.deleteDocument(resolver, uri)
                }
                5 -> {
                    val target = split(other)
                    require(parts.names.isNotEmpty() && target.names.isNotEmpty())
                    // Providers cannot guarantee atomic replacement or cross-provider moves.
                    if (parent(path) != parent(other) || exists(other)) false else {
                        val uri = resolve(path) ?: return false
                        DocumentsContract.renameDocument(resolver, uri, target.names.last()) != null
                    }
                }
                else -> false
            }
        } finally {
            // Only positive identities are cached, so a mutation can stale just the touched path
            // and its descendants (path-based providers change child IDs on rename).
            if (operation in 1..5 && parts.names.isNotEmpty()) evict(parts)
            if (operation == 5) runCatching { evict(split(other)) }
        }
    }.getOrDefault(false)
}
