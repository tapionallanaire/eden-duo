// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.fragments

import android.app.Dialog
import android.graphics.Typeface
import android.os.Bundle
import android.text.Html
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.style.BackgroundColorSpan
import android.text.style.ForegroundColorSpan
import android.text.style.StyleSpan
import android.text.method.LinkMovementMethod
import org.commonmark.parser.Parser
import org.commonmark.renderer.html.HtmlRenderer
import android.view.View
import android.widget.RadioGroup
import androidx.appcompat.app.AlertDialog
import androidx.core.os.bundleOf
import androidx.core.view.isVisible
import androidx.fragment.app.DialogFragment
import androidx.lifecycle.lifecycleScope
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.color.MaterialColors
import com.google.android.material.radiobutton.MaterialRadioButton
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.databinding.DialogDualScreenModFetcherBinding
import org.yuzu.yuzu_emu.model.DualScreenModRelease
import org.yuzu.yuzu_emu.utils.DualScreenModRepository

class DualScreenModFetcherDialogFragment : DialogFragment() {
    private var _binding: DialogDualScreenModFetcherBinding? = null
    private val binding get() = _binding!!
    private var fetchJob: Job? = null
    private var selected: DualScreenModRelease? = null
    private var selectedUrl: String? = null
    private var showReleaseNotes = false

