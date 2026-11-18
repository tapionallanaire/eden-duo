// SPDX-License-Identifier: GPL-3.0-or-later
package org.yuzu.yuzu_emu.features

import java.io.Closeable
import java.io.File
import java.io.FileDescriptor
import java.io.RandomAccessFile
import java.nio.channels.ClosedChannelException
import java.nio.channels.NonReadableChannelException
import java.nio.channels.NonWritableChannelException

/** The canonical file stays in the provider process; callers receive only an AppFuse proxy. */
internal class DocumentProxyFile private constructor(
    private val file: RandomAccessFile,
    private val readable: Boolean,
    private val writable: Boolean,
    private val append: Boolean,
    private val lease: Closeable?
) : Closeable {
    private var closed = false

    private fun requireOpen() {
        if (closed) throw ClosedChannelException()
    }

    @Synchronized fun descriptor(): FileDescriptor {
        requireOpen()
        return file.fd
    }

    private fun requireRange(offset: Long, size: Int, data: ByteArray) {
        require(offset >= 0 && size >= 0 && size <= data.size) { "Invalid document byte range" }
    }

    @Synchronized fun size(): Long {
        requireOpen()
        return file.length()
    }

    @Synchronized fun read(offset: Long, size: Int, data: ByteArray): Int {
        requireOpen()
        if (!readable) throw NonReadableChannelException()
        requireRange(offset, size, data)
        file.seek(offset)
        var count = 0
        while (count < size) {
            val next = file.read(data, count, size - count)
            if (next < 0) break
            count += next
        }
        return count
    }

    @Synchronized fun write(offset: Long, size: Int, data: ByteArray): Int {
        requireOpen()
        if (!writable) throw NonWritableChannelException()
        requireRange(offset, size, data)
        val position = if (append) file.length() else offset
        require(position <= Long.MAX_VALUE - size) { "Document byte range overflows" }
        file.seek(position)
        file.write(data, 0, size)
        return size
    }

    @Synchronized fun fsync() {
        requireOpen()
        file.fd.sync()
    }

    @Synchronized override fun close() {
        if (closed) return
        closed = true
        try {
            file.close()
        } finally {
            lease?.close()
        }
    }

    companion object {
        /** Ownership of [lease] transfers only after this returns successfully. */
        fun open(path: File, mode: String, lease: Closeable?): DocumentProxyFile {
            require(mode == "r" || mode == "w" || mode == "wt" || mode == "wa" || mode == "rw" || mode == "rwt") {
                "Unsupported document mode: $mode"
            }
            val writable = mode != "r"
            val file = RandomAccessFile(path, if (writable) "rw" else "r")
            try {
                if (mode == "w" || mode == "wt" || mode == "rwt") file.setLength(0)
                return DocumentProxyFile(
                    file,
                    mode == "r" || mode == "rw" || mode == "rwt",
                    writable,
                    mode == "wa",
                    lease
                )
            } catch (e: Throwable) {
                try {
                    file.close()
                } catch (closeError: Throwable) {
                    e.addSuppressed(closeError)
                }
                throw e
            }
        }
    }
}
