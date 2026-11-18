// SPDX-FileCopyrightText: Copyright 2026 Duet Plus contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.nativeplay

import android.content.Context

internal object NativeGameSupport {
    data class Match(
        val profile: NativeGameProfile,
        val supportedVersions: List<NativeGameProfile>
    )

    /** Checks the selected executable and update. Native launch must also validate mods and DLC. */
    fun findBuildCandidate(context: Context, path: String, programIndex: Int): Match? {
        val fields = readGameBuild(path, programIndex) ?: return null
        val version = fields[1].toLong()
        val modules = fields.drop(2).associate { field ->
            val pair = field.split('=', limit = 2)
            pair[0] to pair[1]
        }
        val catalog = context.assets.open(NativeGameCatalog.ASSET).reader()
            .use(NativeGameCatalog::read)
        val profile = catalog.match(NativeGameBuild(fields[0], version, modules)) ?: return null
        return Match(profile, catalog.versions(profile.programId))
    }

    private external fun readGameBuild(path: String, programIndex: Int): Array<String>?
}
