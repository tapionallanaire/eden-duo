// SPDX-FileCopyrightText: Copyright 2026 Duet Plus contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.nativeplay

import java.io.Reader

internal enum class NativeEngine {
    PEANUTS,
    BANANA
}

internal data class NativeGameBuild(
    val programId: String,
    val version: Long,
    val moduleBuildIds: Map<String, String>
)

internal data class NativeGameProfile(
    val engine: NativeEngine,
    val programId: String,
    val version: Long,
    val displayVersion: String,
    val title: String,
    val moduleBuildIds: Map<String, String>
)

internal class NativeGameCatalog private constructor(profiles: List<NativeGameProfile>) {
    private val byProgram = profiles.groupBy { it.programId }
    val programIndices = profiles.map { it.programId.takeLast(3).toInt(16) }.distinct().sorted()

    fun versions(programId: String): List<NativeGameProfile> =
        byProgram[programId.uppercase()].orEmpty()

    fun match(build: NativeGameBuild): NativeGameProfile? =
        versions(build.programId).singleOrNull { entry ->
            entry.version == build.version &&
                entry.moduleBuildIds.all { (name, expected) ->
                    val observed = build.moduleBuildIds[name]
                    observed != null && fullBuildId.matches(observed) &&
                        observed.startsWith(expected, ignoreCase = true)
                }
        }

    companion object {
        const val ASSET = "native_games.tsv"

        private const val MAX_VERSION = 0xFFFFFFFFL
        private val titleId = Regex("[0-9A-F]{16}")
        private val buildId = Regex("(?:[0-9a-fA-F]{32}){1,2}")
        private val fullBuildId = Regex("[0-9a-fA-F]{64}")

        fun read(reader: Reader): NativeGameCatalog {
            val profiles = reader.buffered().useLines { lines ->
                lines.filter { it.isNotBlank() && !it.startsWith("#") }.map { row ->
                    val fields = row.split('\t')
                    require(fields.size >= 6) { "Incomplete native catalog entry" }
                    val engine = NativeEngine.valueOf(fields[0].uppercase())
                    val id = fields[1]
                    val version = fields[2].toLong()
                    require(titleId.matches(id) && version in 0..MAX_VERSION)
                    val modules = fields.drop(5).associate { field ->
                        val pair = field.split('=', limit = 2)
                        require(pair.size == 2 && pair[0].isNotEmpty() && buildId.matches(pair[1]))
                        pair[0] to pair[1]
                    }
                    require(modules.size == fields.size - 5 && "main" in modules) {
                        "Missing or duplicate native module in $id"
                    }
                    NativeGameProfile(engine, id, version, fields[3], fields[4], modules)
                }.toList()
            }
            require(profiles.map { it.programId to it.version }.toSet().size == profiles.size) {
                "Ambiguous native catalog entry"
            }
            return NativeGameCatalog(profiles)
        }
    }
}
