// SPDX-FileCopyrightText: Copyright 2026 Duet Plus contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.nativeplay

import android.os.Build
import java.io.File
import java.util.zip.ZipFile
import org.json.JSONObject
import org.yuzu.yuzu_emu.features.settings.model.StringSetting
import org.yuzu.yuzu_emu.features.settings.utils.SettingsFile
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.utils.GpuDriverHelper
import org.yuzu.yuzu_emu.utils.NativeConfig

internal object NativeLaunchDriver {
    const val SYSTEM = "system"

    /** Main-thread launch transaction, before any emulation configuration or core is active. */
    fun resolve(game: Game, custom: Boolean): String {
        return synchronized(NativeConfig) {
            check(!org.yuzu.yuzu_emu.NativeLibrary.isRunning() && !org.yuzu.yuzu_emu.NativeLibrary.isPaused()) {
                "Stop emulation before resolving a native driver"
            }
            check(!NativeConfig.isPerGameConfigLoaded()) { "Game settings are still in use" }
            try {
                if (custom) SettingsFile.loadCustomConfig(game) else NativeConfig.reloadGlobalConfig()
                StringSetting.DRIVER_PATH.getString(false).ifBlank { SYSTEM }
            } finally {
                NativeConfig.unloadPerGameConfig()
                NativeConfig.reloadGlobalConfig()
            }
        }
    }

    /** Inventory metadata is display-only. Reopen and validate the exact archive at launch. */
    fun validate(choice: String): File? {
        if (choice == SYSTEM) return null
        val root = File(GpuDriverHelper.driverStoragePath).canonicalFile
        val file = File(choice).canonicalFile
        require(file.parentFile == root && file.isFile && file.extension.equals("zip", true)) {
            "Selected native driver is not an installed driver ZIP"
        }
        require(GpuDriverHelper.getDrivers().any { File(it.first).canonicalFile == file }) {
            "Selected native driver is no longer installed"
        }
        ZipFile(file).use { zip ->
            val entries = zip.entries().asSequence().filter { it.name == "meta.json" }.toList()
            require(entries.size == 1) { "Driver ZIP must contain exactly one meta.json" }
            val entry = entries.single()
            require(!entry.isDirectory && entry.size in 1L..65536L) { "Invalid driver metadata size" }
            val metadata = zip.getInputStream(entry).use { input ->
                val bytes = input.readNBytes(65537)
                require(bytes.size <= 65536) { "Driver metadata is too large" }
                JSONObject(bytes.toString(Charsets.UTF_8))
            }
            val library = validateMetadata(metadata, Build.VERSION.SDK_INT)
            require(zip.entries().asSequence().any { !it.isDirectory && it.name.substringAfterLast('/') == library }) {
                "Driver library is missing or invalid"
            }
        }
        return file
    }

    internal fun validateMetadata(metadata: JSONObject, api: Int): String {
        for (key in listOf("name", "description", "author", "vendor", "driverVersion", "libraryName")) {
            val value = metadata.opt(key)
            require(value is String && (key == "description" || value.isNotBlank())) {
                "Driver metadata is missing $key"
            }
        }
        val minApi = metadata.opt("minApi")
        require(minApi is Int && minApi in 1..api) { "Driver is incompatible with this Android version" }
        val library = metadata.getString("libraryName")
        require(library.endsWith(".so") && '/' !in library && '\\' !in library) {
            "Driver library is missing or invalid"
        }
        return library
    }
}
