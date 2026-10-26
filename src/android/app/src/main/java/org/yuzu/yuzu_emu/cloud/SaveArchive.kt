// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.cloud

import java.io.File
import java.io.IOException
import java.security.MessageDigest
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

/**
 * Eden Duo: a save folder as bytes and as an identity.
 *
 * [hash] is what decides whether something changed, locally or in the cloud: two saves with the
 * same file names and contents hash the same, whatever their mtimes are, so a phone and a handheld
 * only disagree when the saves really differ. Times are kept out of the zip so the same save always
 * uploads the same bytes.
 */
object SaveArchive {
    /** Eden writes this into every save folder, even one the game never touched. */
    private const val SIZE_MARKER = ".yuzu_save_size"

    private const val FIXED_TIME = 946684800000L // 2000-01-01, so zips of one save match

    fun files(dir: File): List<File> =
        if (!dir.isDirectory) {
            emptyList()
        } else {
            dir.walkTopDown().filter { it.isFile }.sortedBy { relative(dir, it) }.toList()
        }

    /** True when the game has actually written something (the size marker alone does not count). */
    fun hasData(dir: File): Boolean =
        files(dir).any { it.name != SIZE_MARKER && it.length() > 0 }

    /** Content identity of the folder: name, size and bytes of every file, order-stable. */
    fun hash(dir: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        for (file in files(dir)) {
            digest.update(relative(dir, file).toByteArray())
            digest.update(0)
            digest.update(file.length().toString().toByteArray())
            digest.update(0)
            file.inputStream().use { input ->
                val buffer = ByteArray(64 * 1024)
                while (true) {
                    val read = input.read(buffer)
                    if (read <= 0) break
                    digest.update(buffer, 0, read)
                }
            }
        }
        return digest.digest().joinToString("") { "%02x".format(it) }
    }

    fun zip(dir: File): ByteArray {
        val out = java.io.ByteArrayOutputStream()
        ZipOutputStream(out).use { zip ->
            for (file in files(dir)) {
                val entry = ZipEntry(relative(dir, file)).apply { time = FIXED_TIME }
                zip.putNextEntry(entry)
                file.inputStream().use { it.copyTo(zip) }
                zip.closeEntry()
            }
        }
        return out.toByteArray()
    }

    /**
     * Replaces [dir]'s contents with the zip in [bytes]. Names that would escape the folder are
     * refused: a save from another device is still data from outside this app.
     */
    fun extract(bytes: ByteArray, dir: File) {
        val target = dir.canonicalFile
        if (!target.exists() && !target.mkdirs()) {
            throw IOException("Cannot create the save folder")
        }
        val incoming = sortedMapOf<String, ByteArray>()
        ZipInputStream(bytes.inputStream()).use { zip ->
            while (true) {
                val entry = zip.nextEntry ?: break
                if (entry.isDirectory) continue
                val name = entry.name
                if (name.startsWith("/") || name.split('/').any { it == ".." || it.isEmpty() }) {
                    throw IOException("Save archive has an invalid path: $name")
                }
                incoming[name] = zip.readBytes()
            }
        }
        // Only touch the folder once the whole archive has been read.
        files(target).forEach { it.delete() }
        for ((name, content) in incoming) {
            val file = File(target, name)
            if (!file.canonicalFile.startsWith(target)) {
                throw IOException("Save archive escapes the save folder: $name")
            }
            file.parentFile?.mkdirs()
            file.writeBytes(content)
        }
    }

    private fun relative(dir: File, file: File): String =
        file.absolutePath.removePrefix(dir.absolutePath).trimStart(File.separatorChar)
            .replace(File.separatorChar, '/')
}
