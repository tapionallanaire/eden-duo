// SPDX-FileCopyrightText: Copyright 2026 igawa6
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo

import android.content.Context
import android.hardware.display.DisplayManager

/**
 * Eden Duo: whether this device actually has a second panel.
 *
 * Everything Duo-shaped - the second-screen Presentation, AuxCompanionActivity, the companion
 * toggle (gesture, Quick Settings tile, in-game menu), the Second Screen settings and the native
 * DSMod presenter - hangs off this one answer. On a single-screen device it is false, so none of
 * those are created, drawn or offered: there is no window to put anywhere, and creating one only
 * costs a Vulkan instance and a display listener per session.
 */
object DuoCapability {
    /** Presented displays, i.e. panels the system can draw on (a panel that is off still counts). */
    fun displayCount(context: Context?): Int {
        val manager = context?.getSystemService(Context.DISPLAY_SERVICE) as? DisplayManager
            ?: return 1
        return manager.displays.size
    }

    fun isAvailable(context: Context?): Boolean = displayCount(context) > 1

    fun isSingleScreen(context: Context?): Boolean = !isAvailable(context)
}
