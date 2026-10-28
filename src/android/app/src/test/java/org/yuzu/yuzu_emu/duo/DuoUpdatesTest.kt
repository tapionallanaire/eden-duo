// SPDX-FileCopyrightText: Copyright 2026 tapionallanaire
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class DuoUpdatesTest {
    private fun release(tag: String, apk: String? = "https://example/app-mainline-release.apk") =
        """
        {
          "tag_name": "$tag",
          "name": "Duet Plus $tag",
          "body": "notes",
          "html_url": "https://github.com/tapionallanaire/duet-plus/releases/tag/$tag",
          "assets": [${if (apk == null) "" else """{"name": "app-mainline-release.apk", "browser_download_url": "$apk"}"""}]
        }
        """.trimIndent()

    @Test fun newerReleaseIsOffered() {
        val update = DuoUpdates.parse(release("v1.1.2"), "1.1.1")
        assertEquals("v1.1.2", update?.tag)
        assertEquals(listOf("https://example/app-mainline-release.apk"), update?.assets)
    }

    @Test fun theInstalledVersionIsNotAnUpdate() {
        // "v" or not, and a dev build ahead of the last release, must not offer anything.
        assertNull(DuoUpdates.parse(release("v1.1.1"), "1.1.1"))
        assertNull(DuoUpdates.parse(release("v1.1.1"), "1.1.2"))
        assertNull(DuoUpdates.parse(release("v1.1.1"), "1.2"))
    }

    @Test fun aReleaseWithoutAnApkIsNotOffered() {
        assertNull(DuoUpdates.parse(release("v1.1.2", apk = null), "1.1.1"))
    }

    @Test fun versionsCompareNumericallyNotAsText() {
        assertTrue(DuoUpdates.isNewer("1.1.10", "1.1.9"))
        assertTrue(DuoUpdates.isNewer("v2.0.0", "1.9.9"))
        assertTrue(DuoUpdates.isNewer("1.2", "1.1.9"))
        assertFalse(DuoUpdates.isNewer("1.1.9", "1.1.10"))
        assertFalse(DuoUpdates.isNewer("1.1.1", "1.1.1"))
    }
}
