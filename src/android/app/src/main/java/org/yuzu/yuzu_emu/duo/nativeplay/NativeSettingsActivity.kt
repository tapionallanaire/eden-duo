// SPDX-FileCopyrightText: Copyright 2026 Duet Plus contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.nativeplay

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.text.format.Formatter
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.IntentCompat
import androidx.core.view.isVisible
import androidx.lifecycle.lifecycleScope
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.databinding.ActivityNativeLaunchBinding
import org.yuzu.yuzu_emu.model.Game

/** Native preferences and derived data only; deleting a pack never deletes a save or source. */
class NativeSettingsActivity : AppCompatActivity() {
    private lateinit var binding: ActivityNativeLaunchBinding
    private lateinit var game: Game
    private var match: NativeGameSupport.Match? = null
    private var dataOwner: Pair<NativeEngine, String>? = null
    private var loading = false
    private var childStarted = false

    private val preparation = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
        childStarted = false
        loadData(result.data?.getStringExtra("native_error"))
    }

    override fun attachBaseContext(base: Context) = super.attachBaseContext(YuzuApplication.applyLanguage(base))

    override fun onCreate(savedInstanceState: Bundle?) {
        org.yuzu.yuzu_emu.utils.ThemeHelper.setTheme(this)
        super.onCreate(savedInstanceState)
        val selected = IntentCompat.getParcelableExtra(intent, GAME, Game::class.java)
        if (selected == null) { finish(); return }
        game = selected
        childStarted = savedInstanceState?.getBoolean("preparing") == true
        binding = nativeLaunchContent(game) { finish() }
        binding.toolbar.setTitle(R.string.native_settings)
        binding.availability.setText(R.string.native_settings)
        binding.summary.setText(R.string.native_mode_explanation)
        binding.saveWarning.isVisible = false
        binding.nativeMode.isVisible = true
        binding.nativeMode.check(when (NativePlayPreferences.mode(this, game.programIdHex)) {
            NativePlayPreferences.Mode.ASK -> R.id.mode_ask
            NativePlayPreferences.Mode.EMULATED -> R.id.mode_emulated
            NativePlayPreferences.Mode.NATIVE -> R.id.mode_native
        })
        binding.nativeMode.setOnCheckedChangeListener { _, checked ->
            val mode = when (checked) {
                R.id.mode_emulated -> NativePlayPreferences.Mode.EMULATED
                R.id.mode_native -> NativePlayPreferences.Mode.NATIVE
                else -> NativePlayPreferences.Mode.ASK
            }
            NativePlayPreferences.setMode(this, game.programIdHex, mode)
        }
        binding.primary.setText(R.string.native_recompile)
        binding.primary.setOnClickListener {
            val selectedMatch = match ?: return@setOnClickListener
            if (childStarted || loading) return@setOnClickListener
            childStarted = true
            setBusy(true)
            try {
                preparation.launch(NativePreparationActivity.intent(this, game, selectedMatch, recompile = true))
            } catch (e: Exception) {
                childStarted = false
                loadData(e.message ?: e.javaClass.simpleName)
            }
        }
        binding.secondary.setText(R.string.native_delete_compiled)
        binding.secondary.setOnClickListener { confirmDelete() }
        if (savedInstanceState?.getBoolean("details") == true) binding.detailsToggle.performClick()
        if (!childStarted) loadData() else setBusy(true)
    }

    private fun setBusy(value: Boolean) {
        if (value) binding.memberChoices.isVisible = false
        binding.progress.isVisible = value
        binding.progress.isIndeterminate = true
        binding.primary.isEnabled = !value && match != null
        binding.secondary.isEnabled = !value && dataOwner != null
    }

    private fun loadData(message: String? = null, knownMembers: NativeGameSupport.Members? = null) {
        if (loading || childStarted) return
        loading = true
        setBusy(true)
        lifecycleScope.launch {
            var options = knownMembers
            try {
                val members = knownMembers ?: withContext(Dispatchers.IO) {
                    NativeGameSupport.members(this@NativeSettingsActivity, game)
                }
                options = members
                val result = withContext(Dispatchers.IO) {
                    val candidate = members.match(this@NativeSettingsActivity, game.path)
                    val owner = members.selected?.let { it.engine to it.programId }
                        ?: if (members.choices.isEmpty()) members.remembered
                            ?: assets.open(NativeGameCatalog.ASSET).reader().use(NativeGameCatalog::read)
                                .versions(game.programIdHex).firstOrNull()?.let { it.engine to it.programId }
                        else null
                    val state = owner?.let { PreparedNativeGame.compiledData(this@NativeSettingsActivity, game.path, it.second, it.first) }
                    Triple(candidate, owner, state)
                }
                match = result.first
                dataOwner = result.second.takeIf { result.third?.getBoolean("present") == true }
                val state = result.third
                val busy = state?.getBoolean("busy") == true
                binding.status.text = message ?: if (busy) getString(R.string.native_data_busy)
                    else if (members.choices.isNotEmpty() && members.selected == null) getString(R.string.native_choose_member)
                    else if (match == null) getString(R.string.native_not_available_message)
                    else getString(R.string.native_needs_preparation)
                binding.detailsText.text = getString(R.string.native_data_size,
                    Formatter.formatFileSize(this@NativeSettingsActivity, state?.getLong("bytes") ?: 0L),
                    Formatter.formatFileSize(this@NativeSettingsActivity, state?.getLong("input_bytes") ?: 0L))
                binding.detailsToggle.isVisible = true
                setBusy(false)
                binding.primary.isEnabled = !busy && match != null
                binding.secondary.isEnabled = !busy && dataOwner != null
            } catch (e: CancellationException) { throw e
            } catch (e: Exception) {
                match = null
                dataOwner = null
                binding.status.text = e.message ?: e.javaClass.simpleName
                setBusy(false)
            } finally {
                options?.let { members ->
                    binding.showMembers(game, members) { loadData(knownMembers = it) }
                    if (members.choices.isEmpty()) dataOwner?.let { binding.gameTitle.text = "${game.title} · ${it.second}" }
                }
                loading = false
            }
        }
    }

    private fun confirmDelete() {
        val owner = dataOwner ?: return
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.native_delete_compiled)
            .setMessage(R.string.native_delete_compiled_confirmation)
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton(R.string.native_delete_compiled) { _, _ ->
                if (loading || childStarted) return@setPositiveButton
                loading = true
                setBusy(true)
                lifecycleScope.launch {
                    var message: String
                    try {
                        NativePreparationActivity.exportMutex.withLock {
                            withContext(Dispatchers.IO) {
                                PreparedNativeGame.deleteCompiledData(this@NativeSettingsActivity, game.path, owner.second, owner.first)
                            }
                        }
                        message = getString(R.string.native_data_deleted)
                    } catch (e: CancellationException) { throw e
                    } catch (e: Exception) { message = e.message ?: e.javaClass.simpleName }
                    loading = false
                    loadData(message)
                }
            }.show()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        outState.putBoolean("preparing", childStarted)
        outState.putBoolean("details", binding.detailsText.isVisible)
        super.onSaveInstanceState(outState)
    }

    companion object {
        private const val GAME = "native_game"
        fun intent(context: Context, game: Game) = Intent(context, NativeSettingsActivity::class.java).putExtra(GAME, game)
    }
}
