// SPDX-FileCopyrightText: Copyright 2026 Duet Plus contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.nativeplay

import android.content.Context

internal object NativeGameSupport {
    /** Reads the program packaged in a NUP. The launch coordinator must resolve add-ons separately. */
    fun findPackagedBuild(context: Context, path: String, programIndex: Int): NativeGameProfile? {
        val fields = readNupBuild(path, programIndex) ?: return null
        val version = fields[1].toLong()
        val modules = fields.drop(2).associate { field ->
            val pair = field.split('=', limit = 2)
            pair[0] to pair[1]
        }
        val catalog = context.assets.open(NativeGameCatalog.ASSET).reader()
            .use(NativeGameCatalog::read)
        return catalog.match(NativeGameBuild(fields[0], version, modules))
    }

    private external fun readNupBuild(path: String, programIndex: Int): Array<String>?
}
