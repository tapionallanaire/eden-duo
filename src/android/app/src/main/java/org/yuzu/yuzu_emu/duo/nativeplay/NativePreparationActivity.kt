// SPDX-FileCopyrightText: Copyright 2026 Duet Plus contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.nativeplay

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.os.StatFs
import android.os.SystemClock
import android.text.format.Formatter
import android.util.Base64
import androidx.activity.OnBackPressedCallback
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.IntentCompat
import androidx.core.view.isVisible
import androidx.lifecycle.lifecycleScope
import com.google.android.material.button.MaterialButton
import com.google.android.material.progressindicator.LinearProgressIndicator
import com.google.android.material.textview.MaterialTextView
import java.io.File
import java.io.IOException
import java.security.MessageDigest
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.databinding.ActivityNativeLaunchBinding
import org.yuzu.yuzu_emu.duo.GameIconCache
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.utils.ThemeHelper

/** Resolves content in Duet's VFS before handing the immutable snapshot to the isolated packer. */
class NativePreparationActivity : AppCompatActivity() {
    private val cancelled = AtomicBoolean()
    private lateinit var game: Game
    private lateinit var expectedKey: String
    private lateinit var binding: ActivityNativeLaunchBinding
    private lateinit var status: MaterialTextView
    private lateinit var progress: LinearProgressIndicator
    private lateinit var cancel: MaterialButton
    private var childStarted = false
    private var exporting = false
    private var verifying = false
    @Volatile private var selectedInputPath: String? = null

