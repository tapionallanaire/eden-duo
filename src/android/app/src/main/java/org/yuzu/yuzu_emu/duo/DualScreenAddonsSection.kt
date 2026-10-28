// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import android.content.Context
import android.net.Uri
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.os.BundleCompat
import androidx.fragment.app.Fragment
import androidx.lifecycle.LifecycleOwner
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.fragments.ContentTypeSelectionDialogFragment
import org.yuzu.yuzu_emu.fragments.DualScreenModFetcherDialogFragment
import org.yuzu.yuzu_emu.fragments.MessageDialogFragment
import org.yuzu.yuzu_emu.fragments.ProgressDialogFragment
import org.yuzu.yuzu_emu.model.AddonViewModel
import org.yuzu.yuzu_emu.model.DualScreenModRelease
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.model.PatchType
import org.yuzu.yuzu_emu.utils.DualScreenModRepository
import org.yuzu.yuzu_emu.utils.DualScreenPackageInstaller
import org.yuzu.yuzu_emu.utils.Log
import org.yuzu.yuzu_emu.utils.NativeConfig
import java.io.File

/**
 * Eden Duo: the dual-screen package part of the Add-ons screen -- install from a file, fetch from
 * the package repository, and the add-on on/off bookkeeping after an install.
 *
 * Construct it as a property of [fragment] (it registers an activity result launcher, which must
 * happen before the fragment is created) and call [registerResultListeners] from onViewCreated.
 */
