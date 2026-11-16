// SPDX-FileCopyrightText: Copyright 2026 tapionallanaire
// SPDX-License-Identifier: GPL-3.0-or-later
// Steam-style save reconciliation, independent of Android so it can be checked on a desktop JVM.
// The same file lives in Calamansi (org.calamansi.sync, GPL-2.0-or-later there); keep the two copies identical
// apart from the licence header and the package line.

package org.yuzu.yuzu_emu.duo.sync

import java.io.File
import java.security.MessageDigest

/** What to do with one save, given its hash here, in the synced folder, and at the last sync (null = absent). */
enum class Action { NONE, PULL, PUSH, CONFLICT }

fun decide(local: String?, remote: String?, base: String?): Action = when {
    local == remote -> Action.NONE
    // Never let a deletion travel: a save missing on one side is filled from the other.
    remote == null -> Action.PUSH
    local == null -> Action.PULL
    local == base -> Action.PULL
    remote == base -> Action.PUSH
    else -> Action.CONFLICT
}

/** Syncthing's own files and half-written temporaries; never part of a save. */
fun isSyncthingFile(name: String): Boolean =
    name.startsWith(".st") || name.startsWith("~syncthing~") || name.startsWith(".syncthing.") ||
        name.contains(".sync-conflict-")

private fun saveFiles(unit: File): List<File> =
    if (unit.isFile) listOf(unit)
    else unit.walkTopDown().onEnter { !isSyncthingFile(it.name) || it == unit }
        .filter { it.isFile && !isSyncthingFile(it.name) }.sortedBy { it.path }.toList()

/** Cheap change marker: file count, total size, newest mtime. Equal stamps reuse the cached hash. */
fun stamp(unit: File): String? {
    if (!unit.exists()) return null
    val files = saveFiles(unit)
    return "${files.size}:${files.sumOf { it.length() }}:${files.maxOfOrNull { it.lastModified() } ?: 0}"
}

/**
 * Content hash over relative paths and bytes (not mtimes); null when the save is absent or holds no bytes (melonDS
 * creates an empty .sav before a game first writes one).
 */
fun hash(unit: File): String? {
    val files = saveFiles(unit)
    if (files.all { it.length() == 0L }) return null
    val md = MessageDigest.getInstance("SHA-256")
    for (f in files) {
        md.update(f.relativeTo(unit).invariantSeparatorsPath.toByteArray())
        md.update(0)
        f.inputStream().use { input ->
            val buf = ByteArray(64 * 1024)
            while (true) {
                val n = input.read(buf)
                if (n < 0) break
                md.update(buf, 0, n)
            }
        }
        md.update(0)
    }
    return md.digest().joinToString("") { "%02x".format(it) }
}

/**
 * Replaces [to] with a copy of [from] (a file or a folder), staging the copy next to [to] first so a failed copy
 * never leaves half a save behind.
 */
fun replaceSave(from: File, to: File) {
    to.parentFile?.mkdirs()
    val incoming = File(to.path + ".incoming")
    val old = File(to.path + ".old")
    incoming.deleteRecursively()
    old.deleteRecursively()
    if (from.isFile) {
        from.copyTo(incoming, overwrite = true)
    } else {
        incoming.mkdirs()
        for (f in saveFiles(from)) f.copyTo(File(incoming, f.relativeTo(from).path), overwrite = true)
    }
    if (to.exists() && !to.renameTo(old)) error("cannot move $to aside")
    if (!incoming.renameTo(to)) {
        old.renameTo(to)
        error("cannot move $incoming into place")
    }
    old.deleteRecursively()
}

/**
 * Save units under [root], [depth] levels down (Switch title folders are depth 1; 3DS high/low title pairs are 2).
 * Keys are relative paths with '/', identical between a device's save folder and the synced folder.
 */
