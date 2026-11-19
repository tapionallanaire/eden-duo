// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// SPDX-License-Identifier: MPL-2.0
// Copyright © 2023 Skyline Team and Contributors (https://github.com/skyline-emu/)

package org.yuzu.yuzu_emu.features

import android.database.Cursor
import android.database.MatrixCursor
import android.os.CancellationSignal
import android.os.Handler
import android.os.HandlerThread
import android.os.ParcelFileDescriptor
import android.os.ProxyFileDescriptorCallback
import android.os.storage.StorageManager
import android.system.ErrnoException
import android.system.OsConstants
import android.system.Os
import android.system.StructStat
import java.nio.channels.ClosedChannelException
import java.nio.channels.NonReadableChannelException
import java.nio.channels.NonWritableChannelException
import android.provider.DocumentsContract
import android.provider.DocumentsProvider
import android.webkit.MimeTypeMap
import java.io.*
import org.yuzu.yuzu_emu.BuildConfig
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.getPublicFilesDir
import org.yuzu.yuzu_emu.duo.nativeplay.NativeSaveSession

class DocumentProvider : DocumentsProvider() {
    private val baseDirectory: File
        get() = File(YuzuApplication.application.getPublicFilesDir().canonicalPath)
    private val proxyHandler by lazy {
        Handler(HandlerThread("save-documents").apply { start() }.looper)
    }

    private inline fun <T> proxyIo(
        operation: String,
        signal: CancellationSignal?,
        source: File?,
        sourceIdentity: StructStat?,
        action: () -> T
    ): T {
        if (signal?.isCanceled == true) throw ErrnoException(operation, OsConstants.ECANCELED)
        var sourceLocked = false
        try {
            if (source != null) {
                NativeSaveSession.lockSourceReads()
                sourceLocked = true
                if (requiresReadLease(source)) throw ErrnoException(operation, OsConstants.EBUSY)
                val expected = sourceIdentity ?: throw ErrnoException(operation, OsConstants.ESTALE)
                val current = Os.stat(source.path)
                if (current.st_dev != expected.st_dev || current.st_ino != expected.st_ino) {
                    throw ErrnoException(operation, OsConstants.ESTALE)
                }
            }
            return action()
        } catch (e: Exception) {
            var cause: Throwable? = e
            while (cause != null) {
                if (cause is ErrnoException) throw cause
                cause = cause.cause
            }
            val errno = when (e) {
                is NativeSaveSession.BusyException -> OsConstants.EBUSY
                is ClosedChannelException, is NonReadableChannelException, is NonWritableChannelException ->
                    OsConstants.EBADF
                is IllegalArgumentException -> OsConstants.EINVAL
                is FileNotFoundException -> OsConstants.ENOENT
                else -> OsConstants.EIO
            }
            throw ErrnoException(operation, errno, e)
        } finally {
            if (sourceLocked) NativeSaveSession.unlockSourceReads()
        }
    }

    // All mutations are exclusive; only lock-free classified non-save reads may coexist with a game.
    private inline fun <T> withSaveAccess(action: () -> T): T {
        try {
            return NativeSaveSession.acquire(requireNotNull(context)).use {
                NativeSaveSession.lockSourceMutations()
                try {
                    action()
                } finally {
                    NativeSaveSession.unlockSourceMutations()
                }
            }
        } catch (e: IllegalStateException) {
            throw FileNotFoundException(e.message).apply { initCause(e) }
        }
    }

    private fun requiresReadLease(file: File): Boolean = DocumentProviderPaths.requiresSaveLease(
        baseDirectory,
        file,
        if (YuzuApplication.startupDeferred) null else NativeSaveSession.sourceReadProtectedRoots()
    )

    private fun lockSourceReadsForDocument() {
        try {
            NativeSaveSession.lockSourceReads()
        } catch (e: NativeSaveSession.BusyException) {
            throw FileNotFoundException(e.message).apply { initCause(e) }
        }
    }

