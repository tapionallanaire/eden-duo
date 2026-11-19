// SPDX-License-Identifier: GPL-3.0-or-later
package org.yuzu.yuzu_emu.features

import java.io.Closeable
import java.io.File
import java.nio.channels.ClosedChannelException
import java.nio.channels.NonReadableChannelException
import java.nio.channels.NonWritableChannelException
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class DocumentProxyFileTest {
    @get:Rule val temporary = TemporaryFolder()

    @Test fun randomAccessPreservesOffsetsAndReturnsOnlyBytesBeforeEof() {
        val file = temporary.newFile().apply { writeText("abcdef") }
        DocumentProxyFile.open(file, "rw", Closeable {}).use { backend ->
            assertEquals(2, backend.write(2, 2, "XYunused".toByteArray()))
            val data = ByteArray(5) { '!'.code.toByte() }
            assertEquals(3, backend.read(3, 5, data))
            assertEquals("Yef!!", data.decodeToString())
            assertEquals(0, backend.read(100, 5, data))
            assertEquals(0, backend.read(0, 0, data))
            assertEquals(6L, backend.size())
            backend.fsync()
        }
        assertEquals("abXYef", file.readText())
    }

    @Test fun truncationAndCreationMatchDocumentModes() {
        for (mode in listOf("w", "wt", "rwt")) {
            val file = temporary.newFile().apply { writeText("old progress") }
            DocumentProxyFile.open(file, mode, Closeable {}).use { backend ->
                assertEquals(0L, backend.size())
                assertEquals(3, backend.write(0, 3, "new".toByteArray()))
                backend.fsync()
            }
            assertEquals("new", file.readText())
        }
        val created = File(temporary.root, "created")
        DocumentProxyFile.open(created, "w", Closeable {}).use { it.write(0, 1, byteArrayOf(7)) }
        assertEquals(1L, created.length())
    }

    @Test fun appendNeverOverwritesEvenWhenTheClientSeeksBackwards() {
        val file = temporary.newFile().apply { writeText("old") }
        DocumentProxyFile.open(file, "wa", Closeable {}).use { backend ->
            backend.write(0, 1, "A".toByteArray())
            backend.write(1, 1, "B".toByteArray())
            backend.fsync()
            assertEquals(5L, backend.size())
        }
        assertEquals("oldAB", file.readText())
    }

    @Test fun permissionsAndInvalidRangesCannotChangeTheFile() {
        val file = temporary.newFile().apply { writeText("safe") }
        DocumentProxyFile.open(file, "r", null).use { backend ->
            val read = ByteArray(4)
            assertEquals(4, backend.read(0, 4, read))
            assertEquals("safe", read.decodeToString())
            try {
                backend.write(0, 1, byteArrayOf(1))
                fail("Read-only descriptor accepted a write")
            } catch (_: NonWritableChannelException) { }
        }
        DocumentProxyFile.open(file, "wa", Closeable {}).use { backend ->
            try {
                backend.read(0, 1, ByteArray(1))
                fail("Write-only descriptor accepted a read")
            } catch (_: NonReadableChannelException) { }
            for ((offset, size) in listOf(-1L to 1, 0L to -1, 0L to 2)) {
                try {
                    backend.write(offset, size, byteArrayOf(1))
                    fail("Invalid byte range was accepted")
                } catch (_: IllegalArgumentException) { }
            }
        }
        DocumentProxyFile.open(file, "rw", Closeable {}).use { backend ->
            try {
                backend.write(Long.MAX_VALUE, 1, byteArrayOf(1))
                fail("Overflowing byte range was accepted")
            } catch (_: IllegalArgumentException) { }
        }
        assertEquals("safe", file.readText())
    }

    @Test fun releaseClosesTheFileBeforeReleasingOwnershipAndPreventsFurtherIo() {
        val file = temporary.newFile().apply { writeText("safe") }
        val released = File(temporary.root, "released")
        lateinit var backend: DocumentProxyFile
        lateinit var descriptor: java.io.FileDescriptor
        backend = DocumentProxyFile.open(file, "rw", Closeable {
            assertFalse("The canonical descriptor must close before ownership is released", descriptor.valid())
            try {
                backend.write(0, 1, byteArrayOf(1))
                fail("File access remained live after ownership was released")
            } catch (_: ClosedChannelException) { }
            released.appendText("released")
        })
        descriptor = backend.descriptor()
        backend.close()
        backend.close()
        assertEquals("released", released.readText())
        try {
            backend.read(0, 1, ByteArray(1))
            fail("Released file accepted a read")
        } catch (_: ClosedChannelException) { }
        try {
            backend.fsync()
            fail("Released file accepted fsync")
        } catch (_: ClosedChannelException) { }
        assertEquals("safe", file.readText())
        assertTrue(file.delete())
    }
}
