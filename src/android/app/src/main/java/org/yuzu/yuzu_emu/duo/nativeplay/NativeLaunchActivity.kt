// SPDX-FileCopyrightText: Copyright 2026 Duet Plus contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.nativeplay

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.text.format.Formatter
import android.view.View
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.IntentCompat
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.isVisible
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.databinding.ActivityNativeLaunchBinding
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.utils.GameIconUtils
import org.yuzu.yuzu_emu.utils.ThemeHelper

/** A launch destination, not a dialog over an emulation surface. */
class NativeLaunchActivity : AppCompatActivity() {
    private lateinit var binding: ActivityNativeLaunchBinding
    private lateinit var game: Game
    private var match: NativeGameSupport.Match? = null
    private var prepared = false
    private var childStarted = false
    private var contentKey = ""
    private var loading = false
    private var failure = ""

    private val preparation = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
        childStarted = false
        if (result.data?.getStringExtra(NativePreparationActivity.SOURCE_PATH) != game.path) {
            showFailure(getString(R.string.native_prepare_interrupted))
        } else if (result.resultCode == RESULT_OK &&
            result.data?.component?.packageName == packageName &&
            result.data?.component?.className in setOf("dev.nxnative.android.GameActivity", "dev.nxbanana.android.GameActivity")) {
            // The gate re-resolves the effective build and validates the immutable pack again.
            finishChoice(NATIVE)
        } else {
            val reason = result.data?.getStringExtra("native_error") ?: getString(R.string.native_prepare_interrupted)
            if (result.data?.getBooleanExtra("native_fallback", false) == true &&
                result.data?.getBooleanExtra("native_cancelled", false) != true) {
                setResult(RESULT_OK, Intent().putExtra(CHOICE, EMULATE)
                    .putExtra(NativePreparationActivity.SOURCE_PATH, game.path)
                    .putExtra(CONTENT_KEY, contentKey).putExtra("native_error", reason))
                finish()
            } else showFailure(reason)
        }
    }

    override fun attachBaseContext(base: Context) = super.attachBaseContext(YuzuApplication.applyLanguage(base))

    override fun onCreate(savedInstanceState: Bundle?) {
        ThemeHelper.setTheme(this)
        super.onCreate(savedInstanceState)
        val selected = IntentCompat.getParcelableExtra(intent, GAME, Game::class.java)
        if (selected == null) {
            finish()
            return
        }
        game = selected
        childStarted = savedInstanceState?.getBoolean("preparing") == true
        contentKey = savedInstanceState?.getString(CONTENT_KEY).orEmpty()
        failure = savedInstanceState?.getString("failure") ?: intent.getStringExtra(ERROR).orEmpty()
        binding = nativeLaunchContent(game) { finish() }
        binding.secondary.setOnClickListener { finishChoice(EMULATE) }
        binding.primary.setOnClickListener {
            if (failure.isNotEmpty()) {
                failure = ""
                loadReadiness()
            } else if (prepared) {
                finishChoice(NATIVE)
            } else {
                val candidate = match ?: return@setOnClickListener
                childStarted = true
                setActionsEnabled(false)
                try {
                    preparation.launch(NativePreparationActivity.intent(this, game, candidate))
                    NativePlayPreferences.setMode(this, game.programIdHex, NativePlayPreferences.Mode.NATIVE)
                } catch (e: Exception) {
                    childStarted = false
                    showFailure(e.message ?: e.javaClass.simpleName)
                }
            }
        }
        if (savedInstanceState?.getBoolean("details") == true) binding.detailsToggle.performClick()
        if (childStarted) {
            // ActivityResultRegistry owns the in-flight child across recreation.
            binding.availability.setText(R.string.native_prepare_title)
            binding.status.setText(R.string.native_preparation_in_progress)
            setActionsEnabled(false)
        } else if (failure.isNotEmpty()) showFailure(failure) else loadReadiness()
    }

    private fun loadReadiness() {
        if (loading) return
        loading = true
        prepared = false
        match = null
        binding.availability.setText(R.string.native_checking)
        binding.summary.text = ""
        binding.status.text = ""
        binding.progress.isVisible = true
        binding.primary.setText(R.string.native_compile_play)
        binding.primary.isEnabled = false
        binding.secondary.isEnabled = true
        lifecycleScope.launch {
            try {
                var unavailableReason = ""
                val candidate = withContext(Dispatchers.IO) {
                    NativeGameSupport.findGameCandidate(this@NativeLaunchActivity, game.path) {
                        if (unavailableReason.isEmpty()) unavailableReason = it.message.orEmpty()
                    }
                        ?.takeIf { PreparedNativeGame.available(it.profile.engine) }
                }
                match = candidate
                contentKey = candidate?.contentKey.orEmpty()
                if (candidate == null || !PreparedNativeGame.available(candidate.profile.engine)) {
                    binding.availability.setText(R.string.native_not_available)
                    binding.summary.text = unavailableReason.ifEmpty { getString(R.string.native_not_available_message) }
                    binding.primary.setText(R.string.native_play)
                    binding.primary.isVisible = true
                    binding.detailsToggle.isVisible = false
                    binding.saveWarning.isVisible = false
                } else {
                    val ready = withContext(Dispatchers.IO) {
                        // A missing or outdated pack needs preparation; launch always checks it again.
                        runCatching { PreparedNativeGame.prepare(this@NativeLaunchActivity, game.path, candidate) }.getOrNull()
                    }
                    prepared = ready != null
                    if (ready != null) {
                        binding.availability.setText(R.string.native_ready)
                        binding.summary.setText(R.string.native_ready_summary)
                        binding.detailsText.text = ready.getStringExtra("native_shader_coverage").orEmpty()
                        binding.primary.setText(R.string.native_play)
                    } else {
                        val estimate = withContext(Dispatchers.IO) { PreparedNativeGame.estimate(this@NativeLaunchActivity, candidate) }
                        binding.availability.setText(R.string.native_needs_preparation)
                        binding.summary.text = getString(R.string.native_prepare_summary, estimate.timeText,
                            Formatter.formatShortFileSize(this@NativeLaunchActivity, estimate.requiredBytes))
                        binding.detailsText.text = getString(R.string.native_prepare_details, estimate.text, estimate.storagePath)
                    }
                    binding.detailsToggle.isVisible = binding.detailsText.text.isNotBlank()
                    binding.primary.isVisible = true
                    binding.primary.isEnabled = true
                    binding.saveWarning.isVisible = true
                }
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                showFailure(e.message ?: e.javaClass.simpleName)
            } finally {
                loading = false
                binding.progress.isVisible = false
            }
        }
    }

    private fun showFailure(reason: String) {
        failure = reason
        binding.availability.setText(R.string.native_launch_failed)
        binding.detailsText.text = reason
        binding.detailsToggle.isVisible = true
        binding.summary.setText(R.string.native_recovery_summary)
        binding.status.text = reason.lineSequence().firstOrNull().orEmpty()
        binding.progress.isVisible = false
        binding.primary.isVisible = true
        binding.primary.setText(R.string.native_retry)
        setActionsEnabled(true)
    }

    private fun setActionsEnabled(enabled: Boolean) {
        binding.primary.isEnabled = enabled
        binding.secondary.isEnabled = enabled
    }

    private fun finishChoice(choice: String) {
        if (isFinishing || childStarted) return
        setActionsEnabled(false)
        NativePlayPreferences.setMode(this, game.programIdHex,
            if (choice == NATIVE) NativePlayPreferences.Mode.NATIVE else NativePlayPreferences.Mode.EMULATED)
        setResult(RESULT_OK, Intent().putExtra(CHOICE, choice)
            .putExtra(NativePreparationActivity.SOURCE_PATH, game.path)
            .putExtra(CONTENT_KEY, contentKey))
        finish()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        outState.putBoolean("preparing", childStarted)
        outState.putBoolean("details", binding.detailsText.isVisible)
        outState.putString("failure", failure)
        // Preparation's result can arrive before readiness is loaded in a recreated host.
        outState.putString(CONTENT_KEY, contentKey)
        super.onSaveInstanceState(outState)
    }

    companion object {
        private const val GAME = "native_game"
        private const val ERROR = "native_error"
        const val CHOICE = "native_choice"
        const val CONTENT_KEY = "native_content_key"
        const val EMULATE = "emulate"
        const val NATIVE = "native"

        internal fun intent(context: Context, game: Game, error: String = "") =
            Intent(context, NativeLaunchActivity::class.java).putExtra(GAME, game).putExtra(ERROR, error)
    }
}