    private val prepare = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
        childStarted = false
        if (result.resultCode != RESULT_OK) {
            finishFailure(result.data?.getStringExtra("native_error") ?: getString(R.string.native_prepare_interrupted),
                result.data?.getBooleanExtra("native_cancelled", false) == true,
                result.data?.getBooleanExtra("native_fallback", false) == true)
        } else verifyPrepared()
    }

    private fun verifyPrepared() {
        verifying = true
        status.setText(R.string.native_verifying)
        progress.isIndeterminate = true
        lifecycleScope.launch {
            try {
                val ready = withContext(Dispatchers.IO) {
                    val current = NativeGameSupport.findGameCandidate(this@NativePreparationActivity, game.path)
                    if (current == null || current.contentKey != expectedKey) {
                        throw IOException(getString(R.string.native_content_changed))
                    }
                    PreparedNativeGame.prepare(this@NativePreparationActivity, game.path, current)
                        ?: throw IOException(getString(R.string.native_runtime_unavailable))
                }
                if (cancelled.get()) {
                    finishFailure(getString(R.string.native_prepare_cancelled))
                } else {
                    setResult(RESULT_OK, ready.putExtra(SOURCE_PATH, game.path))
                    finish()
                }
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                finishFailure(e.message ?: e.javaClass.simpleName)
            }
        }
    }

    override fun attachBaseContext(base: Context) {
        super.attachBaseContext(YuzuApplication.applyLanguage(base))
    }


    override fun onCreate(savedInstanceState: Bundle?) {
        ThemeHelper.setTheme(this)
        super.onCreate(savedInstanceState)
        val selected = IntentCompat.getParcelableExtra(intent, GAME, Game::class.java)
        val key = intent.getStringExtra(CONTENT_KEY)
        if (selected == null || key.isNullOrBlank()) {
            finishFailure(getString(R.string.native_prepare_interrupted))
            return
        }
        game = selected
        expectedKey = key
        childStarted = savedInstanceState?.getBoolean("NativePackerStarted") == true
        verifying = savedInstanceState?.getBoolean("NativeVerifying") == true
        cancelled.set(savedInstanceState?.getBoolean("NativeCancelled") == true)
        selectedInputPath = savedInstanceState?.getString("NativeInputPath")
        buildContent()
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() = cancelExport()
        })
        if (verifying) verifyPrepared() else if (!childStarted) exportInput()
    }

    private fun buildContent() {
        binding = nativeLaunchContent(game) { cancelExport() }
        binding.toolbar.setTitle(R.string.native_prepare_title)
        binding.availability.setText(R.string.native_prepare_title)
        binding.summary.setText(R.string.native_reading_content)
        status = binding.status
        progress = binding.progress
        cancel = binding.primary
        cancel.setText(android.R.string.cancel)
        cancel.setOnClickListener { cancelExport() }
        binding.secondary.isVisible = false
        binding.detailsToggle.isVisible = false
        progress.isIndeterminate = true
        progress.max = 1000
        if (childStarted) status.setText(R.string.native_preparation_in_progress)
    }

    private fun exportInput() {
        exporting = true
        lifecycleScope.launch {
            try {
                val next = exportMutex.withLock {
                    withContext(Dispatchers.IO) {
                        val match = NativeGameSupport.findGameCandidate(this@NativePreparationActivity, game.path)
                        if (match == null || match.contentKey != expectedKey) {
                            throw IOException(getString(R.string.native_content_changed))
                        }
                        if (cancelled.get()) throw IOException(getString(R.string.native_prepare_cancelled))
                        val estimate = PreparedNativeGame.estimate(this@NativePreparationActivity, match)
                        runOnUiThread {
                            if (!isDestroyed && !isFinishing) {
                                binding.detailsText.text = getString(R.string.native_prepare_details, estimate.text, estimate.storagePath)
                                binding.detailsToggle.isVisible = true
                            }
                        }
                        val storage = File(estimate.storagePath)
                        val key = Base64.encodeToString(
                            MessageDigest.getInstance("SHA-256").digest(expectedKey.toByteArray(Charsets.UTF_8)),
                            Base64.URL_SAFE or Base64.NO_WRAP or Base64.NO_PADDING
                        )
                        val engine = match.profile.engine.name.lowercase(java.util.Locale.ROOT)
                        val directory = File(storage, "native/$engine/inputs/${match.profile.programId}/$key")
                        val input = NativeGameSupport.selectPrepareInput(
                            this@NativePreparationActivity, game.path, match.programIndex, match, directory,
                            selectedInputPath
                        )
                        selectedInputPath = input.path
                        val retained = if (input.isFile) input.length() else 0L
                        val needed = (estimate.requiredBytes - retained).coerceAtLeast(0L)
                        if (StatFs(storage.path).availableBytes < needed) {
                            throw IOException(getString(R.string.native_space_needed, Formatter.formatFileSize(this@NativePreparationActivity, needed)))
                        }
                        if (cancelled.get()) throw IOException(getString(R.string.native_prepare_cancelled))
                        PreparedNativeGame.selectStorage(this@NativePreparationActivity, match, estimate)
                        var lastUpdate = 0L
                        val error = PreparedNativeGame.beginInputExport(
                            this@NativePreparationActivity, game.path, input.path, match
                        ).use {
                            NativeGameSupport.exportPrepareInput(
                                this@NativePreparationActivity, game.path, match.programIndex, match, input.path,
                                NativePrepareProgress { completed, total ->
                                    val now = SystemClock.uptimeMillis()
                                    if (now - lastUpdate >= 200 || completed == total) {
                                        lastUpdate = now
                                        runOnUiThread {
                                            if (!isDestroyed && !isFinishing && !cancelled.get()) {
                                                progress.isIndeterminate = total <= 0
                                                if (total > 0) {
                                                    progress.setProgressCompat((completed.toDouble() / total * 1000).toInt().coerceIn(0, 1000), false)
                                                    status.text = getString(
                                                        R.string.native_reading_progress,
                                                        Formatter.formatShortFileSize(this@NativePreparationActivity, completed),
                                                        Formatter.formatShortFileSize(this@NativePreparationActivity, total)
                                                    )
                                                }
                                            }
                                        }
                                    }
                                    !cancelled.get()
                                }
                            )
                        }
                        if (cancelled.get()) throw IOException(getString(R.string.native_prepare_cancelled))
                        if (error != null) throw IOException(error)
                        PreparedNativeGame.compile(this@NativePreparationActivity, game.path, input.path, match)
                            .putExtra("duet_game_title", game.title)
                            .putExtra("duet_game_subtitle", listOf(game.developer, game.version).filter { it.isNotBlank() }.joinToString(" · "))
                            .putExtra("duet_game_artwork", GameIconCache.read(game))
                            .putExtra("duet_prepare_details", getString(R.string.native_prepare_details, estimate.text, estimate.storagePath))
                            .putExtra("duet_force_recompile", intent.getBooleanExtra("duet_force_recompile", false))
                    }
                }
                exporting = false
                if (cancelled.get()) {
                    finishFailure(getString(R.string.native_prepare_cancelled))
                } else {
                    childStarted = true
                    prepare.launch(next)
                }
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                exporting = false
                android.util.Log.e("NativePrepare", "Input preparation failed", e)
                finishFailure(e.message ?: e.javaClass.simpleName)
            }
        }
    }

    private fun cancelExport() {
        if (childStarted) return // The compiler activity owns its durable cancellation while visible.
        cancelled.set(true)
        cancel.isEnabled = false
        status.setText(R.string.native_prepare_cancelling)
        if (!exporting) finishFailure(getString(R.string.native_prepare_cancelled))
    }

    private fun finishFailure(reason: String, userCancelled: Boolean = cancelled.get(), safeForEmulation: Boolean = true) {
        setResult(
            RESULT_CANCELED,
            Intent().putExtra(SOURCE_PATH, if (::game.isInitialized) game.path else null)
                .putExtra("native_error", reason)
                .putExtra("native_cancelled", userCancelled).putExtra("native_fallback", safeForEmulation)
        )
        finish()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        outState.putBoolean("NativePackerStarted", childStarted)
        outState.putString("NativeInputPath", selectedInputPath)
        outState.putBoolean("NativeVerifying", verifying)
        outState.putBoolean("NativeCancelled", cancelled.get())
        super.onSaveInstanceState(outState)
    }

    override fun onDestroy() {
        if (exporting) cancelled.set(true)
        super.onDestroy()
    }

    companion object {
        const val SOURCE_PATH = "duet_game_path"
        private const val GAME = "native_game"
        private const val CONTENT_KEY = "native_content_key"
        // ponytail: one export at a time; use per-input locks only if concurrent preparation is needed.
        internal val exportMutex = Mutex()

        internal fun intent(context: Context, game: Game, match: NativeGameSupport.Match, recompile: Boolean = false): Intent {
            NativePlayPreferences.rememberProfile(context, game.programIdHex, match.profile)
            return Intent(context, NativePreparationActivity::class.java)
                .putExtra(GAME, game)
                .putExtra(CONTENT_KEY, match.contentKey)
                .putExtra("duet_force_recompile", recompile)
        }
    }
}