fun listUnits(root: File, depth: Int): List<String> {
    var level = listOf("")
    repeat(depth) {
        level = level.flatMap { rel ->
            File(root, rel).listFiles().orEmpty()
                .filter { !isSyncthingFile(it.name) && !it.name.endsWith(".incoming") && !it.name.endsWith(".old") }
                .map { if (rel.isEmpty()) it.name else "$rel/${it.name}" }
        }
    }
    return level
}

/** Units below [root] (see [listUnits]) plus those only the synced folder has, mapped to where they go here. */
fun savesUnder(root: File?, depth: Int, payload: String = ""): (List<String>) -> Map<String, File> = { remote ->
    if (root == null) emptyMap()
    else (listUnits(root, depth) + remote).distinct().associateWith { File(root, it + payload) }
}

/** A device-independent save name from a ROM file name, safe on Windows (the usual hub). */
fun saveKey(romFileName: String, extension: String): String =
    romFileName.substringBeforeLast('.').replace(Regex("[<>:\"/\\\\|?*\\x00-\\x1f]"), "_").trimEnd('.', ' ') +
        ".$extension"

/** What reconciling reads and records per unit of one console; SaveSync keeps it in SQLite. */
interface SyncState {
    /** Hash of a save; `side` is "local" or "remote", so each side's hash can be cached. */
    fun hash(unit: String, side: String, file: File): String?
    fun base(unit: String): String?
    fun synced(unit: String, hash: String, state: String)
    fun conflict(unit: String)
}

/**
 * Reconciles one console: [units] maps each unit to where it lives on this device, [remoteRoot] is the console's
 * part of the synced folder, pulls back up the replaced save under [backups]/<unit>. Returns the units both sides
 * changed (left for the user) and whether anything was pushed.
 */
fun reconcileUnits(
    units: Map<String, File>,
    remoteRoot: File,
    backups: File,
    state: SyncState,
    log: (String) -> Unit = {}
): Pair<List<String>, Boolean> {
    val conflicts = mutableListOf<String>()
    var pushed = false
    for ((unit, local) in units) try {
        val remote = File(remoteRoot, unit)
        val l = state.hash(unit, "local", local)
        val r = state.hash(unit, "remote", remote)
        when (decide(l, r, state.base(unit))) {
            Action.NONE -> if (l != null) state.synced(unit, l, "synced")
            Action.PULL -> {
                backupSave(local, File(backups, unit))
                replaceSave(remote, local)
                state.synced(unit, r!!, "pulled")
                log("pulled $unit")
            }
            Action.PUSH -> {
                replaceSave(local, remote)
                state.synced(unit, l!!, "pushed")
                pushed = true
                log("pushed $unit")
            }
            Action.CONFLICT -> {
                state.conflict(unit)
                conflicts += unit
                log("conflict $unit")
            }
        }
    } catch (e: Exception) {
        // One unreadable or unwritable save must not stop the others; it is retried at the next sync.
        log("failed $unit: $e")
    }
    return conflicts to pushed
}

/** The user's answer to a conflict: keep this device's save (push it) or take the synced one (pull it). */
fun resolveConflict(unit: String, local: File, remote: File, backups: File, keepLocal: Boolean, state: SyncState) {
    if (keepLocal) {
        replaceSave(local, remote)
        state.synced(unit, hash(local)!!, "pushed")
    } else {
        backupSave(local, File(backups, unit))
        replaceSave(remote, local)
        state.synced(unit, hash(remote)!!, "pulled")
    }
}

/** Keeps the newest [keep] copies of [save] under [dir] before it is overwritten by a pull. */
fun backupSave(save: File, dir: File, keep: Int = 3) {
    if (!save.exists()) return
    val target = File(dir, System.currentTimeMillis().toString())
    if (save.isFile) save.copyTo(File(target, save.name)) else save.copyRecursively(target)
    dir.listFiles().orEmpty().sortedByDescending { it.name }.drop(keep).forEach { it.deleteRecursively() }
}
