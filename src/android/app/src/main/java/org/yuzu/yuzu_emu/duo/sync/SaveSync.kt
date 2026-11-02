// SPDX-FileCopyrightText: Copyright 2026 tapionallanaire
// SPDX-License-Identifier: GPL-3.0-or-later
// Save sync through Syncthing. The app runs its own Syncthing (libsyncthing.so, the binary from Syncthing-Fork,
// github.com/researchxxl/syncthing-android) so it can read its own private save folders. Each console's saves are
// copied into one synced folder, "calamansi-saves", laid out <console>/<unit> the same on every device; a hub (the
// user's PC) accepts the device and the folder once. An app that syncs only some consoles (Duet Plus: Switch) ignores
// the other console folders. Before a game starts and after it ends the console's
// saves are reconciled three ways (this device, the synced copy, the hash at the last sync, kept in SQLite).
// The same file lives in Calamansi (org.calamansi.sync, GPL-2.0-or-later there); keep the copies identical apart from
// the licence header and the package line.

package org.yuzu.yuzu_emu.duo.sync

import android.app.Activity
import android.app.AlertDialog
import android.content.ContentValues
import android.content.Context
import android.database.sqlite.SQLiteDatabase
import android.database.sqlite.SQLiteOpenHelper
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.widget.Toast
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.text.DateFormat
import java.util.Date
import java.util.UUID
import java.util.concurrent.Executors
import org.json.JSONArray
import org.json.JSONObject

/**
 * One console's saves. In the synced folder they are units [depth] levels below `<id>/`; [units] maps each unit this
 * device can hold (its own, plus the synced ones it has a place for) to its file or folder here.
 * See [savesUnder] for emulators that keep saves in a folder tree.
 */
class SyncConsole(
    val id: String,
    val label: String,
    val depth: Int,
    val units: (synced: List<String>) -> Map<String, File>
)

class Conflict(val console: SyncConsole, val unit: String, val local: File, val remote: File)

