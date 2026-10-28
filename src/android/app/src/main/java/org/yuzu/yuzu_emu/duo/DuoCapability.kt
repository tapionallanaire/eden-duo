// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import android.content.ComponentName
import android.content.Context
import android.content.pm.PackageManager
import android.hardware.display.DisplayManager

/**
 * Eden Duo: whether this device actually has a second panel, decided once per process by [init]
 * (YuzuApplication.onCreate) and forwarded to native (NativeLibrary.setDuoAvailable).
 *
 * On a single-screen device it is false and every dual-screen feature is off:
 * - the second-screen Presentation, AuxCompanionActivity and Screen Swap;
 * - the companion toggle (gesture, in-game menu, Quick Settings tile, whose component is disabled
 *   so the tile leaves the QS editor) and the No Companion handling;
 * - the Second Screen settings;
 * - the game library's companion catalog fetch (GitHub), add-on folder scans, card badges,
 *   "dual-screen games only" filter and "not in your library" section (with its GameTDB art);
 * - the Add-ons screen's dual-screen mod install and fetch, and the About page's GameTDB credit;
 * - natively: the DSMod runtime (its 60 Hz tick and package modules), load plans that patch the
 *   game's code, the module code cache and the presenter (VideoCore::DSMod::DuoEnabled). The dsm
 *   service stays registered: a guest asking for a missing service would wait forever.
 * Decided once because a mid-session change would leave native and UI disagreeing; a display
 * attached later takes effect on the next app start.
 */
object DuoCapability {
    @Volatile
    private var available: Boolean? = null

    /** Presented displays, i.e. panels the system can draw on (a panel that is off still counts). */
    fun displayCount(context: Context?): Int {
        val manager = context?.getSystemService(Context.DISPLAY_SERVICE) as? DisplayManager
            ?: return 1
        return manager.displays.size
    }

    /** Decides for this process; call once at app start. */
    fun init(context: Context): Boolean {
        val duo = displayCount(context) > 1
        available = duo
        syncTileComponent(context, duo)
        return duo
    }

    /**
     * A disabled tile service leaves the Quick Settings editor; only written when it differs,
     * because each change makes the system rescan the package.
     */
    private fun syncTileComponent(context: Context, enabled: Boolean) {
        val component = ComponentName(context, CompanionTileService::class.java)
        val pm = context.packageManager
        val wanted = if (enabled) {
            PackageManager.COMPONENT_ENABLED_STATE_DEFAULT
        } else {
            PackageManager.COMPONENT_ENABLED_STATE_DISABLED
        }
        if (pm.getComponentEnabledSetting(component) != wanted) {
            pm.setComponentEnabledSetting(component, wanted, PackageManager.DONT_KILL_APP)
        }
    }

    /** The decision made by [init]; a live count only if it has not run (e.g. unit tests). */
    fun isAvailable(context: Context?): Boolean = available ?: (displayCount(context) > 1)

    fun isSingleScreen(context: Context?): Boolean = !isAvailable(context)
}
