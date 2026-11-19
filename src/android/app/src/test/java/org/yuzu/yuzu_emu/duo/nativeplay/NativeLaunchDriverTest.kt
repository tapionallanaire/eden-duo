// SPDX-FileCopyrightText: Copyright 2026 Duet Plus contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.nativeplay

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

class NativeLaunchDriverTest {
    private fun metadata() = JSONObject("""{
        "name":"Turnip", "description":"", "author":"Mesa", "vendor":"Qualcomm",
        "driverVersion":"1", "libraryName":"libvulkan.so", "minApi":28
    }""")

    @Test
    fun emptyDescriptionIsValidButOtherRequiredStringsStayNonempty() {
        assertEquals("libvulkan.so", NativeLaunchDriver.validateMetadata(metadata(), 35))
        for (key in listOf("name", "author", "vendor", "driverVersion", "libraryName")) {
            assertThrows(IllegalArgumentException::class.java) {
                NativeLaunchDriver.validateMetadata(metadata().put(key, ""), 35)
            }
        }
        assertThrows(IllegalArgumentException::class.java) {
            NativeLaunchDriver.validateMetadata(metadata().put("description", 1), 35)
        }
    }

    @Test
    fun minApiUsesTheBridgesIntegerOnlyPolicy() {
        assertEquals("libvulkan.so", NativeLaunchDriver.validateMetadata(metadata(), 28))
        for (value in listOf<Any>(0, -1, 36, 28.0, "28", 4294967324L, JSONObject.NULL)) {
            assertThrows(IllegalArgumentException::class.java) {
                NativeLaunchDriver.validateMetadata(metadata().put("minApi", value), 35)
            }
        }
    }

    @Test
    fun libraryMustRemainAnUnqualifiedSharedObject() {
        for (library in listOf(".", "..", "../lib.so", "dir/lib.so", "dir\\lib.so", "lib.txt")) {
            assertThrows(IllegalArgumentException::class.java) {
                NativeLaunchDriver.validateMetadata(metadata().put("libraryName", library), 35)
            }
        }
    }
}
