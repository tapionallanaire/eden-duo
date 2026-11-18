// SPDX-FileCopyrightText: Copyright 2026 Duet Plus contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.nativeplay

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class NativePrepareIdentityTest {
    private val fields = arrayOf(
        "0100F7E00C70E000", "327680", "a".repeat(64), "12345",
        "main=${"1".repeat(64)}", "sdk=${"2".repeat(64)}", "subsdk0=${"3".repeat(64)}"
    )

    @Test
    fun retainsFullObservedModuleMapAndContentIdentity() {
        val identity = NativeGameSupport.parseIdentity(fields)!!
        assertEquals(3, identity.build.moduleBuildIds.size)
        assertEquals("3".repeat(64), identity.build.moduleBuildIds["subsdk0"])
        assertEquals("a".repeat(64), identity.contentKey)
        assertEquals(12345L, identity.inputBytes)
    }

    @Test
    fun rejectsTruncatedDuplicateOrUnversionedIdentity() {
        assertNull(NativeGameSupport.parseIdentity(fields.copyOfRange(0, 4)))
        assertNull(NativeGameSupport.parseIdentity(fields + fields[4]))
        assertNull(NativeGameSupport.parseIdentity(fields.copyOf().also { it[1] = "-1" }))
        assertNull(NativeGameSupport.parseIdentity(fields.copyOf().also { it[4] = "main=1234" }))
    }
}