    private inline fun <T> withReadPath(file: File, action: (File, Boolean) -> T): T {
        val canonical = DocumentProviderPaths.contained(baseDirectory, file)
        lockSourceReadsForDocument()
        try {
            if (!requiresReadLease(canonical)) {
                if (!canonical.exists()) throw FileNotFoundException("The document does not exist")
                return action(canonical, false)
            }
        } finally {
            NativeSaveSession.unlockSourceReads()
        }
        return withSaveAccess {
            val guarded = DocumentProviderPaths.contained(baseDirectory, file)
            if (!guarded.exists()) throw FileNotFoundException("The document does not exist")
            action(guarded, true)
        }
    }

    companion object {
        private val DEFAULT_ROOT_PROJECTION: Array<String> = arrayOf(
            DocumentsContract.Root.COLUMN_ROOT_ID,
            DocumentsContract.Root.COLUMN_MIME_TYPES,
            DocumentsContract.Root.COLUMN_FLAGS,
            DocumentsContract.Root.COLUMN_ICON,
            DocumentsContract.Root.COLUMN_TITLE,
            DocumentsContract.Root.COLUMN_SUMMARY,
            DocumentsContract.Root.COLUMN_DOCUMENT_ID,
            DocumentsContract.Root.COLUMN_AVAILABLE_BYTES
        )

        private val DEFAULT_DOCUMENT_PROJECTION: Array<String> = arrayOf(
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_LAST_MODIFIED,
            DocumentsContract.Document.COLUMN_FLAGS,
            DocumentsContract.Document.COLUMN_SIZE
        )

        const val AUTHORITY: String = BuildConfig.APPLICATION_ID + ".user"
        const val ROOT_ID: String = DocumentProviderPaths.ROOT_ID
    }

    override fun onCreate(): Boolean {
        return true
    }

    /**
     * @return The [File] that corresponds to the document ID supplied by [getDocumentId]
     */
    private fun getFile(documentId: String): File =
        DocumentProviderPaths.resolve(baseDirectory, documentId)

    /**
     * @return A unique ID for the provided [File]
     */
    private fun getDocumentId(file: File): String =
        DocumentProviderPaths.documentId(baseDirectory, file)

    override fun queryRoots(projection: Array<out String>?): Cursor {
        val cursor = MatrixCursor(projection ?: DEFAULT_ROOT_PROJECTION)

        cursor.newRow().apply {
            add(DocumentsContract.Root.COLUMN_ROOT_ID, ROOT_ID)
            add(DocumentsContract.Root.COLUMN_SUMMARY, null)
            add(
                DocumentsContract.Root.COLUMN_FLAGS,
                DocumentsContract.Root.FLAG_SUPPORTS_CREATE or
                    DocumentsContract.Root.FLAG_SUPPORTS_IS_CHILD
            )
            add(DocumentsContract.Root.COLUMN_TITLE, context!!.getString(R.string.app_name))
            add(DocumentsContract.Root.COLUMN_DOCUMENT_ID, getDocumentId(baseDirectory))
            add(DocumentsContract.Root.COLUMN_MIME_TYPES, "*/*")
            add(DocumentsContract.Root.COLUMN_AVAILABLE_BYTES, baseDirectory.freeSpace)
            add(DocumentsContract.Root.COLUMN_ICON, R.drawable.ic_yuzu)
        }

        return cursor
    }

    override fun queryDocument(documentId: String?, projection: Array<out String>?): Cursor =
        withReadPath(DocumentProviderPaths.candidate(baseDirectory, requireNotNull(documentId))) { file, _ ->
        val cursor = MatrixCursor(projection ?: DEFAULT_DOCUMENT_PROJECTION)
        includeFile(cursor, null, file)
    }

    override fun isChildDocument(parentDocumentId: String?, documentId: String?): Boolean {
        if (parentDocumentId == null || documentId == null) return false
        val parent = DocumentProviderPaths.candidate(baseDirectory, parentDocumentId)
        val child = DocumentProviderPaths.candidate(baseDirectory, documentId)
        return child != parent && child.path.startsWith(parent.path + File.separator)
    }

