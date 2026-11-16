// SPDX-FileCopyrightText: Copyright 2026 tapionallanaire
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import android.content.Context
import java.net.HttpURLConnection
import java.net.URL
import org.json.JSONObject
import org.yuzu.yuzu_emu.BuildConfig
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.utils.Log

/**
 * Duet Plus's own update check. Eden's checker reads Eden's feed, which would offer upstream Eden
 * (another app), so it stays compiled out; this reads Duet Plus's GitHub releases and hands the
 * result to the same dialog, download and install path the checker feeds.
 */
object DuoUpdates {
    /** The published package. Development builds sit beside it under another id and never update. */
    const val PACKAGE = "com.tapionallanaire.duetplus"

    private const val REPO = "tapionallanaire/duet-plus"
    private const val API = "https://api.github.com/repos/$REPO/releases/latest"
    private const val LAST_OFFERED = "duo_update_last_offered"

    /** Only the published package updates itself; a dev build updating would install a second app. */
    fun enabled(context: Context): Boolean = context.packageName == PACKAGE

    /**
     * The newest release when it is newer than this build, else null. Touches the network, so
     * never call it on the main thread.
     */
    fun latest(context: Context): NativeLibrary.UpdateResult? {
        if (!enabled(context)) return null
        val body = try {
            fetch(API)
        } catch (error: Exception) {
            Log.warning("[DuoUpdates] Cannot read releases: ${error.message}")
            return null
        } ?: return null

        val release = try {
            parse(body, BuildConfig.VERSION_NAME)
        } catch (error: Exception) {
            Log.warning("[DuoUpdates] Cannot read the release: ${error.message}")
            return null
        } ?: return null

        // Offer each version once: the dialog has "Don't show again" for good.
        val offered = context.getSharedPreferences(PACKAGE, Context.MODE_PRIVATE)
            .getString(LAST_OFFERED, null)
        if (offered == release.tag) return null
        context.getSharedPreferences(PACKAGE, Context.MODE_PRIVATE).edit()
            .putString(LAST_OFFERED, release.tag).apply()
        return release
    }

    private fun fetch(url: String): String? {
        val connection = URL(url).openConnection() as HttpURLConnection
        return try {
            connection.connectTimeout = 10_000
            connection.readTimeout = 15_000
            connection.setRequestProperty("Accept", "application/vnd.github+json")
            connection.setRequestProperty("User-Agent", "DuetPlus-Android")
            if (connection.responseCode != HttpURLConnection.HTTP_OK) return null
            connection.inputStream.bufferedReader().use { it.readText() }
        } finally {
            connection.disconnect()
        }
    }

    /** Reads a GitHub release; null when it carries no installable APK or is not newer. */
    internal fun parse(json: String, installedVersion: String): NativeLibrary.UpdateResult? {
        val release = JSONObject(json)
        val tag = release.optString("tag_name")
        if (tag.isEmpty() || !isNewer(tag, installedVersion)) return null
        val assets = release.optJSONArray("assets") ?: return null
        val apks = (0 until assets.length())
            .mapNotNull { assets.optJSONObject(it)?.optString("browser_download_url")?.takeIf(String::isNotEmpty) }
            .filter { it.endsWith(".apk", ignoreCase = true) }
        if (apks.isEmpty()) return null
        return NativeLibrary.UpdateResult(
            tag = tag,
            title = release.optString("name").ifEmpty { tag },
            body = release.optString("body"),
            url = release.optString("html_url"),
            assets = apks.toMutableList()
        )
    }

    /** True when [remote] is a later version than [installed]; "v1.1.2" and "1.1.2" are the same. */
    internal fun isNewer(remote: String, installed: String): Boolean {
        val a = versionParts(remote)
        val b = versionParts(installed)
        for (index in 0 until maxOf(a.size, b.size)) {
            val left = a.getOrElse(index) { 0 }
            val right = b.getOrElse(index) { 0 }
            if (left != right) return left > right
        }
        return false
    }

    private fun versionParts(version: String): List<Int> =
        Regex("\\d+").findAll(version.substringBefore('-')).map { it.value.toInt() }.toList()
}
