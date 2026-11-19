// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.utils

import kotlinx.serialization.Serializable

/** Title selection for a complete external-content listing. Called under the provider lock. */
internal object ExternalContentBootRules {
    data class ContainerStamp(val uri: String, val size: Long, val modified: Long)

    /** ExternalStorageProvider's volume root ends in ':', directory descendants use '/'. */
    fun inDocumentRoot(documentId: String, rootId: String): Boolean {
        val root = rootId.trimEnd('/')
        return documentId == root ||
            documentId.startsWith(if (root.endsWith(':')) root else "$root/")
    }

    /** Scan bookkeeping is best-effort; an unsuccessful attempt must remain retryable. */
    fun mountContainer(
        uri: String,
        mounted: MutableSet<String>,
        register: () -> Boolean
    ): Boolean {
        if (uri in mounted) return true
        if (!register()) return false
        mounted.add(uri)
        return true
    }

    @Serializable
    data class ContainerTitles(
        val size: Long,
        val modified: Long,
        val titles: List<Long>,
        val records: List<ContentRecord> = emptyList()
    )

    /** JNI kind: 0 = application, 1 = versioned update, 2 = DLC. */
    @Serializable
    data class ContentRecord(val baseTitleId: Long, val kind: Int, val version: Long? = null)

    /**
     * Empty records in pre-metadata caches mean unknown, not disabled. Such selected-title
     * containers stay required even with Update/DLC disabled until a readable observation
     * establishes their kind/version. We cannot infer those from a base title id alone.
     */
    @Serializable
    data class ContentContainer(
        val uri: String,
        val gameFolder: Boolean,
        val records: List<ContentRecord> = emptyList(),
        val external: Boolean = !gameFolder
    )

    /** Only a readable observation can replace a path's previous title/kind/version evidence. */
    fun rememberContainer(
        known: Map<Long, List<ContentContainer>>,
        uri: String,
        gameFolder: Boolean,
        external: Boolean,
        records: List<ContentRecord>
    ): Map<Long, List<ContentContainer>> {
        if (records.isEmpty()) return known
        val remembered = known.mapValues { (_, entries) ->
            entries.filter { it.uri != uri }
        }.toMutableMap()
        records.map { it.baseTitleId }.distinct().forEach { title ->
            remembered[title] = remembered[title].orEmpty() +
                ContentContainer(uri, gameFolder, records, external)
        }
        return remembered
    }

    /** Snapshot before discovery; never let a listing or a warm-provider flag waive known input. */
    fun requiredContainers(
        baseTitleId: Long,
        gamePath: String,
        known: List<ContentContainer>,
        external: Map<String, ContainerTitles>,
        disabled: Set<String>,
        inExternalRoots: (String) -> Boolean
    ): List<ContentContainer> {
        val candidates = known.filter { !it.external || inExternalRoots(it.uri) } +
            external.filter { baseTitleId in it.value.titles && inExternalRoots(it.key) }
                .map { ContentContainer(it.key, false, it.value.records) }
        return candidates.filter { it.uri != gamePath }.groupBy { it.uri }.map { (uri, entries) ->
            ContentContainer(uri, entries.all { it.gameFolder },
                entries.flatMap { it.records }.distinct(), entries.all { it.external })
        }.filter { container ->
            container.records.isEmpty() || container.records.any {
                it.baseTitleId == baseTitleId && when (it.kind) {
                    0 -> false
                    1 -> "Update" !in disabled &&
                        (it.version == null || "Update@${it.version}" !in disabled)
                    2 -> "DLC" !in disabled
                    else -> true
                }
            }
        }
    }

    /** Reopens even warm entries. The native game-folder gate intentionally returns true/no-op. */
    fun registerRequired(
        required: List<ContentContainer>,
        register: (ContentContainer) -> Boolean
    ): Boolean {
        var complete = true
        required.forEach { if (!register(it)) complete = false }
        return complete
    }

    data class Selection(
        val remembered: Map<String, ContainerTitles>,
        val matches: List<String>,
        val readCount: Int
    )

    fun select(
        baseTitleId: Long,
        stamps: List<ContainerStamp>,
        stored: Map<String, ContainerTitles>,
        readTitles: (String) -> List<Long>
    ): Selection {
        // A missing/unreadable path is still evidence for Prepare on the next cold process.
        // Scope to currently configured roots at use time, not by erasing failed observations.
        val remembered = stored.toMutableMap()
        val matches = mutableListOf<String>()
        var readCount = 0
        for (stamp in stamps.distinctBy { it.uri }) {
            val previous = stored[stamp.uri]
            // Without both stamps, a same-size replacement or a timestamp collision is unsafe.
            val stamped = stamp.size > 0L && stamp.modified > 0L
            val entry = if (stamped && previous != null && previous.size == stamp.size &&
                previous.modified == stamp.modified
            ) {
                previous
            } else {
                readCount++
                ContainerTitles(stamp.size, stamp.modified, readTitles(stamp.uri).filter {
                    it != 0L
                }.distinct())
            }
            // Missing keys or unreadable containers must be retried on the next boot.
            if (stamped && entry.titles.isNotEmpty()) {
                remembered[stamp.uri] = entry
            }
            if (baseTitleId in entry.titles) {
                matches.add(stamp.uri)
            }
        }
        return Selection(remembered, matches, readCount)
    }
}
