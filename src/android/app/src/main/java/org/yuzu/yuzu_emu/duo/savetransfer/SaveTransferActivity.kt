// SPDX-FileCopyrightText: Copyright 2026 tapionallanaire
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.duo.savetransfer

import android.os.Bundle
import android.view.KeyEvent
import androidx.appcompat.app.AppCompatActivity
import org.yuzu.yuzu_emu.utils.ThemeHelper

/** Hosts the save transfer screen from the main menu. */
class SaveTransferActivity : AppCompatActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        ThemeHelper.setTheme(this)
        super.onCreate(savedInstanceState)
        if (savedInstanceState == null) {
            supportFragmentManager.beginTransaction()
                .replace(android.R.id.content, SaveTransferFragment.newInstance(false), SaveTransferFragment.TAG)
                .commit()
        }
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val screen = supportFragmentManager.findFragmentByTag(SaveTransferFragment.TAG) as? SaveTransferFragment
        return screen?.onControllerKey(event) == true || super.dispatchKeyEvent(event)
    }
}