    /**
     * @return A new [File] with a unique name based off the supplied [name], not conflicting with any existing file
     */
    private fun File.resolveWithoutConflict(name: String): File {
        var file = DocumentProviderPaths.child(this, name)
        if (file.exists()) {
            var noConflictId =
                1 // Makes sure two files don't have the same name by adding a number to the end
            val extension = name.substringAfterLast('.')
            val baseName = name.substringBeforeLast('.')
            while (file.exists())
                file = DocumentProviderPaths.child(this, "$baseName (${noConflictId++}).$extension")
        }
        return file
    }

    override fun createDocument(
        parentDocumentId: String?,
        mimeType: String?,
        displayName: String
    ): String = withSaveAccess {
        val parentFile = getFile(parentDocumentId!!)
        val newFile = parentFile.resolveWithoutConflict(displayName)

        try {
            if (DocumentsContract.Document.MIME_TYPE_DIR == mimeType) {
                if (!newFile.mkdir()) {
                    throw IOException("Failed to create directory")
                }
            } else {
                if (!newFile.createNewFile()) {
                    throw IOException("Failed to create file")
                }
            }
        } catch (e: IOException) {
            throw FileNotFoundException("Couldn't create document '${newFile.path}': ${e.message}")
        }

        return@withSaveAccess getDocumentId(newFile)
    }

    override fun deleteDocument(documentId: String?) = withSaveAccess {
        val file = getFile(documentId!!)
        if (!file.delete()) {
            throw FileNotFoundException("Couldn't delete document with ID '$documentId'")
        }
    }

    override fun removeDocument(documentId: String, parentDocumentId: String?) = withSaveAccess {
        val parent = getFile(parentDocumentId!!)
        val file = getFile(documentId)

        if (parent == file || file.parentFile == null || file.parentFile!! == parent) {
            if (!file.delete()) {
                throw FileNotFoundException("Couldn't delete document with ID '$documentId'")
            }
        } else {
            throw FileNotFoundException("Couldn't delete document with ID '$documentId'")
        }
    }

    override fun renameDocument(documentId: String?, displayName: String?): String = withSaveAccess {
        if (displayName == null) {
            throw FileNotFoundException(
                "Couldn't rename document '$documentId' as the new name is null"
            )
        }

        val sourceFile = getFile(documentId!!)
        if (sourceFile == baseDirectory) throw FileNotFoundException("The user directory cannot be renamed")
        val sourceParentFile = sourceFile.parentFile
            ?: throw FileNotFoundException(
                "Couldn't rename document '$documentId' as it has no parent"
            )
        val destFile = DocumentProviderPaths.child(sourceParentFile, displayName)

        try {
            if (!sourceFile.renameTo(destFile)) {
                throw FileNotFoundException(
                    "Couldn't rename document from '${sourceFile.name}' to '${destFile.name}'"
                )
            }
        } catch (e: Exception) {
            throw FileNotFoundException(
                "Couldn't rename document from '${sourceFile.name}' to '${destFile.name}': " +
                    "${e.message}"
            )
        }

        return@withSaveAccess getDocumentId(destFile)
    }

    private fun copyDocument(
        sourceDocumentId: String,
        sourceParentDocumentId: String,
        targetParentDocumentId: String?
    ): String {
        if (!isChildDocument(sourceParentDocumentId, sourceDocumentId)) {
            throw FileNotFoundException(
                "Couldn't copy document '$sourceDocumentId' as its parent is not " +
                    "'$sourceParentDocumentId'"
            )
        }

        return copyDocument(sourceDocumentId, targetParentDocumentId)
    }

