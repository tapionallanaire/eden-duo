// SPDX-License-Identifier: GPL-3.0-or-later
package org.yuzu.yuzu_emu.duo.nativeplay

import java.io.File
import java.io.IOException
import java.math.BigInteger
import java.nio.file.DirectoryIteratorException
import java.nio.file.Files
import java.nio.file.LinkOption.NOFOLLOW_LINKS
import java.nio.file.NoSuchFileException
import java.nio.file.attribute.BasicFileAttributes

/** Pure trust-boundary rules, shared by parent admission, child admission and durable recovery. */
internal object NativeSaveBindingRules {
    fun requireIdentity(captured: String, current: String) {
        check(captured == current) { "The selected account or save location changed. Open this action again." }
    }

    fun requireUserWords(folderId: String, words: List<String>) {
        require(words.size == 2 && words.all { Regex("[0-9a-f]{16}").matches(it) } &&
            folderId == (words[1] + words[0]).uppercase()) { "Selected save account UID does not match its folder identity" }
    }

    fun requirePart(autoCreate: Boolean, size: BigInteger, journal: BigInteger, path: String?) {
        require(size.signum() >= 0 && journal.signum() >= 0)
        require((path != null) == (autoCreate || size.signum() > 0 || journal.signum() > 0)) {
            "Save path does not match its declaration or on-demand creation policy"
        }
    }

    fun requireOfflineBcat(engine: NativeEngine, kinds: List<String>, nand: File, programId: String) {
        require(engine == NativeEngine.PEANUTS && kinds == listOf("bcat")) {
            "This title declares unsupported native save kinds: ${kinds.joinToString()}"
        }
        require(Regex("[0-9A-F]{16}").matches(programId)) { "Invalid BCAT program identity" }
        try {
            require(nand.isAbsolute && nand.canonicalFile == nand && nand.isDirectory) {
                "Cannot inspect offline BCAT: NAND must be a physical directory without symlinks"
            }
            // BISFactory::GetBCATDirectory creates the directory. Inspect that path without opening it via VFS.
            var path = nand.toPath()
            for (component in listOf("system", "save", "bcat", programId)) {
                path = path.resolve(component)
                val attrs = try {
                    Files.readAttributes(path, BasicFileAttributes::class.java, NOFOLLOW_LINKS)
                } catch (_: NoSuchFileException) { return }
                require(attrs.isDirectory) { "Cannot use offline BCAT: $path is not a physical directory" }
            }
            Files.newDirectoryStream(path).use {
                require(!it.iterator().hasNext()) { "Native play is offline; use emulation to keep using this game's existing BCAT delivery data" }
            }
        } catch (e: IOException) {
            throw IllegalStateException("Cannot inspect this game's offline BCAT delivery cache", e)
        } catch (e: DirectoryIteratorException) {
            throw IllegalStateException("Cannot inspect this game's offline BCAT delivery cache", e)
        }
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
