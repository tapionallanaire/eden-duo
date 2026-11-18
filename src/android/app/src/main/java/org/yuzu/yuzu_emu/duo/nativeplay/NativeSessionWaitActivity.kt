// SPDX-License-Identifier: GPL-3.0-or-later
package org.yuzu.yuzu_emu.duo.nativeplay

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import org.yuzu.yuzu_emu.YuzuApplication

/** No AppCompat, core configuration, dynamic theme, language or restored emulator fragments. */
class NativeSessionWaitActivity : Activity() {
    internal var originalActivity = ""
    private lateinit var originalIntent: Intent
    private lateinit var message: TextView
    private lateinit var retry: Button
    private val handler = Handler(Looper.getMainLooper())
    private var checking = false
    private var destroyed = false
    private val poll = Runnable { checkReady() }

    override fun onCreate(savedInstanceState: Bundle?) {
        setTheme(android.R.style.Theme_Material_NoActionBar)
        super.onCreate(null)
        if (originalActivity.isEmpty()) originalActivity = savedInstanceState?.getString("originalActivity")
            ?: "org.yuzu.yuzu_emu.ui.main.MainActivity"
        @Suppress("DEPRECATION")
        val savedIntent = savedInstanceState?.getParcelable<Intent>("originalIntent")
        originalIntent = savedIntent ?: Intent(intent)
        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
            val padding = (24 * resources.displayMetrics.density).toInt()
            setPadding(padding, padding, padding, padding)
        }
        message = TextView(this).apply { gravity = Gravity.CENTER; textSize = 18f }
        retry = Button(this).apply {
            text = "Retry"
            setOnClickListener { checkReady() }
        }
        val back = Button(this).apply {
            text = "Return without stopping the game"
            setOnClickListener { finish() }
        }
        layout.addView(message)
        layout.addView(retry)
        layout.addView(back)
        setContentView(layout)
        val failure = YuzuApplication.startupFailure
        if (failure.isNotEmpty()) message.text = "Saves could not be recovered safely. No game or save operation was started.\n\n$failure"
        else {
            message.text = "A native game still owns the saves. This screen will continue after its process finishes."
            handler.post(poll)
        }
    }

    private fun checkReady() {
        if (checking || destroyed || isFinishing) return
        checking = true
        retry.isEnabled = false
        Thread({
            val error = runCatching { YuzuApplication.application.initializeHost() }.exceptionOrNull()
            handler.post {
                checking = false
                if (destroyed || isFinishing) return@post
                retry.isEnabled = true
                when (error) {
                    null -> {
                        val resume = Intent(originalIntent).setClassName(packageName, originalActivity)
                        // Finish first: a restored singleTop/singleTask component must not reuse this wait instance.
                        finish()
                        startActivity(resume)
                    }
                    is NativeSaveSession.BusyException -> {
                        message.text = "A native game still owns the saves. This screen will continue after its process finishes."
                        handler.postDelayed(poll, 500)
                    }
                    else -> message.text = "Saves could not be recovered safely. No game or save operation was started.\n\n${error.message ?: error.javaClass.simpleName}"
                }
            }
        }, "SaveSessionStartup").start()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putString("originalActivity", originalActivity)
        outState.putParcelable("originalIntent", originalIntent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        originalIntent = Intent(intent)
    }

    override fun onDestroy() {
        destroyed = true
        handler.removeCallbacksAndMessages(null)
        super.onDestroy()
    }
}
