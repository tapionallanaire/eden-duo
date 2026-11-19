// SPDX-License-Identifier: GPL-3.0-or-later
package org.yuzu.yuzu_emu.duo.nativeplay

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class NativeCompilationStateTest {
    @Test fun liveCompilerNeverSettlesFromAnOldTerminalStatus() {
        assertFalse(nativeCompilerHasSettled(true, 0, 10_000))
        assertFalse(nativeCompilerHasSettled(true, 100, 100_000))
    }

    @Test fun delayedServiceStartKeepsItsGracePeriodUntilTheBoundary() {
        assertFalse(nativeCompilerHasSettled(false, 100, 100))
        assertFalse(nativeCompilerHasSettled(false, 100, 5_099))
        assertTrue(nativeCompilerHasSettled(false, 100, 5_100))
    }

    @Test fun idleAndRebootedRequestsDoNotWaitForever() {
        assertTrue(nativeCompilerHasSettled(false, 0, 0))
        assertTrue(nativeCompilerHasSettled(false, 100, 99))
    }
}
