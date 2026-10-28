// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.yuzu.yuzu_emu.fragments

import android.app.Dialog
import android.content.DialogInterface
import android.os.Bundle
import androidx.appcompat.app.AlertDialog
import androidx.core.view.isVisible
import androidx.fragment.app.DialogFragment
import androidx.fragment.app.activityViewModels
import androidx.preference.PreferenceManager
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.model.AddonViewModel

class ContentTypeSelectionDialogFragment : DialogFragment() {
    private val addonViewModel: AddonViewModel by activityViewModels()

    private val preferences get() =
        PreferenceManager.getDefaultSharedPreferences(YuzuApplication.appContext)

    private var selectedItem = 0

    override fun onCreateDialog(savedInstanceState: Bundle?): Dialog {
        // Eden Duo: one screen has no use for dual-screen mods, so no install or fetch entry.
        val duo = org.yuzu.yuzu_emu.duo.DuoCapability.isAvailable(requireContext())
        val launchOptions = listOfNotNull(
            getString(R.string.updates_and_dlc),
            getString(R.string.mods_and_cheats),
            if (duo) getString(R.string.dual_screen_mods) else null
        ).toTypedArray()

        if (savedInstanceState != null) {
            selectedItem = savedInstanceState.getInt(SELECTED_ITEM)
                .coerceIn(0, launchOptions.lastIndex)
        }

        return MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.select_content_type)
            .setPositiveButton(R.string.install) { _: DialogInterface, _: Int ->
                when (selectedItem) {
                    0 -> parentFragmentManager.setFragmentResult(
                        REQUEST_INSTALL_GAME_UPDATE,
                        Bundle()
                    )
                    1 -> {
                        if (!preferences.getBoolean(MOD_NOTICE_SEEN, false)) {
                            preferences.edit().putBoolean(MOD_NOTICE_SEEN, true).apply()
                            addonViewModel.showModNoticeDialog(true)
                            return@setPositiveButton
                        }
                        addonViewModel.showModInstallPicker(true)
                    }
                    2 -> parentFragmentManager.setFragmentResult(
                        REQUEST_INSTALL_DUAL_SCREEN_MOD,
                        Bundle()
                    )
                }
            }
            .setSingleChoiceItems(launchOptions, selectedItem) { dialog: DialogInterface, i: Int ->
                selectedItem = i
                (dialog as AlertDialog).getButton(DialogInterface.BUTTON_NEUTRAL).isVisible = i == 2
            }
            .apply {
                if (duo) {
                    setNeutralButton(R.string.fetch) { _, _ ->
                        parentFragmentManager.setFragmentResult(REQUEST_FETCH_DUAL_SCREEN_MOD, Bundle())
                    }
                }
            }
            .setNegativeButton(android.R.string.cancel, null)
            .create()
    }

    override fun onStart() {
        super.onStart()
        (requireDialog() as AlertDialog).getButton(DialogInterface.BUTTON_NEUTRAL).isVisible =
            selectedItem == 2
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putInt(SELECTED_ITEM, selectedItem)
    }

    companion object {
        const val TAG = "ContentTypeSelectionDialogFragment"
        const val REQUEST_INSTALL_GAME_UPDATE = "RequestInstallGameUpdate"
        const val REQUEST_INSTALL_DUAL_SCREEN_MOD = "RequestInstallDualScreenMod"
        const val REQUEST_FETCH_DUAL_SCREEN_MOD = "RequestFetchDualScreenMod"

        private const val SELECTED_ITEM = "SelectedItem"
        private const val MOD_NOTICE_SEEN = "ModNoticeSeen"
    }
}
