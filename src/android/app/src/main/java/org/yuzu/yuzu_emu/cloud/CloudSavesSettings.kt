// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.cloud

import android.content.Context
import androidx.core.content.edit
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.YuzuApplication
import org.yuzu.yuzu_emu.features.settings.model.AbstractBooleanSetting
import org.yuzu.yuzu_emu.features.settings.model.AbstractIntSetting
import org.yuzu.yuzu_emu.features.settings.model.view.HeaderSetting
import org.yuzu.yuzu_emu.features.settings.model.view.RunnableSetting
import org.yuzu.yuzu_emu.features.settings.model.view.SettingsItem
import org.yuzu.yuzu_emu.features.settings.model.view.SingleChoiceSetting
import org.yuzu.yuzu_emu.features.settings.model.view.SwitchSetting

/**
 * Eden Duo: the Google Drive saves section of the settings screen.
 *
 * Sync is per title and automatic: a game's save is pulled before it starts and pushed after it
 * stops. What the user chooses here is whether that happens at all, and what to do when this device
 * and the cloud both changed since the last sync.
 */
object CloudSavesSettings {
    private const val PREFS = "cloud_saves"
    private const val KEY_ENABLED = "enabled"
    private const val KEY_POLICY = "conflict_policy"

    /** What tapping a row asks the activity to do; the fragment owns the activity plumbing. */
    enum class Action { SIGN_IN, SIGN_OUT }

    private val prefs get() = YuzuApplication.appContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    fun enabled(context: Context): Boolean = prefs.getBoolean(KEY_ENABLED, true)

    fun setEnabled(context: Context, value: Boolean) = prefs.edit { putBoolean(KEY_ENABLED, value) }

    /** Conflict policy values match [Policy]: 0 ask, 1 keep this device, 2 use the cloud. */
    fun policy(context: Context): SaveSync.Policy =
        SaveSync.Policy.entries.getOrElse(prefs.getInt(KEY_POLICY, 0)) { SaveSync.Policy.ASK }

    fun setPolicy(context: Context, policy: SaveSync.Policy) =
        prefs.edit { putInt(KEY_POLICY, policy.ordinal) }

    val setting: AbstractBooleanSetting = object : AbstractBooleanSetting {
        override fun getBoolean(needsGlobal: Boolean): Boolean = enabled(YuzuApplication.appContext)
        override fun setBoolean(value: Boolean) = setEnabled(YuzuApplication.appContext, value)
        override val key: String = KEY_ENABLED
        override val isRuntimeModifiable: Boolean = true
        override val pairedSettingKey: String = ""
        override val isSwitchable: Boolean = false
        override var global: Boolean = true
        override val isSaveable: Boolean = true
        override val defaultValue: Boolean = true
        override fun getValueAsString(needsGlobal: Boolean): String = getBoolean(needsGlobal).toString()
        override fun reset() = setBoolean(defaultValue)
    }

    val policySetting: AbstractIntSetting = object : AbstractIntSetting {
        override fun getInt(needsGlobal: Boolean): Int = policy(YuzuApplication.appContext).ordinal
        override fun setInt(value: Int) =
            setPolicy(YuzuApplication.appContext, SaveSync.Policy.entries[value])
        override val key: String = KEY_POLICY
        override val isRuntimeModifiable: Boolean = true
        override val pairedSettingKey: String = ""
        override val isSwitchable: Boolean = false
        override var global: Boolean = true
        override val isSaveable: Boolean = true
        override val defaultValue: Int = 0
        override fun getValueAsString(needsGlobal: Boolean): String = getInt(needsGlobal).toString()
        override fun reset() = setInt(defaultValue)
    }

    /** The account row reads the signed-in state, so it says something true on every visit. */
    private fun accountDescription(context: Context): String {
        if (!CloudAuth.isConfigured(context)) {
            return context.getString(R.string.cloud_not_configured)
        }
        val email = CloudAuth.email(context)
        return if (email == null) {
            context.getString(R.string.cloud_sign_in_description)
        } else {
            context.getString(R.string.cloud_signed_in_as, email)
        }
    }

    fun addTo(sl: ArrayList<SettingsItem>, context: Context, onAction: (Action) -> Unit) {
        val signedIn = CloudAuth.isConfigured(context) && CloudAuth.email(context) != null
        sl.apply {
            add(HeaderSetting(R.string.cloud_saves))
            add(
                RunnableSetting(
                    titleId = R.string.cloud_account,
                    descriptionString = accountDescription(context),
                    isRunnable = true
                ) {
                    onAction(if (signedIn) Action.SIGN_OUT else Action.SIGN_IN)
                }
            )
            add(
                SwitchSetting(
                    setting,
                    titleId = R.string.cloud_sync_enabled,
                    descriptionId = R.string.cloud_sync_enabled_description
                )
            )
            add(
                SingleChoiceSetting(
                    policySetting,
                    titleId = R.string.cloud_conflict_policy,
                    descriptionId = R.string.cloud_conflict_policy_description,
                    choicesId = R.array.cloudConflictChoices,
                    valuesId = R.array.cloudConflictValues
                )
            )
        }
    }
}