class SaveSync(
    private val context: Context,
    private val appName: String,
    private val guiPort: Int,
    private val listenPort: Int,
    val consoles: List<SyncConsole>,
    /** The console whose game is running now, if any; its saves are never touched mid-game. */
    private val playing: () -> String? = { null }
) {
    private val prefs = context.getSharedPreferences("save_sync", Context.MODE_PRIVATE)
    private val home = File(context.filesDir, "syncthing")
    private val synced = File(context.filesDir, "save-sync")
    private val backups = File(context.filesDir, "save-sync-backups")
    private val binary = File(context.applicationInfo.nativeLibraryDir, "libsyncthing.so")
    private val db = Db(context)
    private val apiKey: String = prefs.getString("api_key", null)
        ?: UUID.randomUUID().toString().also { prefs.edit().putString("api_key", it).apply() }

    val available get() = binary.exists()

    /** Device ID of the hub (the PC), once the user has picked it. */
    val hub: String? get() = prefs.getString("hub", null)
    val hubName: String get() = prefs.getString("hub_name", null) ?: "your PC"
    val deviceName get() = "$appName (${Build.MODEL})"

    // ---- Syncthing process and REST API ----

    private fun api(method: String, path: String, body: Any? = null, timeoutMs: Int = 5000): String {
        val c = URL("http://127.0.0.1:$guiPort$path").openConnection() as HttpURLConnection
        c.requestMethod = method
        c.connectTimeout = timeoutMs
        c.readTimeout = timeoutMs
        c.setRequestProperty("X-API-Key", apiKey)
        if (body != null) {
            c.doOutput = true
            c.setRequestProperty("Content-Type", "application/json")
            c.outputStream.use { it.write(body.toString().toByteArray()) }
        }
        try {
            if (c.responseCode !in 200..299) error("$method $path: HTTP ${c.responseCode}")
            return c.inputStream.bufferedReader().use { it.readText() }
        } finally {
            c.disconnect()
        }
    }

    private fun running() = runCatching { api("GET", "/rest/system/ping", timeoutMs = 1000) }.isSuccess

    /** Starts Syncthing if it isn't running and waits for its API. Blocking; call off the main thread. */
    @Synchronized
    fun start(): Boolean {
        if (running()) return true
        if (!available) return false
        home.mkdirs()
        ProcessBuilder(
            binary.path, "serve", "--no-browser", "--no-restart",
            "--gui-address=127.0.0.1:$guiPort", "--gui-apikey=$apiKey"
        ).apply {
            environment().putAll(
                mapOf(
                    "HOME" to home.path, "STHOMEDIR" to home.path, "STNOUPGRADE" to "1",
                    "SQLITE_TMPDIR" to context.cacheDir.path
                )
            )
            redirectErrorStream(true)
            redirectOutput(File(home, "syncthing.log"))
        }.start()
        repeat(60) {
            if (running()) {
                configureOnce()
                return true
            }
            Thread.sleep(500)
        }
        Log.w(TAG, "Syncthing did not start, see ${home.path}/syncthing.log")
        return false
    }

    /** GET, change, PUT: HttpURLConnection can't send PATCH on every Android version. */
    private fun change(path: String, edit: JSONObject.() -> Unit) =
        api("PUT", path, JSONObject(api("GET", path)).apply(edit))

    private fun configureOnce() {
        if (prefs.getBoolean("configured", false)) return
        change("/rest/config/options") {
            // Own ports, so Calamansi and Duet Plus can both run Syncthing on one device.
            put("listenAddresses", JSONArray(listOf("tcp://:$listenPort", "quic://:$listenPort", "dynamic+https://relays.syncthing.net/endpoint")))
            put("urAccepted", -1)
        }
        runCatching { api("DELETE", "/rest/config/folders/default") }
        change("/rest/config/devices/${myId()}") { put("name", deviceName) }
        prefs.edit().putBoolean("configured", true).apply()
    }

    fun myId(): String = JSONObject(api("GET", "/rest/system/status")).getString("myID")

    /** Devices on the local network, and devices that already asked to connect (the PC adding this one first). */
    fun nearby(): List<Pair<String, String>> {
        val me = myId()
        val found = linkedMapOf<String, String>()
        val pending = JSONObject(api("GET", "/rest/cluster/pending/devices"))
        for (id in pending.keys()) found[id] = pending.getJSONObject(id).optString("name").ifEmpty { id.take(7) }
        val seen = JSONObject(api("GET", "/rest/system/discovery"))
        for (id in seen.keys()) {
            val addresses = seen.getJSONObject(id).optJSONArray("addresses")
            val hosts = (0 until (addresses?.length() ?: 0))
                .map { addresses!!.getString(it).substringAfter("://").substringBeforeLast(':') }
            // The LAN IPv4 address is the one people recognise; link-local IPv6 ones are noise.
            val host = hosts.firstOrNull { it.matches(Regex("""\d+\.\d+\.\d+\.\d+""")) } ?: hosts.firstOrNull().orEmpty()
            found.putIfAbsent(id, "${id.take(7)} at $host")
        }
        found.remove(me)
        return found.toList()
    }

    /** Adds the hub and offers it the saves folder; the user accepts both on the PC. */
    fun connect(id: String, name: String) {
        api(
            "PUT", "/rest/config/devices/$id",
            JSONObject().put("deviceID", id).put("name", name).put("addresses", JSONArray(listOf("dynamic")))
        )
        File(synced, ".stfolder").mkdirs()
        api(
            "PUT", "/rest/config/folders/$FOLDER",
            JSONObject().put("id", FOLDER).put("label", "Calamansi saves").put("path", synced.path)
                .put("type", "sendreceive").put("fsWatcherEnabled", true)
                .put("devices", JSONArray().put(JSONObject().put("deviceID", id)))
        )
        // Only this app's consoles: Duet Plus never downloads (or uploads) another console's saves.
        api(
            "POST", "/rest/db/ignores?folder=$FOLDER",
            JSONObject().put("ignore", JSONArray(consoles.map { "!/${it.id}" } + "*"))
        )
        prefs.edit().putString("hub", id).putString("hub_name", name).apply()
    }

    fun disconnect() {
        val id = hub ?: return
        runCatching {
            api("DELETE", "/rest/config/folders/$FOLDER")
            api("DELETE", "/rest/config/devices/$id")
        }
        prefs.edit().remove("hub").remove("hub_name").apply()
    }

    fun hubOnline(): Boolean = hub?.let { id ->
        runCatching {
            JSONObject(api("GET", "/rest/system/connections")).getJSONObject("connections")
                .optJSONObject(id)?.optBoolean("connected") == true
        }.getOrDefault(false)
    } ?: false

    /** Whether the hub has added the saves folder. */
    fun folderAccepted(): Boolean = runCatching {
        JSONObject(api("GET", "/rest/db/completion?folder=$FOLDER&device=$hub")).optString("remoteState") == "valid"
    }.getOrDefault(false)

    /**
     * Rescans the synced folder and waits until it is connected to the hub and has nothing left to
     * download. False when the hub stays unreachable: the caller then leaves the synced folder alone.
     */
    fun waitInSync(timeoutMs: Long, skip: () -> Boolean = { false }): Boolean {
        rescan()
        val end = System.currentTimeMillis() + timeoutMs
        var quiet = 0
        while (System.currentTimeMillis() < end && !skip()) {
            val idle = hubOnline() && runCatching {
                val s = JSONObject(api("GET", "/rest/db/status?folder=$FOLDER"))
                s.optString("state") == "idle" && s.optLong("needTotalItems") == 0L
            }.getOrDefault(false)
            // ponytail: two quiet polls stand in for "the hub's latest index has arrived"; Syncthing has no
            // direct signal for that. Watch /rest/events RemoteIndexUpdated if this proves too eager.
            quiet = if (idle) quiet + 1 else 0
            if (quiet >= 2) return true
            Thread.sleep(700)
        }
        return false
    }

    // ---- reconciliation ----

    /** Reconciles [console]'s saves. Returns the units both sides changed; those are left for the user. */
    fun reconcile(console: SyncConsole): List<Conflict> {
        if (playing() == console.id) return emptyList()
        val remoteRoot = File(synced, console.id)
        val units = console.units(listUnits(remoteRoot, console.depth))
        val (conflicts, pushed) = reconcileUnits(units, remoteRoot, File(backups, console.id), db.of(console.id))
        if (pushed) rescan()
        return conflicts.map { Conflict(console, it, units.getValue(it), File(remoteRoot, it)) }
    }

    fun resolve(c: Conflict, keepLocal: Boolean) {
        resolveConflict(c.unit, c.local, c.remote, File(backups, c.console.id), keepLocal, db.of(c.console.id))
        if (keepLocal) rescan()
    }

    private fun rescan() = runCatching { api("POST", "/rest/db/scan?folder=$FOLDER") }

    /** Start, wait for the hub, reconcile. Null when the hub couldn't be reached (or [skip] said so). Blocking. */
    fun sync(consoles: List<SyncConsole>, timeoutMs: Long, skip: () -> Boolean = { false }): List<Conflict>? {
        if (hub == null || !start() || !waitInSync(timeoutMs, skip) || skip()) return null
        return consoles.flatMap { reconcile(it) }
    }

    fun summary(): List<String> = db.rows()

    // ---- UI ----

    /** Syncs [consoleId]'s saves, asks about conflicts, then runs [launch]. Always launches, like Steam. */
    fun beforeLaunch(activity: Activity, consoleId: String, launch: () -> Unit) {
        val console = consoles.find { it.id == consoleId }
        if (hub == null || console == null || !available) return launch()
        // "Play now" only stops the wait for the PC; a copy already under way finishes before the game starts.
        val skipped = java.util.concurrent.atomic.AtomicBoolean()
        val dialog = AlertDialog.Builder(activity)
            .setTitle("Syncing saves")
            .setMessage("Checking $hubName for newer ${console.label} saves…")
            .setCancelable(false)
            .setNegativeButton("Play now") { _, _ -> skipped.set(true) }
            .show()
        worker.execute {
            val conflicts = runCatching { sync(listOf(console), 10_000) { skipped.get() } }
                .onFailure { Log.w(TAG, "sync before launch", it) }.getOrNull()
            main.post {
                if (dialog.isShowing) dialog.dismiss()
                if (activity.isFinishing || activity.isDestroyed) return@post
                if (skipped.get()) return@post launch()
                if (conflicts == null) {
                    Toast.makeText(activity, "Couldn't reach $hubName: playing with this device's saves", Toast.LENGTH_LONG).show()
                }
                askConflicts(activity, conflicts.orEmpty(), launch)
            }
        }
    }

    /** Pushes the saves a session just wrote. Conflicts wait for the next launch or the settings screen. */
    fun afterExit(consoleId: String) {
        val console = consoles.find { it.id == consoleId } ?: return
        if (hub == null || !available) return
        worker.execute { runCatching { sync(listOf(console), 15_000) }.onFailure { Log.w(TAG, "sync after exit", it) } }
    }

    /** Every console, at most every few minutes (HOME resume). */
    fun syncAllInBackground() {
        if (hub == null || !available) return
        val now = System.currentTimeMillis()
        if (now - lastAll < 5 * 60_000) return
        lastAll = now
        worker.execute { runCatching { sync(consoles, 10_000) }.onFailure { Log.w(TAG, "sync all", it) } }
    }

    private fun askConflicts(activity: Activity, conflicts: List<Conflict>, then: () -> Unit) {
        val c = conflicts.firstOrNull() ?: return then()
        val fmt = DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT)
        fun modified(f: File) = f.walkTopDown().filter { it.isFile }.maxOfOrNull { it.lastModified() }
            ?.let { fmt.format(Date(it)) } ?: "unknown"
        AlertDialog.Builder(activity)
            .setTitle("Save conflict")
            .setMessage(
                "${c.console.label} ${c.unit} changed here and on another device since the last sync.\n\n" +
                    "This device: ${modified(c.local)}\nSynced copy: ${modified(c.remote)}\n\n" +
                    "The save you replace is kept as a backup."
            )
            .setCancelable(false)
            .setPositiveButton("Use synced copy") { _, _ -> resolveThen(activity, c, false) { askConflicts(activity, conflicts.drop(1), then) } }
            .setNegativeButton("Keep this device's") { _, _ -> resolveThen(activity, c, true) { askConflicts(activity, conflicts.drop(1), then) } }
            .setNeutralButton("Decide later") { _, _ -> askConflicts(activity, conflicts.drop(1), then) }
            .show()
    }

    private fun resolveThen(activity: Activity, c: Conflict, keepLocal: Boolean, then: () -> Unit) {
        worker.execute {
            val error = runCatching { resolve(c, keepLocal) }.exceptionOrNull()
            main.post {
                if (error != null) Toast.makeText(activity, "Couldn't copy the save: ${error.message}", Toast.LENGTH_LONG).show()
                then()
            }
        }
    }

    /** The settings entry: connect to the hub, or show sync state, sync now, resolve conflicts, disconnect. */
    fun showSettings(activity: Activity) {
        if (!available) {
            AlertDialog.Builder(activity).setTitle("Save sync")
                .setMessage("Save sync isn't included in this build (no Syncthing for this CPU).")
                .setPositiveButton(android.R.string.ok, null).show()
            return
        }
        if (hub == null) askConnect(activity) else showStatus(activity)
    }

    private fun askConnect(activity: Activity) {
        AlertDialog.Builder(activity)
            .setTitle("Connect to Syncthing")
            .setMessage(
                "Sync your saves with Syncthing on your PC over the network. $appName runs its own Syncthing; " +
                    "you accept this device on the PC once, then saves sync before and after every game."
            )
            .setPositiveButton("Connect") { _, _ ->
                if (Build.VERSION.SDK_INT >= 37) activity.requestPermissions(arrayOf("android.permission.ACCESS_LOCAL_NETWORK"), 0)
                scan(activity)
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun scan(activity: Activity) {
        val dialog = AlertDialog.Builder(activity).setTitle("Looking for your PC")
            .setMessage("Starting Syncthing and searching the network…").setCancelable(false).show()
        worker.execute {
            val wifi = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
            // Syncthing finds the PC from its local-discovery broadcasts, which Android drops without this lock.
            val lock = wifi.createMulticastLock("save-sync").apply { acquire() }
            var me: String? = null
            var found = emptyList<Pair<String, String>>()
            try {
                if (start()) {
                    me = myId()
                    repeat(15) {
                        found = runCatching { nearby() }.getOrDefault(found)
                        if (found.isNotEmpty() && it >= 4) return@repeat
                        Thread.sleep(1000)
                    }
                }
            } finally {
                lock.release()
            }
            main.post {
                dialog.dismiss()
                if (me == null) {
                    AlertDialog.Builder(activity).setTitle("Save sync")
                        .setMessage("Syncthing didn't start on this device.").setPositiveButton(android.R.string.ok, null).show()
                    return@post
                }
                pick(activity, me!!, found)
            }
        }
    }

    private fun pick(activity: Activity, me: String, found: List<Pair<String, String>>) {
        val b = AlertDialog.Builder(activity).setTitle(if (found.isEmpty()) "No PC found" else "Pick your PC")
        if (found.isEmpty()) {
            b.setMessage(
                "Make sure Syncthing is running on your PC and both are on the same network. You can also add " +
                    "this device on the PC (Add Remote Device) with its ID:\n\n$me"
            )
        } else {
            b.setItems(found.map { it.second }.toTypedArray()) { _, i ->
                val (id, name) = found[i]
                worker.execute {
                    val error = runCatching { connect(id, name.takeUnless { it.startsWith(id.take(7)) } ?: "PC") }.exceptionOrNull()
                    main.post { if (error != null) toast(activity, "Couldn't add the PC: ${error.message}") else waitAccept(activity) }
                }
            }
        }
        b.setPositiveButton("Scan again") { _, _ -> scan(activity) }.setNegativeButton(android.R.string.cancel, null).show()
    }

    private fun waitAccept(activity: Activity) {
        val dialog = AlertDialog.Builder(activity).setTitle("Accept on your PC")
            .setMessage(
                "Open Syncthing on $hubName (http://127.0.0.1:8384). Accept the device \"$deviceName\", then add " +
                    "the \"Calamansi saves\" folder it offers (keep the folder ID; any path works)."
            )
            .setCancelable(false).setNegativeButton("Later", null).show()
        worker.execute {
            repeat(300) {
                if (!dialog.isShowing) return@execute
                val online = hubOnline()
                if (online && folderAccepted()) {
                    main.post { dialog.dismiss(); toast(activity, "Connected to $hubName. Saves will sync before and after every game.") }
                    syncAllInBackground()
                    return@execute
                }
                main.post {
                    dialog.setMessage(
                        if (!online) "Open Syncthing on $hubName and accept the device \"$deviceName\"."
                        else "Device accepted. Now add the \"Calamansi saves\" folder it offers on the PC."
                    )
                }
                Thread.sleep(2000)
            }
        }
    }

    private fun showStatus(activity: Activity) {
        val dialog = AlertDialog.Builder(activity).setTitle("Save sync")
            .setMessage("Checking…")
            .setPositiveButton("Sync now", null)
            .setNeutralButton("Disconnect") { _, _ ->
                AlertDialog.Builder(activity).setTitle("Disconnect from $hubName?")
                    .setMessage("Saves stay on this device and on the PC; they just stop syncing.")
                    .setPositiveButton("Disconnect") { _, _ -> worker.execute { disconnect() } }
                    .setNegativeButton(android.R.string.cancel, null).show()
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
        fun refresh() = worker.execute {
            val online = start() && hubOnline()
            val waiting = online && !folderAccepted()
            val text = buildString {
                append(if (online) "Connected to $hubName." else "$hubName is offline; saves sync when it's back.")
                if (waiting) append("\nOn the PC, add the \"Calamansi saves\" folder this device offers.")
                append("\n\nThis device: $deviceName\n${runCatching { myId() }.getOrDefault("")}\n\n")
                val rows = summary()
                append(if (rows.isEmpty()) "No saves synced yet." else rows.joinToString("\n"))
            }
            main.post { if (dialog.isShowing) dialog.setMessage(text) }
        }
        refresh()
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
            dialog.setMessage("Syncing…")
            worker.execute {
                val conflicts = runCatching { sync(consoles, 15_000) }.getOrNull()
                main.post {
                    if (!dialog.isShowing) return@post
                    if (conflicts == null) toast(activity, "Couldn't reach $hubName.")
                    askConflicts(activity, conflicts.orEmpty()) { refresh() }
                }
            }
        }
    }

    private fun toast(activity: Activity, text: String) = Toast.makeText(activity, text, Toast.LENGTH_LONG).show()

    // ---- SQLite: one row per save, the hash both sides agreed on at the last sync plus cached hashes ----

    private class Db(context: Context) : SQLiteOpenHelper(context, "save_sync.db", null, 1) {
        override fun onCreate(db: SQLiteDatabase) = db.execSQL(
            "CREATE TABLE saves(console TEXT NOT NULL, unit TEXT NOT NULL, base TEXT, state TEXT, synced_at INTEGER," +
                " local_stamp TEXT, local_hash TEXT, remote_stamp TEXT, remote_hash TEXT, PRIMARY KEY(console, unit))"
        )

        override fun onUpgrade(db: SQLiteDatabase, old: Int, new: Int) = Unit

        private fun ensure(console: String, unit: String) = writableDatabase.insertWithOnConflict(
            "saves", null, ContentValues().apply { put("console", console); put("unit", unit) },
            SQLiteDatabase.CONFLICT_IGNORE
        )

        private fun update(console: String, unit: String, values: ContentValues) {
            ensure(console, unit)
            writableDatabase.update("saves", values, "console=? AND unit=?", arrayOf(console, unit))
        }

        private fun get(console: String, unit: String, column: String): String? =
            readableDatabase.rawQuery("SELECT $column FROM saves WHERE console=? AND unit=?", arrayOf(console, unit))
                .use { if (it.moveToFirst()) it.getString(0) else null }

        fun base(console: String, unit: String) = get(console, unit, "base")

        /** Hash of a save, recomputed only when its file count, size or newest mtime changed. */
        fun hash(console: String, unit: String, side: String, file: File): String? {
            val s = stamp(file) ?: return null
            if (get(console, unit, "${side}_stamp") == s) return get(console, unit, "${side}_hash")
            val h = hash(file)
            update(console, unit, ContentValues().apply { put("${side}_stamp", s); put("${side}_hash", h) })
            return h
        }

        fun synced(console: String, unit: String, hash: String, state: String) = update(
            console, unit,
            ContentValues().apply { put("base", hash); put("state", state); put("synced_at", System.currentTimeMillis()) }
        )

        fun state(console: String, unit: String, state: String) =
            update(console, unit, ContentValues().apply { put("state", state) })

        fun of(console: String) = object : SyncState {
            override fun hash(unit: String, side: String, file: File) = hash(console, unit, side, file)
            override fun base(unit: String) = base(console, unit)
            override fun synced(unit: String, hash: String, state: String) = synced(console, unit, hash, state)
            override fun conflict(unit: String) = state(console, unit, "conflict")
        }

        fun rows(): List<String> {
            val fmt = DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT)
            return readableDatabase.rawQuery(
                "SELECT console, unit, state, synced_at FROM saves WHERE state IS NOT NULL ORDER BY synced_at DESC LIMIT 30",
                null
            ).use { c ->
                generateSequence { if (c.moveToNext()) c else null }.map {
                    val at = if (it.isNull(3)) "" else fmt.format(Date(it.getLong(3)))
                    "${it.getString(0)} ${it.getString(1)}: ${it.getString(2)} $at"
                }.toList()
            }
        }
    }

    private var lastAll = 0L

    companion object {
        private const val TAG = "SaveSync"
        const val FOLDER = "calamansi-saves"
        private val worker = Executors.newSingleThreadExecutor()
        private val main = Handler(Looper.getMainLooper())
    }
}
