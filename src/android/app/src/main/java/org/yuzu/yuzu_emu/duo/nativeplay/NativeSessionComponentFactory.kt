// SPDX-License-Identifier: GPL-3.0-or-later
package org.yuzu.yuzu_emu.duo.nativeplay

import android.app.Activity
import android.content.Intent
import androidx.core.app.CoreComponentFactory
import org.yuzu.yuzu_emu.YuzuApplication

/** Redirect before activity constructors, theme/config reads, ViewModels or restored fragments run. */
class NativeSessionComponentFactory : CoreComponentFactory() {
    override fun instantiateActivity(cl: ClassLoader, className: String, intent: Intent?): Activity {
        if (!YuzuApplication.nativeProcess && YuzuApplication.startupDeferred &&
            className != NativeSessionWaitActivity::class.java.name) {
            return NativeSessionWaitActivity().apply { originalActivity = className }
        }
        return super.instantiateActivity(cl, className, intent)
    }
}
