// SPDX-License-Identifier: GPL-3.0-or-later
package org.yuzu.yuzu_emu.features

import java.io.File
import java.io.FileNotFoundException

/** Canonical containment is checked before the provider exposes any user-tree file. */
internal object DocumentProviderPaths {
    const val ROOT_ID = "root"

    fun contained(root: File, file: File): File {
        val base = root.canonicalFile
        val target = file.canonicalFile
        if (target != base && !target.path.startsWith(base.path + File.separator)) {
            throw FileNotFoundException("The document is outside the user directory")
        }
        return target
    }

    fun candidate(root: File, documentId: String): File {
        if (documentId != ROOT_ID && !documentId.startsWith("$ROOT_ID/")) {
            throw FileNotFoundException("The document is not in the user directory")
        }
        val relative = if (documentId == ROOT_ID) "" else documentId.substring(ROOT_ID.length + 1)
        return contained(root, File(root, relative))
    }

    fun resolve(root: File, documentId: String): File {
        val file = candidate(root, documentId)
        if (!file.exists()) throw FileNotFoundException("The document does not exist")
        return file
    }

    fun documentId(root: File, file: File): String {
        val target = contained(root, file)
        val relative = target.relativeTo(root.canonicalFile).invariantSeparatorsPath
        return if (relative.isEmpty()) ROOT_ID else "$ROOT_ID/$relative"
    }

    fun child(parent: File, name: String): File {
        if (name.isEmpty() || name == "." || name == ".." || '/' in name || '\\' in name || '\u0000' in name) {
            throw FileNotFoundException("The document name must be a single file name")
        }
        return contained(parent, File(parent, name))
    }

    /** A null lock-free snapshot must never turn unknown saves into an unguarded source. */
    fun requiresSaveLease(root: File, file: File, protectedTrees: List<String>?): Boolean {
        if (protectedTrees == null) return true
        val path = contained(root, file).path
        fun overlaps(tree: String): Boolean =
            path == tree || path.startsWith(tree + File.separator) || tree.startsWith(path + File.separator)
        if (overlaps(File(root, "config").canonicalPath)) return true
        for (tree in protectedTrees) {
            if (overlaps(tree)) return true
            val parent = File(tree).parentFile ?: continue
            if (path.startsWith(parent.path + File.separator + "save.before-")) return true
        }
        return false
    }
}