class DualScreenAddonsSection(
    private val fragment: Fragment,
    private val game: () -> Game,
    private val addonViewModel: () -> AddonViewModel
) {
    private companion object {
        /** The type PatchManager reports for a folder holding dualscreen/manifest.json. */
        const val DUAL_SCREEN_ADDON_TYPE = "Dual screen mod"
    }

    private val installDualScreenPackageFile =
        fragment.registerForActivityResult(ActivityResultContracts.OpenDocument()) { result ->
            if (result != null) {
                installDualScreenPackage(result)
            }
        }

    private fun installDualScreenPackage(uri: Uri? = null, release: DualScreenModRelease? = null) {
        // Eden Duo: the OpenDocument launcher is registered regardless of screen count, so a file
        // picker opened in a two-screen process can be re-delivered here after the process
        // restarts single-screen; refuse to install on one screen.
        if (DuoCapability.isSingleScreen(fragment.context)) return
        val activity = fragment.requireActivity()
        val game = game()
        // The installer writes through java.io.File; a linked (SAF) load folder cannot be written.
        if (game.addonDir.startsWith("content://")) {
            MessageDialogFragment.newInstance(
                activity,
                titleId = R.string.dual_screen_mod_install_failed,
                descriptionId = R.string.dual_screen_mod_linked_folder
            ).show(fragment.parentFragmentManager, MessageDialogFragment.TAG)
            return
        }
        ProgressDialogFragment.newInstance(
            activity,
            R.string.installing_dual_screen_mod,
            true
        ) { progressCallback, messageCallback ->
            val addonDirectory = File(game.addonDir)
            val result = if (release == null) {
                DualScreenPackageInstaller.install(
                    context = activity,
                    uri = requireNotNull(uri),
                    gameTitleId = game.programIdHex,
                    addonDirectory = addonDirectory,
                    progressCallback = progressCallback
                )
            } else {
                try {
                    messageCallback(
                        activity.getString(R.string.dual_screen_mod_downloading, release.assetName)
                    )
                    val archive = DualScreenModRepository.download(
                        release,
                        activity.cacheDir,
                        progressCallback
                    )
                    try {
                        messageCallback(activity.getString(R.string.installing_dual_screen_mod))
                        DualScreenPackageInstaller.install(
                            archive,
                            release.assetName,
                            game.programIdHex,
                            addonDirectory,
                            progressCallback
                        )
                    } finally {
                        archive.delete()
                    }
                } catch (_: DualScreenModRepository.DownloadCancelledException) {
                    return@newInstance activity.getString(
                        R.string.dual_screen_mod_install_cancelled
                    )
                } catch (e: Exception) {
                    if (progressCallback(0, 0)) {
                        return@newInstance activity.getString(
                            R.string.dual_screen_mod_install_cancelled
                        )
                    }
                    return@newInstance MessageDialogFragment.newInstance(
                        activity,
                        titleId = R.string.dual_screen_mod_install_failed,
                        descriptionString = activity.getString(
                            R.string.dual_screen_mod_fetch_failed,
                            e.message.orEmpty()
                        )
                    )
                }
            }
            when (result) {
                is DualScreenPackageInstaller.Result.Installed -> {
                    addonViewModel().persistAddonStates()
                    val notes = applyDualScreenAddonStates(activity, game.programId, result)
                    val problem = verifyDualScreenInstall(activity, game, addonDirectory, result)
                    addonViewModel().refreshAddons(force = true)
                    if (problem != null) {
                        MessageDialogFragment.newInstance(
                            activity,
                            titleId = R.string.dual_screen_mod_install_failed,
                            descriptionString = problem
                        )
                    } else if (notes.isEmpty()) {
                        activity.getString(R.string.dual_screen_mod_installed)
                    } else {
                        MessageDialogFragment.newInstance(
                            activity,
                            titleId = R.string.dual_screen_mod_installed,
                            descriptionString = notes
                        )
                    }
                }

                is DualScreenPackageInstaller.Result.Failed -> {
                    if (result.error == DualScreenPackageInstaller.Error.Cancelled) {
                        activity.getString(R.string.dual_screen_mod_install_cancelled)
                    } else {
                        MessageDialogFragment.newInstance(
                            activity,
                            titleId = R.string.dual_screen_mod_install_failed,
                            descriptionString = dualScreenInstallError(activity, result)
                        )
                    }
                }
            }
        }.show(fragment.parentFragmentManager, ProgressDialogFragment.TAG)
    }

    /**
     * Keeps the second screen on the package just installed, through the add-on on/off list
     * (reversible from the Add-ons screen):
     * - an update keeps the user's choice: if the version it replaced was off, the new one is too;
     * - an older version that could not be deleted is turned off;
     * - other folders holding only a dual-screen package for this game (hand-copied ones
     *   included) are turned off, because the runtime uses the first enabled one by name.
     * Folders that also hold other mods are left alone and named in the returned note.
     */
    private fun applyDualScreenAddonStates(
        context: Context,
        programId: String,
        result: DualScreenPackageInstaller.Result.Installed
    ): String {
        val disabled = NativeConfig.getDisabledAddons(programId).toMutableList()
        val replaced = result.removed + result.notRemoved
        if (replaced.any { it in disabled }) {
            if (result.folderName !in disabled) {
                disabled += result.folderName
            }
        } else {
            // A fresh install (or a reinstall over the same folder) is meant to be used: a
            // leftover entry for this folder name must not leave the second screen idle.
            disabled.remove(result.folderName)
        }
        disabled.removeAll(result.removed.toSet())

        val turnedOff = mutableListOf<String>()
        for (name in result.notRemoved +
            result.otherPackages.filter { it.onlyDualScreen }.map { it.folderName }) {
            if (name !in disabled) {
                disabled += name
                turnedOff += name
            }
        }
        val mayShadow = result.otherPackages
            .filter { !it.onlyDualScreen && it.folderName !in disabled }
            .map { it.folderName }
            .filter { it < result.folderName }
        NativeConfig.setDisabledAddons(programId, disabled.toTypedArray())
        NativeConfig.saveGlobalConfig()

        val notes = mutableListOf<String>()
        if (result.notRemoved.isNotEmpty()) {
            notes += context.getString(
                R.string.dual_screen_mod_old_not_removed,
                result.notRemoved.joinToString()
            )
        }
        val othersOff = turnedOff - result.notRemoved.toSet()
        if (othersOff.isNotEmpty()) {
            notes += context.getString(
                R.string.dual_screen_mod_others_disabled,
                othersOff.joinToString()
            )
        }
        if (mayShadow.isNotEmpty()) {
            notes += context.getString(
                R.string.dual_screen_mod_others_enabled,
                mayShadow.joinToString()
            )
        }
        return notes.joinToString("\n\n")
    }

    /**
     * Asks the same scanner the Add-ons list uses whether the package just installed is visible.
     * Returns null when it is listed, otherwise what is wrong (also logged), so a package the
     * list and the second screen cannot see is never reported as installed.
     */
    private fun verifyDualScreenInstall(
        context: Context,
        game: Game,
        addonDirectory: File,
        result: DualScreenPackageInstaller.Result.Installed
    ): String? {
        val folder = File(addonDirectory, result.folderName)
        val scanned = NativeLibrary.getModLoadDirectory(game.programId)
        val patches = NativeLibrary.getPatchesForFile(game.path, game.programId)
        val problem = when {
            patches == null -> context.getString(R.string.dual_screen_mod_check_no_scan)
            patches.any {
                PatchType.from(it.type) == PatchType.Mod && it.name == result.folderName &&
                    it.version.contains(DUAL_SCREEN_ADDON_TYPE)
            } -> null
            !File(folder, "dualscreen/manifest.json").isFile ->
                context.getString(R.string.dual_screen_mod_check_missing, folder.path)
            else -> context.getString(
                R.string.dual_screen_mod_check_not_listed,
                folder.path,
                scanned.ifEmpty { "-" }
            )
        }
        val scannedText = scanned.ifEmpty { "-" }
        Log.info(
            "[DSModInstaller] self-check ${result.folderName}: " +
                "installed under ${addonDirectory.path}, scanner reads $scannedText, " +
                "${patches?.size ?: "no"} add-on(s) listed -> ${problem ?: "listed"}"
        )
        return problem
    }

    /** The message for a failed install: what went wrong, then the installer's detail. */
    private fun dualScreenInstallError(
        context: Context,
        result: DualScreenPackageInstaller.Result.Failed
    ): String {
        val detail = result.detail
        val message = when (result.error) {
            DualScreenPackageInstaller.Error.RuntimeTooOld -> return context.getString(
                R.string.dual_screen_mod_runtime_too_old,
                if (result.requiredRuntime == DualScreenPackageInstaller.UNKNOWN_RUNTIME) {
                    "?"
                } else {
                    result.requiredRuntime.toString()
                },
                result.runtime
            )
            DualScreenPackageInstaller.Error.MissingPlatformLibrary ->
                return context.getString(R.string.dual_screen_mod_missing_platform_library, detail)
            DualScreenPackageInstaller.Error.CannotWrite ->
                return context.getString(R.string.dual_screen_mod_cannot_write, detail)
            DualScreenPackageInstaller.Error.VersionMismatch ->
                return context.getString(R.string.dual_screen_mod_version_mismatch, detail)
            DualScreenPackageInstaller.Error.ChecksumMismatch ->
                context.getString(R.string.dual_screen_mod_checksum_mismatch)
            DualScreenPackageInstaller.Error.ArchiveTooLarge ->
                context.getString(R.string.dual_screen_mod_archive_too_large)
            DualScreenPackageInstaller.Error.MalformedArchive ->
                context.getString(R.string.dual_screen_mod_malformed_archive)
            DualScreenPackageInstaller.Error.UnsafeEntry,
            DualScreenPackageInstaller.Error.DuplicateEntry,
            DualScreenPackageInstaller.Error.InvalidLayout,
            DualScreenPackageInstaller.Error.InvalidMetadata ->
                context.getString(R.string.dual_screen_mod_invalid_package)
            DualScreenPackageInstaller.Error.TitleMismatch ->
                context.getString(R.string.dual_screen_mod_title_mismatch)
            DualScreenPackageInstaller.Error.Cancelled,
            DualScreenPackageInstaller.Error.InstallFailed ->
                context.getString(R.string.dual_screen_mod_install_failed_description)
        }
        return if (detail.isEmpty()) {
            message
        } else {
            context.getString(R.string.dual_screen_mod_error_detail, message, detail)
        }
    }

    /** Listens for the dual-screen entries of the content-type picker and the fetcher dialog. */
    fun registerResultListeners(owner: LifecycleOwner) {
        // One screen: the content-type picker offers no dual-screen entry, and nothing else may
        // open the fetcher or the installer.
        if (DuoCapability.isSingleScreen(fragment.context)) return
        fragment.parentFragmentManager.setFragmentResultListener(
            ContentTypeSelectionDialogFragment.REQUEST_INSTALL_DUAL_SCREEN_MOD,
            owner
        ) { _, _ ->
            installDualScreenPackageFile.launch(
                arrayOf("application/zip", "application/octet-stream", "*/*")
            )
        }
        fragment.parentFragmentManager.setFragmentResultListener(
            ContentTypeSelectionDialogFragment.REQUEST_FETCH_DUAL_SCREEN_MOD,
            owner
        ) { _, _ ->
            DualScreenModFetcherDialogFragment.newInstance(game().programIdHex, game().title)
                .show(fragment.parentFragmentManager, DualScreenModFetcherDialogFragment.TAG)
        }
        fragment.parentFragmentManager.setFragmentResultListener(
            DualScreenModFetcherDialogFragment.REQUEST_INSTALL,
            owner
        ) { _, bundle ->
            BundleCompat.getParcelable(
                bundle,
                DualScreenModFetcherDialogFragment.RELEASE,
                DualScreenModRelease::class.java
            )?.let { installDualScreenPackage(release = it) }
        }
    }
}
