// SPDX-FileCopyrightText: Copyright 2026 Duet Plus contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.nativeplay

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.Html
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.fragment.app.Fragment
import androidx.lifecycle.lifecycleScope
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import java.util.UUID
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.duo.sync.DuetSaveSync
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.utils.ShaderRuns

/** One fragment-owned decision and original continuation, including cold external launches. */
internal class NativeLaunchGate(private val fragment: Fragment, private val emulate: () -> Unit) {
    private var phase = "choose"
    private var game: Game? = null
    private var custom = false
    private var token = ""
    private var contentKey = ""
    private var programId = ""
    private var playIdentity = ""
    private var failure = ""
    private var busy = false
    private var changingConfiguration = false
    private var returned = false
    private var firmwareChecked = false
    private var savesChecked = false
    private var shadersChecked = false
    private var dialog: AlertDialog? = null
    private var overlapWait: Job? = null
    private lateinit var appContext: android.content.Context
    private var recoveringReturn = false
    private val pollReturn = Runnable { observeReturn() }
    private val pollFallback = Runnable { recoverFallback() }
    private val handler = Handler(Looper.getMainLooper())
    private val finishReturn = Runnable {
        if (phase == "native" && returned) {
            // Keep ExitWatch's detailed reason when available; never close over a known failure.
            when (PreparedNativeGame.launchState(fragment.requireContext(), token)) {
                "stopped", "host_exit" -> fragment.requireActivity().finish()
                else -> nativeFailed(fragment.getString(R.string.native_prepare_interrupted))
            }
        }
    }
    val emulating get() = phase == "emulate"

