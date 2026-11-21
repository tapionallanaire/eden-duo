// SPDX-FileCopyrightText: Copyright 2026 Duet Plus contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.nativeplay

import android.app.Application
import android.content.Context
import android.os.Bundle
import android.text.format.Formatter
import android.view.Gravity
import android.widget.LinearLayout
import android.widget.TextView
import androidx.activity.viewModels
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.isVisible
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.MutableLiveData
import androidx.lifecycle.viewModelScope
import com.google.android.material.button.MaterialButton
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.databinding.ActivityNativeStorageBinding
import org.yuzu.yuzu_emu.utils.ThemeHelper

internal class NativeStorageModel(private val app: Application) : AndroidViewModel(app) {
    data class State(
        val busy: Boolean,
        val games: List<NativeStorage.Game>? = null,
        val message: String = ""
    )

    val state = MutableLiveData<State>()

    init { refresh() }

    fun refresh() = update(false, null)
    fun purge(titleId: String?) = update(true, titleId)

    private fun update(purge: Boolean, titleId: String?) {
        if (state.value?.busy == true) return
        state.value = State(true, message = app.getString(
            if (purge) R.string.native_storage_working else R.string.native_storage_loading
        ))
        viewModelScope.launch {
            state.value = withContext(Dispatchers.IO) {
                val message = if (!purge) "" else try {
                    val result = NativeStorage.purge(app, titleId)
                    app.getString(R.string.native_storage_result, result.removed, result.busy, result.failed)
                } catch (e: CancellationException) {
                    throw e
                } catch (_: Exception) {
                    app.getString(R.string.native_storage_purge_error)
                }
                try {
                    val games = NativeStorage.inventory(app)
                    State(false, games, message)
                } catch (e: CancellationException) {
                    throw e
                } catch (_: Exception) {
                    State(false, message = listOf(message, app.getString(R.string.native_storage_load_error))
                        .filter { it.isNotEmpty() }.joinToString("\n"))
                }
            }
        }
    }
}

class NativeStorageActivity : AppCompatActivity() {
    private val model: NativeStorageModel by viewModels()
    private lateinit var binding: ActivityNativeStorageBinding

    override fun attachBaseContext(base: Context) = super.attachBaseContext(YuzuApplication.applyLanguage(base))

    override fun onCreate(savedInstanceState: Bundle?) {
        ThemeHelper.setTheme(this)
        super.onCreate(savedInstanceState)
        binding = ActivityNativeStorageBinding.inflate(layoutInflater)
        setContentView(binding.root)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        ViewCompat.setOnApplyWindowInsetsListener(binding.root) { view, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            view.setPadding(bars.left, bars.top, bars.right, bars.bottom)
            insets
        }
        ViewCompat.requestApplyInsets(binding.root)
        binding.toolbar.setNavigationOnClickListener { finish() }
        binding.refresh.setOnClickListener { model.refresh() }
        binding.purgeAll.setOnClickListener { confirm(null) }
        model.state.observe(this) { state ->
            val games = state.games
            binding.refresh.isEnabled = !state.busy
            binding.purgeAll.isVisible = !games.isNullOrEmpty()
            binding.total.isVisible = games != null
            binding.games.removeAllViews()
            if (games != null) {
                val label = if (games.any { it.error != null }) R.string.native_storage_partial_total
                    else R.string.native_storage_total
                binding.total.text = getString(label, size(games.sumOf { it.bytes }))
                games.forEach(::addGame)
            }
            binding.status.text = listOfNotNull(state.message.takeIf { it.isNotEmpty() },
                if (games?.isEmpty() == true) getString(R.string.native_storage_empty) else null).joinToString("\n")
            binding.status.isVisible = binding.status.text.isNotEmpty()
        }
    }

    private fun size(bytes: Long) = Formatter.formatFileSize(this, bytes)

    private fun addGame(game: NativeStorage.Game) {
        val row = LinearLayout(this).apply { gravity = Gravity.CENTER_VERTICAL }
        row.addView(TextView(this).apply {
            val usage = if (game.error == null) size(game.bytes) else getString(R.string.native_storage_unknown_size)
            text = getString(R.string.native_storage_row, game.name, game.titleId, usage)
        }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        row.addView(MaterialButton(this).apply {
            setText(R.string.native_storage_purge)
            contentDescription = getString(R.string.native_storage_action, game.name)
            setOnClickListener { confirm(game) }
        })
        binding.games.addView(row)
    }

    private fun confirm(game: NativeStorage.Game?) {
        val title = if (game == null) getString(R.string.native_storage_confirm_all)
            else getString(R.string.native_storage_confirm_game, game.name)
        MaterialAlertDialogBuilder(this)
            .setTitle(title)
            .setMessage(R.string.native_storage_warning)
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton(R.string.native_storage_purge) { _, _ -> model.purge(game?.titleId) }
            .show()
    }
}
