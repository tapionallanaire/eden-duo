// SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.utils

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class IntentGameSetupRulesTest {
    @Test
    fun existingCurrentGameIsNeverReplaced() {
        assertEquals("current", IntentGameSetupRules.deferredGame("current", "args", "intent"))
        assertEquals("current", IntentGameSetupRules.deferredGame("current", null, null))
    }

    @Test
    fun argsGameWinsOverIntentGameWhenCurrentIsUnset() {
        assertEquals("args", IntentGameSetupRules.deferredGame(null, "args", "intent"))
    }

    @Test
    fun intentGameIsTheLastResortWhenNeitherCurrentNorArgsAreSet() {
        assertEquals("intent", IntentGameSetupRules.deferredGame(null, null, "intent"))
    }

    @Test
    fun allUnsetStaysNull() {
        assertNull(IntentGameSetupRules.deferredGame<String>(null, null, null))
    }

    @Test
    fun onlyOrdinaryGamesReachNativeDecision() {
        val title = "0100F2C0115B6000"
        assertEquals(true, IntentGameSetupRules.ordinaryLaunch(title, 0, false, false, false))
        assertEquals(false, IntentGameSetupRules.ordinaryLaunch("0100000000001000", 0, false, false, false))
        assertEquals(false, IntentGameSetupRules.ordinaryLaunch(title, 5, false, false, false))
        assertEquals(false, IntentGameSetupRules.ordinaryLaunch(title, 0, true, false, false))
        assertEquals(false, IntentGameSetupRules.ordinaryLaunch(title, 0, false, true, false))
        assertEquals(false, IntentGameSetupRules.ordinaryLaunch(title, 0, false, false, true))
    }
}
