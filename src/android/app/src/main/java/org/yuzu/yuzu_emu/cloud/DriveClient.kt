// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.cloud

import android.content.Context
import com.google.android.gms.auth.api.signin.GoogleSignInAccount
import java.io.IOException
import java.util.concurrent.TimeUnit
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.MultipartBody
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import org.json.JSONObject

/**
 * Eden Duo: the Drive v3 calls cloud saves need, and nothing else.
 *
 * Save files live in `appDataFolder`, one zip per title, named `save_<TITLEID>.zip`. The content
 * hash travels in the file's private `appProperties`, so one listing tells the sync everything it
 * needs to know without downloading anything: who changed, and whether they disagree.
 */
object DriveClient {
    const val FOLDER = "appDataFolder"
    private const val BASE = "https://www.googleapis.com/drive/v3/files"
    private const val UPLOAD = "https://www.googleapis.com/upload/drive/v3/files"
    private const val FIELDS = "id,name,modifiedTime,size,appProperties"

    /** One save zip as Drive sees it. [hash] is the tree hash recorded at upload time. */
    data class RemoteSave(
        val id: String,
        val name: String,
        val modifiedTime: Long,
        val size: Long,
        val hash: String?
    )

    private val client: OkHttpClient by lazy {
        OkHttpClient.Builder()
            .connectTimeout(20, TimeUnit.SECONDS)
            .readTimeout(60, TimeUnit.SECONDS)
            .writeTimeout(120, TimeUnit.SECONDS)
            .build()
    }

    private fun token(context: Context): String {
        val account: GoogleSignInAccount = CloudAuth.account(context)
            ?: throw IOException("Not signed in to Google")
        return CloudAuth.accessToken(context, account)
    }

    /** The save zip for [titleId], or null when this account never uploaded one. */
    fun findSave(context: Context, titleId: String): RemoteSave? {
        val query = "name = '${fileName(titleId)}' and trashed = false"
        val url = "$BASE?spaces=$FOLDER&fields=files($FIELDS)&q=${java.net.URLEncoder.encode(query, "UTF-8")}"
        val body = get(context, url).string()
        val files = JSONObject(body).optJSONArray("files") ?: return null
        if (files.length() == 0) return null
        val file = files.getJSONObject(0)
        return RemoteSave(
            id = file.getString("id"),
            name = file.optString("name"),
            modifiedTime = parseTime(file.optString("modifiedTime")),
            size = file.optString("size").toLongOrNull() ?: 0L,
            hash = file.optJSONObject("appProperties")?.optString("hash")?.takeIf { it.isNotEmpty() }
        )
    }

    fun download(context: Context, id: String): ByteArray =
        get(context, "$BASE/$id?alt=media").bytes()

    /**
     * Uploads [bytes] as the save zip for [titleId], creating the file or replacing the one at
     * [existingId]. The hash is stored as an app property so later listings can compare without a
     * download.
     */
    fun upload(context: Context, titleId: String, existingId: String?, bytes: ByteArray, hash: String): RemoteSave {
        val metadata = JSONObject()
            .put("name", fileName(titleId))
            .put("appProperties", JSONObject().put("hash", hash).put("uploadedAt", System.currentTimeMillis().toString()))
        if (existingId == null) {
            metadata.put("parents", org.json.JSONArray().put(FOLDER))
        }

        val request = MultipartBody.Builder()
            .setType(MultipartBody.FORM)
            .addFormDataPart("metadata", null, metadata.toString().toRequestBody(JSON))
            .addFormDataPart("file", fileName(titleId), bytes.toRequestBody(ZIP))
            .build()

        val url = if (existingId == null) {
            "$UPLOAD?uploadType=multipart&fields=$FIELDS"
        } else {
            "$UPLOAD/$existingId?uploadType=multipart&fields=$FIELDS"
        }
        val response = client.newCall(
            Request.Builder().url(url).header("Authorization", "Bearer ${token(context)}").patch(request).build()
        ).execute()
        val parsed = JSONObject(response.use { it.body?.string().orEmpty().ifEmpty { "{}" } })
        if (!response.isSuccessful) {
            throw IOException("Drive refused the upload: ${response.code} ${parsed.optString("error")}")
        }
        return RemoteSave(
            id = parsed.optString("id", existingId.orEmpty()),
            name = parsed.optString("name", fileName(titleId)),
            modifiedTime = parseTime(parsed.optString("modifiedTime")),
            size = parsed.optString("size").toLongOrNull() ?: bytes.size.toLong(),
            hash = hash
        )
    }

    fun delete(context: Context, id: String) {
        val response = client.newCall(
            Request.Builder().url("$BASE/$id")
                .header("Authorization", "Bearer ${token(context)}")
                .delete()
                .build()
        ).execute()
        response.use {
            if (!it.isSuccessful && it.code != 404) {
                throw IOException("Drive refused the delete: ${it.code}")
            }
        }
    }

    /** Renames a save so it is kept but no longer the save for its title (conflict aside). */
    fun rename(context: Context, id: String, name: String) {
        val metadata = JSONObject().put("name", name)
        val response = client.newCall(
            Request.Builder().url("$BASE/$id?fields=$FIELDS")
                .header("Authorization", "Bearer ${token(context)}")
                .patch(metadata.toString().toRequestBody(JSON))
                .build()
        ).execute()
        response.use {
            if (!it.isSuccessful) {
                throw IOException("Drive refused the rename: ${it.code}")
            }
        }
    }

    fun fileName(titleId: String): String = "save_${titleId.uppercase()}.zip"

    private fun get(context: Context, url: String) = run {
        val response = client.newCall(
            Request.Builder().url(url).header("Authorization", "Bearer ${token(context)}").get().build()
        ).execute()
        if (!response.isSuccessful) {
            val text = response.use { it.body?.string().orEmpty() }
            throw IOException("Drive answered ${response.code}: ${text.take(200)}")
        }
        response.body ?: throw IOException("Drive sent an empty body")
    }

    /** Drive answers RFC 3339 ("2026-10-27T12:00:00.000Z"); java.time needs API 26. */
    private fun parseTime(iso: String): Long {
        for (pattern in TIME_PATTERNS) {
            try {
                val format = java.text.SimpleDateFormat(pattern, java.util.Locale.US)
                format.timeZone = java.util.TimeZone.getTimeZone("UTC")
                return format.parse(iso)?.time ?: continue
            } catch (e: Exception) {
                continue
            }
        }
        return 0L
    }

    private val TIME_PATTERNS = listOf("yyyy-MM-dd'T'HH:mm:ss.SSS'Z'", "yyyy-MM-dd'T'HH:mm:ss'Z'")

    private val JSON = "application/json; charset=utf-8".toMediaType()
    private val ZIP = "application/zip".toMediaType()
}
