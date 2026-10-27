// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.cloud

import android.content.Context
import android.content.Intent
import com.google.android.gms.auth.GoogleAuthUtil
import com.google.android.gms.auth.api.signin.GoogleSignIn
import com.google.android.gms.auth.api.signin.GoogleSignInAccount
import com.google.android.gms.auth.api.signin.GoogleSignInOptions
import com.google.android.gms.common.Scopes
import com.google.android.gms.common.api.ApiException
import com.google.android.gms.common.api.Scope
import java.io.IOException
import org.yuzu.yuzu_emu.R

/**
 * Eden Duo: the Google account used for cloud saves.
 *
 * Sign-in is Google's own account picker. Everything the sync reads and writes lives in the app's
 * hidden Drive folder (`appDataFolder`), so it never appears in the user's Drive and no Drive scope
 * beyond that folder is requested.
 *
 * The OAuth client is the app's own, registered in the Google Cloud project for this package name
 * and signing certificate; [R.string.google_drive_client_id] carries its web client id. Without
 * that registration Google answers DEVELOPER_ERROR and nothing can be signed in - [isConfigured]
 * reports it so the settings screen can say so instead of failing a random tap.
 */
object CloudAuth {
    /** Drive's per-app hidden folder. Nothing outside it is reachable with this token. */
    private const val DRIVE_APPDATA_SCOPE = "https://www.googleapis.com/auth/drive.appdata"
    private const val OAUTH_TOKEN_PREFIX = "oauth2:https://www.googleapis.com/auth/drive.appdata"

    fun clientId(context: Context): String = context.getString(R.string.google_drive_client_id)

    fun isConfigured(context: Context): Boolean = clientId(context).isNotBlank()

    private fun options(context: Context): GoogleSignInOptions =
        GoogleSignInOptions.Builder(GoogleSignInOptions.DEFAULT_SIGN_IN)
            .requestEmail()
            .requestServerAuthCode(clientId(context))
            .requestScopes(Scope(DRIVE_APPDATA_SCOPE), Scope(Scopes.EMAIL))
            .build()

    /** The picker intent. The caller starts it for result and hands the data to [onSignInResult]. */
    fun signInIntent(context: Context): Intent =
        GoogleSignIn.getClient(context, options(context)).signInIntent

    fun signOut(context: Context) {
        GoogleSignIn.getClient(context, options(context)).signOut()
    }

    /** The account signed in on this device, if any. Cheap; safe to call on the main thread. */
    fun account(context: Context): GoogleSignInAccount? =
        try {
            GoogleSignIn.getLastSignedInAccount(context)
        } catch (e: Exception) {
            null
        }

    fun email(context: Context): String? = account(context)?.email

    /** Applies the picker result. Throws [IOException] with a message worth showing. */
    fun onSignInResult(context: Context, data: Intent?) {
        try {
            GoogleSignIn.getSignedInAccountFromIntent(data).getResult(ApiException::class.java)
                ?: throw IOException("Google returned no account")
        } catch (e: ApiException) {
            throw IOException(signInError(context, e.statusCode), e)
        }
    }

    private fun signInError(context: Context, status: Int): String = when {
        !isConfigured(context) -> context.getString(R.string.cloud_not_configured)
        status == 12501 -> context.getString(R.string.cloud_sign_in_cancelled)
        status == 12500 -> context.getString(R.string.cloud_sign_in_failed)
        else -> "${context.getString(R.string.cloud_sign_in_failed)} (${status})"
    }

    /**
     * A Drive access token for [account]. Blocking, network, off the main thread only. Google hands
     * out a token valid for about an hour; the caller fetches one per sync rather than caching.
     */
    fun accessToken(context: Context, account: GoogleSignInAccount): String {
        val googleAccount = account.account
            ?: throw IOException(context.getString(R.string.cloud_sign_in_failed))
        return try {
            GoogleAuthUtil.getToken(context, googleAccount, OAUTH_TOKEN_PREFIX)
        } catch (e: IOException) {
            throw e
        } catch (e: Exception) {
            throw IOException("Google refused a Drive token: ${e.message}", e)
        }
    }
}
