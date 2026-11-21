// SPDX-License-Identifier: GPL-3.0-or-later
package org.yuzu.yuzu_emu.duo.nativeplay

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.Assert.assertEquals
import org.junit.Test

class NativeLaunchWaitTest {
    @Test fun retryContinuesOnceAfterReleaseButNeverAfterCancel() = runBlocking {
        var occupied = true
        var launches = 0
        val inspected = CompletableDeferred<Unit>()
        val cancelled = launch {
            waitForLaunchSlot { inspected.complete(Unit); occupied }
            launches++
        }
        withTimeout(5000) { inspected.await() }
        cancelled.cancelAndJoin()
        val secondInspection = CompletableDeferred<Unit>()
        val retry = launch {
            waitForLaunchSlot { secondInspection.complete(Unit); occupied }
            launches++
        }
        withTimeout(5000) { secondInspection.await() }
        assertEquals(0, launches)
        occupied = false
        withTimeout(5000) { retry.join() }
        assertEquals(1, launches)
    }
}
