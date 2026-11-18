// SPDX-FileCopyrightText: Copyright 2026 Duet Plus contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.nativeplay

import android.content.Context
import android.content.Intent
import android.graphics.BitmapFactory
import android.os.Bundle
import android.os.SystemClock
import androidx.activity.OnBackPressedCallback
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.isVisible
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.databinding.ActivityNativeLaunchBinding
import org.yuzu.yuzu_emu.utils.ThemeHelper

/** One host-owned surface; engine facades retain compiler ownership and durable cancellation. */
class NativeCompilationActivity : AppCompatActivity() {
    private lateinit var binding: ActivityNativeLaunchBinding
    private lateinit var engine: NativeEngine
    private var jobPath: String? = null
    private var observer: Job? = null
    private var cancelling = false
    private var cancelInFlight = false
    private var userCancellation = false
    private var completed = false
    private var startingAt = 0L
    private var lastError: String? = null

    override fun attachBaseContext(base: Context) = super.attachBaseContext(YuzuApplication.applyLanguage(base))

    override fun onCreate(savedInstanceState: Bundle?) {
        ThemeHelper.setTheme(this)
        super.onCreate(savedInstanceState)
        setResult(RESULT_CANCELED, Intent().putExtra("native_error", getString(R.string.native_prepare_interrupted)))
        engine = when (intent.getStringExtra("duet_native_engine")) {
            "peanuts" -> NativeEngine.PEANUTS
            "banana" -> NativeEngine.BANANA
            else -> { fail(getString(R.string.native_runtime_unavailable), safeForEmulation = true); return }
        }
        binding = ActivityNativeLaunchBinding.inflate(layoutInflater)
        setContentView(binding.root)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        ViewCompat.setOnApplyWindowInsetsListener(binding.root) { view, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            view.setPadding(bars.left, bars.top, bars.right, bars.bottom)
            insets
        }
        ViewCompat.requestApplyInsets(binding.root)
        binding.toolbar.setTitle(R.string.native_prepare_title)
        binding.toolbar.setNavigationOnClickListener { cancelPreparation() }
        binding.gameTitle.text = intent.getStringExtra("duet_game_title").orEmpty()
        binding.gameSubtitle.text = intent.getStringExtra("duet_game_subtitle").orEmpty()
        binding.gameSubtitle.isVisible = binding.gameSubtitle.text.isNotEmpty()
        intent.getByteArrayExtra("duet_game_artwork")?.let { artwork ->
            binding.gameArtwork.setImageBitmap(BitmapFactory.decodeByteArray(artwork, 0, artwork.size))
        }
        binding.availability.setText(R.string.native_preparation_in_progress)
        binding.summary.setText(R.string.native_compile_preserves_data)
        binding.status.setText(R.string.native_verifying)
        binding.status.accessibilityLiveRegion = android.view.View.ACCESSIBILITY_LIVE_REGION_POLITE
        binding.progress.max = 1000
        binding.progress.isIndeterminate = true
        binding.detailsText.text = intent.getStringExtra("duet_prepare_details").orEmpty()
        binding.detailsToggle.setOnClickListener {
            val expanded = !binding.detailsText.isVisible
            binding.detailsText.isVisible = expanded
            binding.detailsToggle.setText(if (expanded) R.string.native_hide_details else R.string.native_show_details)
            ViewCompat.setStateDescription(binding.detailsToggle, getString(if (expanded) R.string.native_details_expanded else R.string.native_details_collapsed))
        }
        if (savedInstanceState?.getBoolean("details") == true) binding.detailsToggle.performClick()
        binding.primary.setText(R.string.native_resume)
        binding.primary.isVisible = false
        binding.primary.setOnClickListener { startPreparation() }
        binding.secondary.setText(R.string.native_cancel_preparation)
        binding.secondary.isEnabled = false
        binding.secondary.setOnClickListener { cancelPreparation() }
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() = cancelPreparation()
        })
        cancelling = savedInstanceState?.getBoolean("cancelling") == true
        userCancellation = savedInstanceState?.getBoolean("userCancellation") == true
        startingAt = savedInstanceState?.getLong("startingAt") ?: 0L
        val retained = savedInstanceState?.getString(JOB_PATH) ?: intent.getStringExtra(JOB_PATH)
        val consent = savedInstanceState == null && retained == null
        lifecycleScope.launch {
            try {
                val path = withContext(Dispatchers.IO) {
                    PreparedNativeGame.openPreparation(this@NativeCompilationActivity, intent, retained, engine)
                }
                if (completed) return@launch
                jobPath = path
                intent.putExtra(JOB_PATH, path)
                binding.secondary.isEnabled = true
                if (cancelling) cancelPreparation() else if (consent) startPreparation()
                observe()
            } catch (e: CancellationException) { throw e
            } catch (e: Exception) { fail(e.message ?: e.javaClass.simpleName, safeForEmulation = retained == null && jobPath == null) }
        }
    }

    override fun onStart() {
        super.onStart()
        if (::binding.isInitialized) observe()
    }

    override fun onStop() {
        observer?.cancel()
        observer = null
        super.onStop()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        outState.putString(JOB_PATH, jobPath ?: intent.getStringExtra(JOB_PATH))
        outState.putBoolean("cancelling", cancelling)
        outState.putBoolean("userCancellation", userCancellation)
        outState.putLong("startingAt", startingAt)
        outState.putBoolean("details", ::binding.isInitialized && binding.detailsText.isVisible)
        super.onSaveInstanceState(outState)
    }

    private fun startPreparation() {
        val path = jobPath ?: return
        if (completed || cancelling || startingAt != 0L) return
        startingAt = SystemClock.elapsedRealtime()
        lastError = null
        binding.primary.isVisible = false
        binding.status.setText(R.string.native_preparation_in_progress)
        lifecycleScope.launch {
            try {
                withContext(Dispatchers.IO) { PreparedNativeGame.startPreparation(this@NativeCompilationActivity, path, engine) }
            } catch (e: CancellationException) { throw e
            } catch (e: Exception) {
                lastError = e.message ?: e.javaClass.simpleName
                cancelPreparation(false)
            }
            observe()
        }
    }

    private fun cancelPreparation(userInitiated: Boolean = true) {
        if (completed || cancelInFlight) return
        if (userInitiated) userCancellation = true
        cancelling = true
        binding.primary.isVisible = false
        binding.secondary.isEnabled = false
        binding.status.setText(R.string.native_compiler_cancelling)
        val path = jobPath ?: return
        cancelInFlight = true
        lifecycleScope.launch {
            try {
                withContext(Dispatchers.IO) { PreparedNativeGame.cancelPreparation(this@NativeCompilationActivity, path, engine) }
            } catch (e: CancellationException) { throw e
            } catch (e: Exception) {
                lastError = e.message ?: e.javaClass.simpleName
                binding.status.text = lastError
                binding.secondary.isEnabled = true
            } finally { cancelInFlight = false }
            observe()
        }
    }

    private fun observe() {
        val path = jobPath ?: return
        if (completed || observer?.isActive == true || !lifecycle.currentState.isAtLeast(androidx.lifecycle.Lifecycle.State.STARTED)) return
        observer = lifecycleScope.launch {
            while (!completed) {
                try {
                    val state = withContext(Dispatchers.IO) {
                        PreparedNativeGame.preparationStatus(this@NativeCompilationActivity, path, engine)
                    }
                    val alive = state.getBoolean("alive")
                    val revoked = state.getBoolean("cancelled")
                    val promoted = state.getBoolean("promoted")
                    val settled = nativeCompilerHasSettled(alive, startingAt, SystemClock.elapsedRealtime())
                    if (cancelling && !alive && (revoked || promoted)) {
                        fail(lastError ?: getString(if (promoted) R.string.native_cancelled_pack_kept else R.string.native_prepare_cancelled),
                            userCancellation, safeForEmulation = true)
                        return@launch
                    }
                    if (promoted && !alive && !revoked && !cancelling) {
                        val ready = withContext(Dispatchers.IO) {
                            PreparedNativeGame.preparationResult(this@NativeCompilationActivity, path, engine)
                        }
                        if (ready != null && !cancelling) {
                            completed = true
                            setResult(RESULT_OK, ready)
                            finish()
                            return@launch
                        }
                    }
                    if (state.getBoolean("publication_started") && settled && !promoted) {
                        if (startingAt != 0L) {
                            lastError = getString(R.string.native_publication_interrupted)
                            cancelPreparation(false)
                            delay(1000)
                            continue
                        }
                        fail(getString(R.string.native_publication_interrupted), safeForEmulation = true)
                        return@launch
                    }
                    if (revoked && !alive) {
                        fail(lastError ?: getString(R.string.native_prepare_cancelled),
                            userCancellation || lastError == null, safeForEmulation = true)
                        return@launch
                    }
                    if (revoked) cancelling = true
                    if (settled && state.getString("state") == "failed") {
                        if (startingAt != 0L) {
                            lastError = state.optString("error").ifBlank { getString(R.string.native_prepare_interrupted) }
                            cancelPreparation(false)
                            delay(1000)
                            continue
                        }
                        fail(state.optString("error_detail").ifBlank {
                            state.optString("error").ifBlank { getString(R.string.native_prepare_interrupted) }
                        }, safeForEmulation = true)
                        return@launch
                    }
                    if (alive) startingAt = 0L
                    else if (startingAt != 0L && settled) {
                        lastError = getString(R.string.native_compiler_not_started)
                        cancelPreparation(false)
                    }
                    render(state, alive)
                } catch (e: CancellationException) { throw e
                } catch (e: Exception) {
                    lastError = e.message ?: e.javaClass.simpleName
                    binding.status.text = lastError
                    if (!cancelling) cancelPreparation(false)
                    binding.secondary.isEnabled = !cancelInFlight
                }
                delay(1000)
            }
        }
    }

    private fun render(state: JSONObject, alive: Boolean) {
        binding.progress.isIndeterminate = startingAt != 0L || cancelling
        binding.progress.setProgressCompat(state.getInt("progress").coerceIn(0, 1000), false)
        if (binding.gameTitle.text.isEmpty()) binding.gameTitle.text = state.optString("title", state.optString("title_id"))
        binding.status.text = if (cancelling) getString(R.string.native_compiler_cancelling) else lastError ?: state.getString("status")
        binding.summary.text = if (cancelling) getString(R.string.native_cancellation_wait) else state.getString("detail")
        binding.detailsText.text = listOf(state.optString("error_detail"), intent.getStringExtra("duet_prepare_details").orEmpty())
            .filter { it.isNotEmpty() }.joinToString("\n\n")
        binding.primary.setText(if (state.getString("state") == "failed") R.string.native_retry else R.string.native_resume)
        binding.primary.isVisible = !alive && startingAt == 0L && !cancelling
        binding.secondary.isEnabled = !cancelInFlight
        binding.secondary.setText(if (alive || startingAt != 0L || cancelling) R.string.native_cancel_preparation else R.string.native_return_to_duet)
        if (state.getString("state") == "failed") lastError = state.optString("error")
    }

    private fun fail(reason: String, userCancelled: Boolean = false, safeForEmulation: Boolean = false) {
        if (completed) return
        completed = true
        setResult(RESULT_CANCELED, Intent().putExtra("native_error", reason)
            .putExtra("native_cancelled", userCancelled).putExtra("native_fallback", safeForEmulation))
        if (callingActivity == null)
            android.widget.Toast.makeText(this, reason, android.widget.Toast.LENGTH_LONG).show()
        finish()
    }

    companion object {
        private const val JOB_PATH = "job"
    }
}

/** A delayed service start is not a failed run; a live compiler lease always wins over status. */
internal fun nativeCompilerHasSettled(alive: Boolean, requestedAt: Long, now: Long): Boolean =
    !alive && (requestedAt == 0L || now < requestedAt || now - requestedAt >= 5_000L)
