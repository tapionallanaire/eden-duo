// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

/**
 * Eden Duo: the build label of the in-game overlay. A release is named by the app's name and
 * version ("Duet Plus 1.1.2": app_name and the Android versionName, both set in duo.gradle); any
 * other build keeps the commit id the overlay showed before, so a beta or dev build still says
 * which commit it is.
 */
object DuoBuildLabel {
    private val release = Regex("""\d+\.\d+\.\d+(\.\d+)?""")

    /**
     * [appName]: the app's display name; [versionName]: the app's versionName; [buildVersion]:
     * the native build version ("<10-char commit>-<branch>", or a tag). The overlay shows the
     * first dash-separated part of [buildVersion] for non-releases, as upstream does.
     */
    fun overlay(appName: String, versionName: String, buildVersion: String): String =
        if (release.matches(versionName)) {
            "$appName $versionName"
        } else {
            buildVersion.split("-").getOrNull(0) ?: ""
        }
}