/** The launch and content-export stages use the same Duet artwork, navigation and fixed actions. */
internal fun AppCompatActivity.nativeLaunchContent(game: Game, back: () -> Unit): ActivityNativeLaunchBinding {
    val binding = ActivityNativeLaunchBinding.inflate(layoutInflater)
    setContentView(binding.root)
    WindowCompat.setDecorFitsSystemWindows(window, false)
    ViewCompat.setOnApplyWindowInsetsListener(binding.root) { view, insets ->
        val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
        view.setPadding(bars.left, bars.top, bars.right, bars.bottom)
        insets
    }
    ViewCompat.requestApplyInsets(binding.root)
    binding.toolbar.setNavigationOnClickListener { back() }
    GameIconUtils.loadGameIcon(game, binding.gameArtwork)
    binding.gameTitle.text = game.title
    binding.gameSubtitle.text = listOf(game.developer, game.version).filter { it.isNotBlank() }.joinToString(" · ")
    binding.gameSubtitle.isVisible = binding.gameSubtitle.text.isNotEmpty()
    binding.detailsToggle.setOnClickListener {
        val expanded = !binding.detailsText.isVisible
        binding.detailsText.isVisible = expanded
        binding.detailsToggle.setText(if (expanded) R.string.native_hide_details else R.string.native_show_details)
        ViewCompat.setStateDescription(binding.detailsToggle, getString(if (expanded) R.string.native_details_expanded else R.string.native_details_collapsed))
        if (expanded) binding.detailsText.post {
            binding.detailsText.requestRectangleOnScreen(
                android.graphics.Rect(0, 0, binding.detailsText.width, binding.detailsText.height)
            )
        }
    }
    binding.status.accessibilityLiveRegion = View.ACCESSIBILITY_LIVE_REGION_POLITE
    return binding
}
