// SPDX-License-Identifier: GPL-3.0-or-later
package org.yuzu.yuzu_emu.duo.nativeplay

import java.io.File

/** Pure trust-boundary rules, shared by parent admission, child admission and durable recovery. */
internal object NativeSaveBindingRules {
    fun requireIdentity(captured: String, current: String) {
        check(captured == current) { "The selected account or save location changed. Open this action again." }
    }

    fun requireUserWords(folderId: String, words: List<String>) {
        require(words.size == 2 && words.all { Regex("[0-9a-f]{16}").matches(it) } &&
            folderId == (words[1] + words[0]).uppercase()) { "Selected save account UID does not match its folder identity" }
    }

    fun requirePhysicalParts(rootPath: String, paths: List<String>) {
        val root = File(rootPath)
        require(root.isAbsolute && root.canonicalPath == root.path) { "Save root is not a physical canonical path" }
        paths.forEach { path ->
            val file = File(path)
            require(file.isAbsolute && file.canonicalPath == file.path &&
                file.path.startsWith(root.path + File.separator)) { "Save part escapes its physical root" }
        }
        require(paths.distinct().size == paths.size && paths.indices.all { i ->
            paths.indices.all { j -> i == j || !paths[i].startsWith(paths[j] + File.separator) }
        }) { "Aliased save parts" }
    }
}