    override fun onCreateDialog(savedInstanceState: Bundle?): Dialog {
        selectedUrl = savedInstanceState?.getString(SELECTED_URL)
        showReleaseNotes = savedInstanceState?.getBoolean(SHOW_RELEASE_NOTES) ?: false
        _binding = DialogDualScreenModFetcherBinding.inflate(layoutInflater)
        binding.showReleaseNotes.isChecked = showReleaseNotes
        binding.showReleaseNotes.setOnCheckedChangeListener { _, checked ->
            showReleaseNotes = checked
            binding.notes.isVisible = checked && selected != null
        }
        binding.notes.movementMethod = LinkMovementMethod.getInstance()
        binding.game.text = getString(
            R.string.dual_screen_mod_fetch_source,
            requireArguments().getString(GAME_TITLE)
        )
        return MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.dual_screen_mods)
            .setView(binding.root)
            .setPositiveButton(R.string.install, null)
            .setNeutralButton(R.string.refresh, null)
            .setNegativeButton(android.R.string.cancel, null)
            .create()
    }

    override fun onStart() {
        super.onStart()
        // Eden Duo: a fetcher saved by a two-screen process can be restored in a later process
        // that now starts single-screen; the picker and result listeners are gated, but this
        // dialog would otherwise still fetch and show usable dual-screen UI.
        if (org.yuzu.yuzu_emu.duo.DuoCapability.isSingleScreen(requireContext())) {
            dismiss()
            return
        }
        val alert = requireDialog() as AlertDialog
        alert.getButton(Dialog.BUTTON_POSITIVE).setOnClickListener {
            selected?.let { release ->
                parentFragmentManager.setFragmentResult(
                    REQUEST_INSTALL,
                    bundleOf(RELEASE to release)
                )
                dismiss()
            }
        }
        alert.getButton(Dialog.BUTTON_NEUTRAL).setOnClickListener { fetchReleases() }
        if (selected == null && fetchJob?.isActive != true) fetchReleases()
    }

    private fun fetchReleases() {
        fetchJob?.cancel()
        selected = null
        binding.releases.removeAllViews()
        binding.notes.text = ""
        binding.notes.isVisible = false
        binding.gameVersion.isVisible = false
        binding.showReleaseNotes.isVisible = false
        binding.loading.isVisible = true
        binding.status.isVisible = true
        binding.status.setText(R.string.dual_screen_mod_fetch_loading)
        val alert = requireDialog() as AlertDialog
        alert.getButton(Dialog.BUTTON_POSITIVE).isEnabled = false
        alert.getButton(Dialog.BUTTON_NEUTRAL).isEnabled = false
        fetchJob = lifecycleScope.launch {
            try {
                val releases = withContext(Dispatchers.IO) {
                    DualScreenModRepository.fetch(requireArguments().getString(TITLE_ID).orEmpty())
                }
                binding.status.isVisible = releases.isEmpty()
                binding.status.setText(R.string.dual_screen_mod_fetch_empty)
                binding.showReleaseNotes.isVisible = releases.isNotEmpty()
                val spacing =
                    resources.getDimensionPixelSize(R.dimen.dual_screen_fetch_option_spacing)
                for ((index, release) in releases.withIndex()) {
                    val button = MaterialRadioButton(requireContext()).apply {
                        id = View.generateViewId()
                        setPadding(paddingLeft, spacing / 2, paddingRight, spacing / 2)
                        val label = getString(
                            R.string.dual_screen_mod_fetch_release,
                            release.title,
                            release.tag,
                            release.publishedAt.take(10)
                        )
                        text = if (index == 0) latestReleaseLabel(label, this) else label
                        setOnCheckedChangeListener { _, checked ->
                            if (checked) {
                                selected = release
                                selectedUrl = release.downloadUrl
                                val gameVersion =
                                    DualScreenModRepository.requiredGameVersion(release.notes)
                                binding.gameVersion.isVisible = gameVersion != null
                                binding.gameVersion.text = gameVersion?.let {
                                    getString(R.string.dual_screen_fetch_game_version, it)
                                }
                                binding.notes.isVisible = showReleaseNotes
                                val notes = release.notes.ifBlank {
                                    getString(R.string.dual_screen_mod_fetch_no_notes)
                                }
                                val html = HtmlRenderer.builder().escapeHtml(true).build()
                                    .render(Parser.builder().build().parse(notes))
                                // Android's HTML list renderer places the bullet against the text.
                                binding.notes.text = Html.fromHtml(
                                    html.replace("<li>", "<li>&nbsp;"),
                                    Html.FROM_HTML_MODE_COMPACT
                                )
                            }
                        }
                    }
                    binding.releases.addView(
                        button,
                        RadioGroup.LayoutParams(
                            RadioGroup.LayoutParams.MATCH_PARENT,
                            RadioGroup.LayoutParams.WRAP_CONTENT
                        ).apply { if (index > 0) topMargin = spacing }
                    )
                }
                val selectedIndex = releases.indexOfFirst { it.downloadUrl == selectedUrl }
                    .coerceAtLeast(0)
                if (releases.isNotEmpty()) {
                    binding.releases.check(binding.releases.getChildAt(selectedIndex).id)
                }
                alert.getButton(Dialog.BUTTON_POSITIVE).isEnabled = selected != null
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                binding.gameVersion.isVisible = false
                binding.showReleaseNotes.isVisible = false
                binding.notes.isVisible = false
                binding.status.isVisible = true
                binding.status.text = getString(
                    R.string.dual_screen_mod_fetch_failed,
                    e.message.orEmpty()
                )
            } finally {
                _binding?.loading?.isVisible = false
                alert.getButton(Dialog.BUTTON_NEUTRAL).isEnabled = true
            }
        }
    }

    private fun latestReleaseLabel(label: String, view: View): CharSequence {
        val text = SpannableStringBuilder(label)
        text.append("  ")
        val start = text.length
        text.append("\u00a0").append(getString(R.string.dual_screen_fetch_latest)).append("\u00a0")
        val flags = Spanned.SPAN_EXCLUSIVE_EXCLUSIVE
        text.setSpan(
            BackgroundColorSpan(
                MaterialColors.getColor(view, com.google.android.material.R.attr.colorPrimaryContainer)
            ),
            start, text.length, flags
        )
        text.setSpan(
            ForegroundColorSpan(
                MaterialColors.getColor(view, com.google.android.material.R.attr.colorOnPrimaryContainer)
            ),
            start, text.length, flags
        )
        text.setSpan(StyleSpan(Typeface.BOLD), start, text.length, flags)
        return text
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putString(SELECTED_URL, selectedUrl)
        outState.putBoolean(SHOW_RELEASE_NOTES, showReleaseNotes)
    }

    override fun onDestroyView() {
        fetchJob?.cancel()
        _binding = null
        selected = null
        super.onDestroyView()
    }

    companion object {
        const val TAG = "DualScreenModFetcherDialogFragment"
        const val REQUEST_INSTALL = "InstallFetchedDualScreenMod"
        const val RELEASE = "Release"
        private const val TITLE_ID = "TitleId"
        private const val GAME_TITLE = "GameTitle"
        private const val SELECTED_URL = "SelectedUrl"
        private const val SHOW_RELEASE_NOTES = "ShowReleaseNotes"

        fun newInstance(titleId: String, gameTitle: String) =
            DualScreenModFetcherDialogFragment().apply {
                arguments = bundleOf(TITLE_ID to titleId, GAME_TITLE to gameTitle)
            }
    }
}
