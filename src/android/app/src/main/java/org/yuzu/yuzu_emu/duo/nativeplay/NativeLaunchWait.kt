// SPDX-License-Identifier: GPL-3.0-or-later
package org.yuzu.yuzu_emu.duo.nativeplay

import kotlinx.coroutines.delay

/** A busy probe never grants ownership: the original launch must still acquire its save reservation. */
internal suspend fun waitForLaunchSlot(occupied: () -> Boolean) {
    do { delay(500) } while (occupied())
}