    // Registration must happen when the fragment is constructed, never from a click/STARTED callback.
    private val choice = fragment.registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
        if (phase == "surface") {
            val data = result.data
            if (data == null || result.resultCode != Activity.RESULT_OK ||
                data.getStringExtra(NativePreparationActivity.SOURCE_PATH) != game?.path) {
                phase = "cancelled"
                fragment.requireActivity().finish()
            } else when (data.getStringExtra(NativeLaunchActivity.CHOICE)) {
                NativeLaunchActivity.EMULATE -> {
                    val nativeError = data.getStringExtra("native_error").orEmpty()
                    if (nativeError.isNotEmpty()) nativeFailed(nativeError)
                    else { phase = "preflight"; preflight() }
                }
                NativeLaunchActivity.NATIVE -> {
                    contentKey = data.getStringExtra(NativeLaunchActivity.CONTENT_KEY).orEmpty()
                    programId = data.getStringExtra(NativeLaunchActivity.PROGRAM_ID).orEmpty()
                    launchNative()
                }
                else -> {
                    phase = "cancelled"
                    fragment.requireActivity().finish()
                }
            }
        }
    }
    private val native = fragment.registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
        if (phase == "native") {
            returned = true
            observeReturn()
        }
    }

    fun restore(saved: Bundle?) {
        val state = saved?.getBundle("NativeLaunchGate") ?: return
        phase = state.getString("phase")?.takeUnless { it == "occupied" } ?: "choose"
        @Suppress("DEPRECATION")
        game = state.getParcelable("game")
        custom = state.getBoolean("custom")
        token = state.getString("token") ?: ""
        contentKey = state.getString("contentKey") ?: ""
        programId = state.getString(NativeLaunchActivity.PROGRAM_ID).orEmpty()
        playIdentity = state.getString("playIdentity") ?: ""
        failure = state.getString("failure") ?: ""
        returned = state.getBoolean("returned")
        firmwareChecked = state.getBoolean("firmware")
        savesChecked = state.getBoolean("saves")
        shadersChecked = state.getBoolean("shaders")
        ShaderRuns.precacheNext = state.getBoolean("precache")
    }

    fun save(out: Bundle) {
        out.putBundle("NativeLaunchGate", Bundle().apply {
            putString("phase", phase)
            putParcelable("game", game)
            putBoolean("custom", custom)
            putString("token", token)
            putString("contentKey", contentKey)
            putString(NativeLaunchActivity.PROGRAM_ID, programId)
            putString("playIdentity", playIdentity)
            putString("failure", failure)
            putBoolean("returned", returned)
            putBoolean("firmware", firmwareChecked)
            putBoolean("saves", savesChecked)
            putBoolean("shaders", shadersChecked)
            putBoolean("precache", ShaderRuns.precacheNext)
        })
    }

    fun decide(selected: Game, useCustom: Boolean, ordinary: Boolean) {
        game = selected
        appContext = fragment.requireContext().applicationContext
        custom = useCustom
        if (overlapWait?.isActive == true) return
        if (ordinary && phase != "native" && phase != "fallback" && NativeSaveSession.occupied(appContext, token)) {
            refuseOverlap()
            return
        }
        if (!ordinary && phase == "choose") phase = "emulate"
        when (phase) {
            "emulate" -> continueEmulated()
            "preflight" -> preflight()
            "failure" -> fragment.lifecycleScope.launchWhenStarted { showFailure() }
            "fallback" -> recoverFallback()
            "choose" -> choose()
            "install" -> launchNative()
        }
    }

    private fun refuseOverlap() {
        if (overlapWait?.isActive == true) return
        overlapWait = fragment.lifecycleScope.launchWhenResumed {
            dialog?.dismiss()
            dialog = MaterialAlertDialogBuilder(fragment.requireContext())
                .setTitle(R.string.native_launch_title)
                .setMessage(R.string.native_close_current_game)
                .setNegativeButton(android.R.string.cancel) { _, _ -> cancelWait() }
                .setOnCancelListener { cancelWait() }.show()
            try {
                waitForLaunchSlot { NativeSaveSession.occupied(appContext, token) }
            } catch (e: CancellationException) { throw e
            } catch (e: Exception) {
                dialog?.dismiss()
                fail(e.message ?: e.javaClass.simpleName)
                return@launchWhenResumed
            }
            dialog?.dismiss()
            dialog = null
            overlapWait = null
            if (!fragment.requireActivity().isFinishing) decide(checkNotNull(game), custom, true)
        }
    }

    private fun cancelWait() {
        overlapWait?.cancel()
        fragment.requireActivity().finish()
    }

    private fun choose() {
        val selected = game ?: return
        if (busy) return
        val mode = NativePlayPreferences.mode(appContext, selected.programIdHex)
        if (mode == NativePlayPreferences.Mode.EMULATED) {
            if (failure.isNotEmpty()) { showAdmissionFailure(); return }
            phase = "preflight"
            preflight()
            return
        }
        busy = true
        fragment.lifecycleScope.launch {
            val match: NativeGameSupport.Match?
            val nativeReady: Boolean
            var supportedTitle: Boolean
            try {
                supportedTitle = withContext(Dispatchers.IO) {
                    appContext.assets.open(NativeGameCatalog.ASSET).reader()
                        .use(NativeGameCatalog::read).recognizesTitle(selected.programIdHex)
                }
                if (supportedTitle && mode == NativePlayPreferences.Mode.ASK) {
                    busy = false
                    showChoice(selected)
                    return@launch
                }
                val result = withContext(Dispatchers.IO) {
                    val members = NativeGameSupport.members(appContext, selected)
                    if (members.choices.isNotEmpty()) supportedTitle = true
                    val candidate = members.match(appContext, selected.path)
                    val ready = candidate != null && mode == NativePlayPreferences.Mode.NATIVE &&
                        failure.isEmpty() && runCatching {
                            PreparedNativeGame.prepare(appContext, selected.path, candidate)
                        }.getOrNull() != null
                    candidate to ready
                }
                match = result.first
                nativeReady = result.second
            } catch (e: CancellationException) { busy = false; throw e
            } catch (_: Exception) {
                busy = false
                if (failure.isNotEmpty()) { showAdmissionFailure(); return@launch }
                phase = "preflight"
                preflight()
                return@launch
            }
            busy = false
            if (match == null) {
                if (failure.isNotEmpty()) { showAdmissionFailure(); return@launch }
                if (supportedTitle) { showChoice(selected); return@launch }
                // Unknown titles and unmatched indexed programs retain the emulated launch path.
                phase = "preflight"
                preflight()
            } else if (nativeReady) {
                contentKey = match.contentKey
                programId = match.profile.programId
                launchNative()
            } else {
                showChoice(selected)
            }
        }
    }

    private fun showChoice(selected: Game) {
        phase = "surface"
        try { choice.launch(NativeLaunchActivity.intent(appContext, selected, failure)) }
        catch (_: Exception) {
            phase = "cancelled"
            fragment.requireActivity().finish()
        }
    }

    private fun launchNative() {
        val selected = game ?: return
        if (busy) return
        if (playIdentity.isEmpty()) playIdentity = NativeSaveSession.identity(appContext)
        if (token.isEmpty()) token = UUID.randomUUID().toString()
        busy = true
        phase = "install"
        fragment.lifecycleScope.launch {
            try {
                if (NativeSaveSession.occupied(appContext, token)) {
                    refuseOverlap()
                    return@launch
                }
                // Resolve on the UI thread, without a suspension inside the config transaction.
                val resolved = NativeLaunchDriver.resolve(selected, custom)
                val intent = withContext(Dispatchers.IO) {
                    val match = NativeGameSupport.requireSelectedBuild(appContext, selected.path, programId, contentKey)
                    NativeSaveSession.reserveNative(appContext, selected.path, contentKey,
                        match.profile.engine, match.programIndex, match.profile.programId, playIdentity, token)
                    NativeSaveSession.acquire(appContext, token).use {
                        val prepared = checkNotNull(PreparedNativeGame.prepare(appContext, selected.path, match))
                        val installed = PreparedNativeGame.prepareDriver(appContext,
                            NativeLaunchDriver.validate(resolved), match.profile.engine)
                        NativePlayPreferences.rememberProfile(appContext, selected.programIdHex, match.profile)
                        prepared.putExtra("duet_gpu_driver", installed)
                    }
                }
                DuetSaveSync.get(appContext).beforeLaunch(fragment.requireActivity(), DuetSaveSync.SWITCH,
                    token, onFailure = { fail(it) }) {
                    if (!fragment.isAdded) return@beforeLaunch
                    try {
                        NativeSaveSession.attachIntent(appContext, token, intent)
                        // Schedule before launch, not on Activity destruction: survives parent process death.
                        DuetSaveSync.get(appContext).afterExit(DuetSaveSync.SWITCH)
                        returned = false
                        phase = "native"
                        // Explicit NEW_TASK breaks result-to task coupling before native crash cleanup.
                        native.launch(intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
                    } catch (e: NativeSaveSession.BusyException) { fail(e.message.orEmpty())
                    } catch (e: Exception) { nativeFailed(e.message ?: e.javaClass.simpleName) }
                }
            } catch (e: CancellationException) { cancelPreparationUnlessRecreating(); throw e
            } catch (e: NativeSaveSession.BusyException) { refuseOverlap()
            } catch (e: Exception) { nativeFailed(e.message ?: e.javaClass.simpleName)
            } finally { busy = false }
        }
    }

    fun resume() {
        if (phase == "failure") showFailure()
        if (phase == "fallback") { recoverFallback(); return }
        if (phase != "native") return
        PreparedNativeGame.watchExits(fragment.requireActivity()) { path, reason ->
            if (phase == "native" && path == game?.path &&
                PreparedNativeGame.launchState(fragment.requireContext(), token).isNotEmpty()) nativeFailed(reason)
        }
        if (returned) observeReturn()
    }

    private fun observeReturn() {
        if (phase != "native" || recoveringReturn || !fragment.isAdded) return
        recoveringReturn = true
        fragment.lifecycleScope.launch {
            try {
                val released = withContext(Dispatchers.IO) { NativeSaveSession.recoverAfterExit(appContext) }
                if (phase != "native") return@launch
                if (!released) {
                    handler.postDelayed(pollReturn, 250)
                    return@launch
                }
                DuetSaveSync.get(appContext).afterExit(DuetSaveSync.SWITCH)
                when (PreparedNativeGame.launchState(appContext, token)) {
                    "stopped" -> fragment.requireActivity().finish()
                    "" -> nativeFailed(fragment.getString(R.string.native_prepare_interrupted))
                    else -> {
                        handler.removeCallbacks(finishReturn)
                        handler.postDelayed(finishReturn, 10000)
                    }
                }
            } catch (e: CancellationException) { throw e
            } catch (e: Exception) { fail(e.message ?: e.javaClass.simpleName) }
            finally { recoveringReturn = false }
        }
    }

    fun pause() {
        PreparedNativeGame.stopWatchingExits()
        handler.removeCallbacks(finishReturn)
        handler.removeCallbacks(pollReturn)
        handler.removeCallbacks(pollFallback)
    }
    fun destroy() {
        changingConfiguration = fragment.activity?.isChangingConfigurations == true
        overlapWait?.cancel()
        pause(); dialog?.dismiss(); dialog = null
        if (!changingConfiguration && phase != "native")
            NativeSaveSession.cancel(token)
    }

    private fun fail(reason: String) {
        busy = false
        NativeSaveSession.cancel(token)
        token = ""
        playIdentity = ""
        phase = "failure"
        failure = reason
        handler.removeCallbacks(finishReturn)
        fragment.lifecycleScope.launchWhenStarted { showFailure() }
    }

    private fun showFailure() {
        if (phase == "failure") showAdmissionFailure()
    }

    private fun showAdmissionFailure() {
        phase = "failure"
        dialog?.dismiss()
        dialog = MaterialAlertDialogBuilder(fragment.requireContext())
            .setTitle("Cannot open shared saves")
            .setMessage(failure)
            .setPositiveButton("Retry") { _, _ ->
                failure = ""
                phase = "preflight"
                preflight()
            }
            .setNegativeButton(android.R.string.cancel) { _, _ -> fragment.requireActivity().finish() }
            .setOnCancelListener { fragment.requireActivity().finish() }
            .setOnDismissListener { dialog = null }.show()
    }

    private fun nativeFailed(reason: String) {
        busy = false
        NativeSaveSession.cancel(token)
        if (playIdentity.isEmpty()) playIdentity = NativeSaveSession.identity(appContext)
        failure = reason
        phase = "fallback"
        firmwareChecked = false
        savesChecked = false
        shadersChecked = false
        handler.removeCallbacks(finishReturn)
        handler.removeCallbacks(pollReturn)
        handler.removeCallbacks(pollFallback)
        handler.post(pollFallback)
    }

    /** No native retry: first prove writer release and journal recovery, then ordinary emulation preflight. */
    private fun recoverFallback() {
        if (phase != "fallback" || !fragment.isAdded) return
        if (recoveringReturn) {
            handler.removeCallbacks(pollFallback)
            handler.postDelayed(pollFallback, 250)
            return
        }
        recoveringReturn = true
        fragment.lifecycleScope.launchWhenStarted {
            try {
                val released = withContext(Dispatchers.IO) { NativeSaveSession.recoverAfterExit(appContext) }
                if (!released) {
                    handler.postDelayed(pollFallback, 250)
                    return@launchWhenStarted
                }
                token = ""
                android.widget.Toast.makeText(appContext,
                    "$failure\nPlaying emulated instead.", android.widget.Toast.LENGTH_LONG).show()
                phase = "preflight"
                preflight()
            } catch (e: CancellationException) { throw e
            } catch (e: Exception) {
                fail("Native play failed: $failure\n\nShared saves could not be recovered safely: ${e.message ?: e.javaClass.simpleName}")
            } finally { recoveringReturn = false }
        }
    }

    private fun preflight() {
        val selected = game ?: return
        if (playIdentity.isEmpty()) playIdentity = NativeSaveSession.identity(appContext)
        if (token.isEmpty()) token = UUID.randomUUID().toString()
        fragment.lifecycleScope.launchWhenStarted {
            if (NativeSaveSession.occupied(appContext, token)) {
                refuseOverlap()
                return@launchWhenStarted
            }
            try { ensureEmulatedReservation() }
            catch (e: CancellationException) { cancelPreparationUnlessRecreating(); throw e }
            catch (e: Exception) { fail(e.message ?: e.javaClass.simpleName); return@launchWhenStarted }
            if (!firmwareChecked) {
                if (NativeLibrary.gameRequiresFirmware(selected.programId) && !NativeLibrary.isFirmwareAvailable()) {
                    MaterialAlertDialogBuilder(fragment.requireContext())
                        .setTitle(R.string.loader_requires_firmware)
                        .setMessage(Html.fromHtml(fragment.getString(R.string.loader_requires_firmware_description), Html.FROM_HTML_MODE_LEGACY))
                        .setPositiveButton(android.R.string.ok) { _, _ -> firmwareChecked = true; preflight() }
                        .setNegativeButton(android.R.string.cancel) { _, _ -> fragment.requireActivity().finish() }
                        .setOnCancelListener { fragment.requireActivity().finish() }.show()
                    return@launchWhenStarted
                }
                firmwareChecked = true
            }
            if (!savesChecked) {
                DuetSaveSync.get(fragment.requireContext()).beforeLaunch(fragment.requireActivity(),
                    DuetSaveSync.SWITCH, token, onFailure = { fail(it) }) {
                    if (fragment.isAdded) { savesChecked = true; preflight() }
                }
                return@launchWhenStarted
            }
            if (!shadersChecked) {
                ShaderRuns.askThenLaunch(fragment.requireContext(), selected, cancel = {
                    phase = "cancelled"
                    fragment.requireActivity().finish()
                }) {
                    if (fragment.isAdded) { shadersChecked = true; preflight() }
                }
                return@launchWhenStarted
            }
            phase = "emulate"
            emulate()
        }
    }

    private suspend fun ensureEmulatedReservation() = withContext(Dispatchers.IO) {
        if (NativeSaveSession.isEmulating(token)) return@withContext
        NativeSaveSession.reserveEmulated(appContext, playIdentity, token)
        NativeSaveSession.acquire(appContext, token).use {
            fragment.activity?.intent?.getStringExtra(org.yuzu.yuzu_emu.utils.WarmUp.EXTRA_SAVE_IDENTITY)?.let {
                NativeSaveSession.assertIdentity(appContext, it)
            }
        }
    }

    private fun continueEmulated() {
        if (playIdentity.isEmpty()) playIdentity = NativeSaveSession.identity(appContext)
        if (token.isEmpty()) token = UUID.randomUUID().toString()
        fragment.lifecycleScope.launch {
            try { ensureEmulatedReservation(); emulate() }
            catch (e: CancellationException) { cancelPreparationUnlessRecreating(); throw e }
            catch (e: Exception) { fail(e.message ?: e.javaClass.simpleName) }
        }
    }

    fun runEmulated(block: () -> Unit) {
        if (!NativeSaveSession.hasReservation(token)) NativeSaveSession.reserveEmulated(appContext, playIdentity, token)
        try { NativeSaveSession.runEmulated(appContext, token, block) }
        catch (e: NativeSaveSession.BusyException) {
            handler.post { if (fragment.isAdded) fail(e.message.orEmpty()) }
        }
        finally { DuetSaveSync.get(appContext).afterExit(DuetSaveSync.SWITCH) }
    }

    private fun cancelPreparationUnlessRecreating() {
        if (!changingConfiguration && fragment.activity?.isChangingConfigurations != true)
            NativeSaveSession.cancel(token)
    }
}