    override fun copyDocument(sourceDocumentId: String, targetParentDocumentId: String?): String = withSaveAccess {
        val parent = getFile(targetParentDocumentId!!)
        val oldFile = getFile(sourceDocumentId)
        val newFile = parent.resolveWithoutConflict(oldFile.name)

        try {
            if (!(
                newFile.createNewFile() && newFile.setWritable(true) &&
                    newFile.setReadable(true)
                )
            ) {
                throw IOException("Couldn't create new file")
            }

            FileInputStream(oldFile).use { inStream ->
                FileOutputStream(newFile).use { outStream ->
                    inStream.copyTo(outStream)
                }
            }
        } catch (e: IOException) {
            throw FileNotFoundException("Couldn't copy document '$sourceDocumentId': ${e.message}")
        }

        return@withSaveAccess getDocumentId(newFile)
    }

    override fun moveDocument(
        sourceDocumentId: String,
        sourceParentDocumentId: String?,
        targetParentDocumentId: String?
    ): String = withSaveAccess {
        try {
            val newDocumentId = copyDocument(
                sourceDocumentId,
                sourceParentDocumentId!!,
                targetParentDocumentId
            )
            removeDocument(sourceDocumentId, sourceParentDocumentId)
            return@withSaveAccess newDocumentId
        } catch (e: FileNotFoundException) {
            throw FileNotFoundException("Couldn't move document '$sourceDocumentId'")
        }
    }

    private fun includeFile(cursor: MatrixCursor, documentId: String?, file: File?): MatrixCursor {
        val localDocumentId = documentId ?: file?.let { getDocumentId(it) }
        val localFile = file ?: getFile(documentId!!)

        var flags = 0
        if (localFile.isDirectory && localFile.canWrite()) {
            flags = DocumentsContract.Document.FLAG_DIR_SUPPORTS_CREATE
        } else if (localFile.canWrite()) {
            flags = DocumentsContract.Document.FLAG_SUPPORTS_WRITE
            flags = flags or DocumentsContract.Document.FLAG_SUPPORTS_DELETE

            flags = flags or DocumentsContract.Document.FLAG_SUPPORTS_REMOVE
            flags = flags or DocumentsContract.Document.FLAG_SUPPORTS_MOVE
            flags = flags or DocumentsContract.Document.FLAG_SUPPORTS_COPY
            flags = flags or DocumentsContract.Document.FLAG_SUPPORTS_RENAME
        }

        cursor.newRow().apply {
            add(DocumentsContract.Document.COLUMN_DOCUMENT_ID, localDocumentId)
            add(
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                if (localFile == baseDirectory) {
                    context!!.getString(R.string.app_name)
                } else {
                    localFile.name
                }
            )
            add(DocumentsContract.Document.COLUMN_SIZE, localFile.length())
            add(DocumentsContract.Document.COLUMN_MIME_TYPE, getTypeForFile(localFile))
            add(DocumentsContract.Document.COLUMN_LAST_MODIFIED, localFile.lastModified())
            add(DocumentsContract.Document.COLUMN_FLAGS, flags)
            if (localFile == baseDirectory) {
                add(DocumentsContract.Root.COLUMN_ICON, R.drawable.ic_yuzu)
            }
        }

        return cursor
    }

    private fun getTypeForFile(file: File): Any {
        return if (file.isDirectory) {
            DocumentsContract.Document.MIME_TYPE_DIR
        } else {
            getTypeForName(file.name)
        }
    }

    private fun getTypeForName(name: String): Any {
        val lastDot = name.lastIndexOf('.')
        if (lastDot >= 0) {
            val extension = name.substring(lastDot + 1)
            val mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(extension)
            if (mime != null) {
                return mime
            }
        }
        return "application/octect-stream"
    }

    override fun queryChildDocuments(
        parentDocumentId: String?,
        projection: Array<out String>?,
        sortOrder: String?
    ): Cursor = withReadPath(DocumentProviderPaths.candidate(baseDirectory, requireNotNull(parentDocumentId))) { parent, guarded ->
        var cursor = MatrixCursor(projection ?: DEFAULT_DOCUMENT_PROJECTION)

        for (file in parent.listFiles().orEmpty()) {
            val canonical = DocumentProviderPaths.contained(baseDirectory, file)
            if (!guarded && requiresReadLease(canonical)) {
                throw FileNotFoundException("Saves are in use; this source includes protected save/profile data.")
            }
            cursor = includeFile(cursor, null, canonical)
        }
        cursor
    }

