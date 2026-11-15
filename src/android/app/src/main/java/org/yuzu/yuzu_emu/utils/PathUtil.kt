// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.utils

import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.DocumentsContract
import java.io.File

object PathUtil {

    /**
     * The save folder as a file path. A folder linked in the Eden import is stored as a content://
     * tree URI, which native code opens through SAF but java.io.File can't; it maps to its mount
     * here, which takes All files access ([needsAllFilesAccess]) to read and write.
     */
    fun saveDir(): String {
        val dir = NativeConfig.getSaveDir()
        if (!dir.startsWith("content://")) return dir
        return getPathFromUri(Uri.parse(dir)) ?: dir
    }

    /** True while the save folder is outside app storage and the grant to reach it is missing. */
    fun needsAllFilesAccess(): Boolean =
        NativeConfig.getSaveDir().startsWith("content://") &&
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && !Environment.isExternalStorageManager()

    /**
     * Converts a content:// URI from the Storage Access Framework to a real filesystem path.
     */
    fun getPathFromUri(uri: Uri): String? {
        val docId = try {
            DocumentsContract.getTreeDocumentId(uri)
        } catch (_: Exception) {
            return null
        }

        if (docId.startsWith("primary:")) {
            val relativePath = docId.substringAfter(":")
            val primaryStoragePath = android.os.Environment.getExternalStorageDirectory().absolutePath
            return "$primaryStoragePath/$relativePath"
        }

        // external SD cards and other volumes)
        val storageIdString = docId.substringBefore(":")
        val removablePath = getRemovableStoragePath(storageIdString)
        if (removablePath != null) {
            return "$removablePath/${docId.substringAfter(":")}"
        }

        return null
    }

    /**
     * Validates that a path is a valid, writable directory.
     * Creates the directory if it doesn't exist.
     */
    fun validateDirectory(path: String): Boolean {
        val dir = File(path)

        if (!dir.exists()) {
            if (!dir.mkdirs()) {
                return false
            }
        }

        return dir.isDirectory && dir.canWrite()
    }

    /**
     * Copies a directory recursively from source to destination.
     */
    fun copyDirectory(source: File, destination: File, overwrite: Boolean = true): Boolean {
        return try {
            source.copyRecursively(destination, overwrite)
            true
        } catch (_: Exception) {
            false
        }
    }

    /**
     * Checks if a directory has any content.
     */
    fun hasContent(path: String): Boolean {
        val dir = File(path)
        return dir.exists() && dir.listFiles()?.isNotEmpty() == true
    }


    fun truncatePathForDisplay(path: String, maxLength: Int = 40): String {
        return if (path.length > maxLength) {
            "...${path.takeLast(maxLength - 3)}"
        } else {
            path
        }
    }

    fun getRemovableStoragePath(idString: String): String? {
        val possibleMountPaths = listOf("/mnt/media_rw/$idString", "/storage/$idString")

        for (mountPath in possibleMountPaths) {
            val pathFile = File(mountPath);
            if (pathFile.exists()) {
                return pathFile.absolutePath
            }
        }

        return null
    }
}