    override fun openDocument(
        documentId: String?,
        mode: String?,
        signal: CancellationSignal?
    ): ParcelFileDescriptor {
        val accessMode = ParcelFileDescriptor.parseMode(mode)
        val candidate = DocumentProviderPaths.candidate(
            baseDirectory, documentId ?: throw FileNotFoundException("Missing document ID")
        )
        val readOnly = accessMode and ParcelFileDescriptor.MODE_READ_WRITE == ParcelFileDescriptor.MODE_READ_ONLY
        var unguarded: DocumentProxyFile? = null
        var openedIdentity: StructStat? = null
        if (readOnly) {
            lockSourceReadsForDocument()
            try {
                signal?.throwIfCanceled()
                if (!requiresReadLease(candidate)) {
                    unguarded = DocumentProxyFile.open(candidate, requireNotNull(mode), null)
                    openedIdentity = Os.fstat(unguarded.descriptor())
                }
            } catch (e: Throwable) {
                unguarded?.close()
                throw e
            } finally {
                NativeSaveSession.unlockSourceReads()
            }
        }
        val sourceIdentity = openedIdentity
        val lease = try {
            if (unguarded == null) NativeSaveSession.acquireDetached(requireNotNull(context)) else null
        } catch (e: IllegalStateException) {
            throw FileNotFoundException(e.message).apply { initCause(e) }
        }
        val backend = unguarded ?: try {
            signal?.throwIfCanceled()
            val file = if (lease == null) candidate else getFile(requireNotNull(documentId))
            DocumentProxyFile.open(file, requireNotNull(mode), lease)
        } catch (e: Throwable) {
            lease?.close()
            throw e
        }
        fun release() {
            try {
                backend.close()
            } catch (e: Exception) {
                android.util.Log.w("DocumentProvider", "Cannot close mediated document", e)
            }
        }
        try {
            signal?.setOnCancelListener { release() }
            signal?.throwIfCanceled()
            val source = if (lease == null) candidate else null
            // Unleased source identity was captured under the independent read-side boundary lock.
            val callback = object : ProxyFileDescriptorCallback() {
                override fun onGetSize(): Long = proxyIo("getSize", signal, source, sourceIdentity) { backend.size() }
                override fun onRead(offset: Long, size: Int, data: ByteArray): Int =
                    proxyIo("read", signal, source, sourceIdentity) { backend.read(offset, size, data) }
                override fun onWrite(offset: Long, size: Int, data: ByteArray): Int =
                    proxyIo("write", signal, source, sourceIdentity) { backend.write(offset, size, data) }
                override fun onFsync() = proxyIo("fsync", signal, source, sourceIdentity) { backend.fsync() }
                override fun onRelease() {
                    signal?.setOnCancelListener(null)
                    release()
                }
            }
            // Never export a canonical file descriptor: a client duplicate must not outlive our lease.
            val storage = requireNotNull(context?.getSystemService(StorageManager::class.java))
            val descriptor = storage.openProxyFileDescriptor(
                accessMode and ParcelFileDescriptor.MODE_READ_WRITE, callback, proxyHandler
            )
            try {
                if (accessMode and ParcelFileDescriptor.MODE_APPEND != 0) {
                    Os.fcntlInt(
                        descriptor.fileDescriptor,
                        OsConstants.F_SETFL,
                        Os.fcntlInt(descriptor.fileDescriptor, OsConstants.F_GETFL, 0) or OsConstants.O_APPEND
                    )
                }
                signal?.throwIfCanceled()
            } catch (e: Throwable) {
                descriptor.close()
                throw e
            }
            return descriptor
        } catch (e: Throwable) {
            signal?.setOnCancelListener(null)
            release()
            throw e
        }
    }
}
