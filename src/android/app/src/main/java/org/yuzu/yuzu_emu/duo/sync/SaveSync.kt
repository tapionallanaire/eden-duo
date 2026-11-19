// SPDX-FileCopyrightText: Copyright 2026 tapionallanaire
// SPDX-License-Identifier: GPL-3.0-or-later
// Save sync through Syncthing. The app runs its own Syncthing (libsyncthing.so, the binary from Syncthing-Fork,
// github.com/researchxxl/syncthing-android) so it can read its own private save folders. Each console's saves are
// copied into one synced folder, "calamansi-saves", laid out <console>/<unit> the same on every device; a hub (the
// user's PC) accepts the device and the folder once. An app that syncs only some consoles (Duet Plus: Switch) ignores
// the other console folders. Before a game starts and after it ends the console's
// saves are reconciled three ways (this device, the synced copy, the hash at the last sync, kept in SQLite).
// Duet's host boundaries also share NativeSaveSession with native play and save maintenance.

package org.yuzu.yuzu_emu.duo.sync

import android.app.Activity
import android.app.job.JobInfo
import android.app.job.JobParameters
import android.app.job.JobScheduler
import android.app.job.JobService
import android.content.ComponentName
import android.content.ContentValues
import android.content.Context
import android.content.Intent
import android.database.sqlite.SQLiteDatabase
import android.database.sqlite.SQLiteOpenHelper
import android.net.Uri
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.PersistableBundle
import android.util.Log
import android.view.View
import android.widget.FrameLayout
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.progressindicator.LinearProgressIndicator
import com.google.android.material.snackbar.Snackbar
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.text.DateFormat
import java.util.Date
import java.util.UUID
import java.util.concurrent.Executors
import org.json.JSONArray
import org.json.JSONObject
import org.yuzu.yuzu_emu.duo.nativeplay.NativeSaveSession

/**
 * One console's saves. In the synced folder they are units [depth] levels below `<id>/`; [units] maps each unit this
 * device can hold (its own, plus the synced ones it has a place for) to its file or folder here.
 * See [savesUnder] for emulators that keep saves in a folder tree.
 */
class SyncConsole(
    val id: String,
    val label: String,
    val depth: Int,
    val units: (synced: List<String>) -> Map<String, File>,
    /** The game's name for a unit, when this app knows it (shown in the conflict dialog). */
    val name: (unit: String) -> String? = { null },
    /** Pins an explicitly approved legacy peer owner before its first connection. */
    val preparePairing: () -> Unit = {},
    /** Null only when the selected profile/root has a real peer mapping. */
    val unpairedReason: () -> String? = { null },
    val scopeDescription: String? = null,
    /** Explicit approval before attaching the selected profile to an existing shared save slot. */
    val ownerConfirmation: () -> SyncOwnerConfirmation? = { null }
)

class SyncOwnerConfirmation(val description: String, val confirm: () -> Unit)

class Conflict internal constructor(
    val console: SyncConsole,
    val unit: String,
    val local: File,
    val remote: File,
    internal val snapshot: SyncConflictSnapshot,
    internal val reservationToken: String?
)

internal class UnpairedSyncException(message: String) : IllegalStateException(message)

internal data class SyncSaveSummary(val modified: Long, val bytes: Long) {
    companion object {
        fun capture(file: File): SyncSaveSummary {
            var modified = 0L
            var bytes = 0L
            for (child in file.walkTopDown().onEnter { it == file || !isSyncthingFile(it.name) }) {
                if (child.isFile && !isSyncthingFile(child.name)) {
                    modified = maxOf(modified, child.lastModified())
                    bytes += child.length()
                }
            }
            return SyncSaveSummary(modified, bytes)
        }
    }
}

/** Captured only under the save lease; a delayed dialog never re-reads a live save. */
internal data class SyncConflictSnapshot(
    val identity: String,
    val localPath: String,
    val remotePath: String,
    val localHash: String?,
    val remoteHash: String?,
    val localSummary: SyncSaveSummary,
    val remoteSummary: SyncSaveSummary
) {
    fun assertCurrent(identity: String, local: File?, remote: File) {
        check(this.identity == identity && local?.canonicalPath == localPath && remote.canonicalPath == remotePath) {
            "The selected profile, save root or save location changed. Compare saves again."
        }
        check(hash(local!!) == localHash && hash(remote) == remoteHash) {
            "The saves changed while the choice was open. Compare saves again."
        }
    }

    companion object {
        fun capture(identity: String, local: File, remote: File) = SyncConflictSnapshot(
            identity, local.canonicalPath, remote.canonicalPath, hash(local), hash(remote),
            SyncSaveSummary.capture(local), SyncSaveSummary.capture(remote)
        )
    }
}

class SaveSync(
    private val context: Context,
    private val appName: String,
    private val guiPort: Int,
    private val listenPort: Int,
    val consoles: List<SyncConsole>,
    /** The console whose game is running now, if any; its saves are never touched mid-game. */
    private val playing: () -> String? = { null },
    /** This app's [SaveSyncJob] subclass (declared in its manifest), which uploads a game's saves when it ends. */
    private val exitJob: Class<out SaveSyncJob>? = null
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
    val hubName: String get() = prefs.getString("hub_name", null)?.takeIf { it.isNotEmpty() } ?: "your PC"
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
        val own = ownAddresses()
        for (id in seen.keys()) {
            val addresses = seen.getJSONObject(id).optJSONArray("addresses")
            val hosts = (0 until (addresses?.length() ?: 0)).map {
                addresses!!.getString(it).substringAfter("://").substringBeforeLast(':')
                    .removePrefix("[").removeSuffix("]").substringBefore('%')
            }
            // The other save-sync app on this same handheld (Calamansi next to Duet Plus) is not a PC.
            if (hosts.any { it in own }) continue
            // The LAN IPv4 address is the one people recognise; link-local IPv6 ones are noise.
            val host = hosts.firstOrNull { it.matches(Regex("""\d+\.\d+\.\d+\.\d+""")) } ?: hosts.firstOrNull().orEmpty()
            found.putIfAbsent(id, "Syncthing at $host")
        }
        found.remove(me)
        return found.toList()
    }

    private fun ownAddresses(): Set<String> = runCatching {
        java.net.NetworkInterface.getNetworkInterfaces().toList()
            .flatMap { it.inetAddresses.toList() }.mapNotNull { it.hostAddress?.substringBefore('%') }.toSet()
    }.getOrDefault(emptySet())

    /** The PC's own device name, once it has connected (Syncthing fills in a blank name from the PC's hello). */
    fun refreshHubName() {
        val id = hub ?: return
        runCatching { JSONObject(api("GET", "/rest/config/devices/$id")).optString("name") }.getOrNull()
            ?.takeIf { it.isNotEmpty() }?.let { prefs.edit().putString("hub_name", it).apply() }
    }

    /** Adds the hub and offers it the saves folder; the user accepts both on the PC. */
    fun connect(id: String, name: String, expectedIdentity: String? = null) =
        NativeSaveSession.acquire(context).use {
            expectedIdentity?.let { NativeSaveSession.assertIdentity(context, it) }
            consoles.forEach { it.preparePairing() }
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
        context.getSystemService(JobScheduler::class.java).cancel(PERIODIC_JOB_ID)
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
        rescan().getOrThrow()
        val begin = System.currentTimeMillis()
        val end = begin + timeoutMs
        var quiet = 0
        while (System.currentTimeMillis() < end && !skip()) {
            val online = hubOnline()
            // An unreachable PC shouldn't hold a game back: Syncthing reconnects within a second or two when it's up.
            if (!online && System.currentTimeMillis() - begin > 3000) return false
            val idle = online && runCatching {
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

    /** Reconciles only while the kernel lease excludes every native/emulated writer. */
    fun reconcile(console: SyncConsole, reservationToken: String? = null): List<Conflict> =
        NativeSaveSession.acquire(context, reservationToken).use {
            check(playing() != console.id) { "Saves are in use by a running game." }
            console.unpairedReason()?.let { throw UnpairedSyncException(it) }
            val identity = NativeSaveSession.identity(context)
            val remoteRoot = File(synced, console.id)
            val units = console.units(listUnits(remoteRoot, console.depth))
            NativeSaveSession.assertIdentity(context, identity)
            val (conflicts, pushed) = reconcileUnits(units, remoteRoot, File(backups, console.id), db.of(console.id)) {
                Log.i(TAG, "${console.id} $it")
                // SaveMerge otherwise logs a failed copy and reports an apparently successful sync.
                check(!it.startsWith("failed ")) { it }
            }
            NativeSaveSession.assertIdentity(context, identity)
            if (pushed) rescan().getOrThrow()
            conflicts.map {
                val local = units.getValue(it)
                val remote = File(remoteRoot, it)
                Conflict(console, it, local, remote, SyncConflictSnapshot.capture(identity, local, remote), reservationToken)
            }
        }

    @Synchronized
    fun resolve(c: Conflict, keepLocal: Boolean) =
        NativeSaveSession.acquire(context, c.reservationToken).use {
            NativeSaveSession.assertIdentity(context, c.snapshot.identity)
            check(playing() != c.console.id) { "Saves are in use by a running game." }
            c.console.unpairedReason()?.let { throw UnpairedSyncException(it) }
            val local = c.console.units(listOf(c.unit))[c.unit]
            val remote = File(File(synced, c.console.id), c.unit)
            c.snapshot.assertCurrent(NativeSaveSession.identity(context), local, remote)
            resolveConflict(c.unit, local!!, remote, File(backups, c.console.id), keepLocal, db.of(c.console.id))
            if (keepLocal) rescan().getOrThrow()
            forget(c)
        }
    /** A newly approved legacy profile must choose conflicts, not inherit another account's hash baseline. */
    internal fun resetSyncHistory(consoleId: String) {
        db.writableDatabase.delete("saves", "console=?", arrayOf(consoleId))
        synchronized(pending) { pending.entries.removeAll { it.value.console.id == consoleId } }
    }


    private fun rescan() = runCatching { api("POST", "/rest/db/scan?folder=$FOLDER") }

    /**
     * Start, wait for the hub, reconcile. Null when the hub couldn't be reached (or [skip] said so). Blocking; one sync
     * at a time, so the exit job never reconciles alongside a launch or background sync.
     */
    @Synchronized
    fun sync(
        consoles: List<SyncConsole>,
        timeoutMs: Long,
        reservationToken: String? = null,
        skip: () -> Boolean = { false }
    ): List<Conflict>? = NativeSaveSession.acquire(context, reservationToken).use {
        consoles.forEach { console -> console.unpairedReason()?.let { throw UnpairedSyncException(it) } }
        if (hub == null || !start() || !waitInSync(timeoutMs, skip) || skip()) return@use null
        consoles.flatMap { reconcile(it, reservationToken) }.also(::remember)
    }


    // ---- UI: Material dialogs only for decisions and waits the user sits through; snackbars for feedback ----

    /** Offline play is allowed, but a busy save lease must never be bypassed. */
    fun beforeLaunch(
        activity: Activity,
        consoleId: String,
        reservationToken: String? = null,
        onFailure: ((String) -> Unit)? = null,
        launch: () -> Unit
    ) {
        val console = consoles.find { it.id == consoleId }
        if (hub == null || console == null || !available) return launch()
        var expectedIdentity: String? = null
        fun refuse(message: String) {
            note(activity, message)
            onFailure?.invoke(message)
        }
        val launchChecked = {
            worker.execute {
                val error = runCatching {
                    NativeSaveSession.acquire(context, reservationToken).use {
                        expectedIdentity?.let { NativeSaveSession.assertIdentity(context, it) }
                    }
                }.exceptionOrNull()
                main.post {
                    if (!activity.isFinishing && !activity.isDestroyed) {
                        if (error == null) launch() else refuse(error.message ?: "Saves are in use.")
                    }
                }
            }
        }
        // "Play now" only stops the wait for the PC; a copy already under way finishes before the game starts.
        val skipped = java.util.concurrent.atomic.AtomicBoolean()
        val finished = java.util.concurrent.atomic.AtomicBoolean()
        val dialog = progress(activity, "Syncing saves", "Checking $hubName for newer ${console.label} saves…")
            .setNegativeButton("Play now") { _, _ -> skipped.set(true) }.create()
        // A quick sync (PC up, nothing new: about 0.8 s on the Thor) launches without a dialog flashing past.
        main.postDelayed({ if (!finished.get() && !activity.isFinishing) dialog.show() }, 1500)
        worker.execute {
            val began = System.currentTimeMillis()
            val result = runCatching {
                NativeSaveSession.acquire(context, reservationToken).use {
                    expectedIdentity = NativeSaveSession.identity(context)
                    sync(listOf(console), 10_000, reservationToken) { skipped.get() }
                }
            }
                .onFailure { Log.w(TAG, "sync before launch", it) }
            val conflicts = result.getOrNull()
            Log.i(
                TAG, "before launch ${console.id}: " + (conflicts?.let { "${it.size} conflicts" } ?: "hub unreachable") +
                    ", ${System.currentTimeMillis() - began} ms${if (skipped.get()) ", skipped" else ""}"
            )
            main.post {
                finished.set(true)
                if (dialog.isShowing) dialog.dismiss()
                if (activity.isFinishing || activity.isDestroyed) return@post
                val failure = result.exceptionOrNull()
                if (failure != null && failure !is UnpairedSyncException) {
                    refuse(failure.message ?: "Save sync could not safely complete. Try again.")
                    return@post
                }
                if (skipped.get()) return@post launchChecked()
                if (conflicts == null) {
                    // ponytail: a toast, not a snackbar: the game's window covers this one at once.
                    Toast.makeText(activity, failure?.message ?: "Couldn't reach $hubName. Playing with this device's saves.", Toast.LENGTH_LONG).show()
                }
                askConflicts(activity, conflicts.orEmpty(), onFailure = onFailure, then = launchChecked)
            }
        }
    }

    /**
     * Uploads the saves a session just wrote, like Steam on exit, through a JobScheduler job: it keeps network access
     * with the app in the background, survives the app's process going away, and retries until the PC has them.
     * Conflicts are asked about when an app screen is next in front.
     */
    fun afterExit(consoleId: String) {
        val index = consoles.indexOfFirst { it.id == consoleId }
        if (index < 0 || hub == null || !available) return
        val job = exitJob ?: return worker.execute { runCatching { upload(consoleId) } }
        val info = JobInfo.Builder(EXIT_JOB_ID + index, ComponentName(context, job))
            .setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY)
            .setBackoffCriteria(30_000, JobInfo.BACKOFF_POLICY_EXPONENTIAL)
            .setExtras(PersistableBundle().apply { putString("console", consoleId) })
            .apply { if (Build.VERSION.SDK_INT >= 31) setExpedited(true) }
            .build()
        context.getSystemService(JobScheduler::class.java).schedule(info)
    }

    /**
     * One upload of [consoleId] ([EVERY] for all consoles): true once the PC holds everything this device has.
     * Blocking.
     */
    @Synchronized
    fun upload(consoleId: String): Boolean = NativeSaveSession.acquire(context).use {
        val which = if (consoleId == EVERY) consoles else consoles.filter { it.id == consoleId }
        if (which.isEmpty() || hub == null || !available) return@use false
        val identity = NativeSaveSession.identity(context)
        // Recovery may have added records while acquiring the lease. Never acknowledge them on an offline/no-op upload.
        val recovered = NativeSaveSession.recoveryUploadsPending(context)
        val conflicts = sync(which, 15_000) ?: return@use false
        if (conflicts.isNotEmpty() || !waitInSync(15_000) || !waitUploaded(120_000)) return@use false
        NativeSaveSession.assertIdentity(context, identity)
        if (recovered && which.size == consoles.size) NativeSaveSession.completeRecoveryUploads(context)
        true
    }

    /**
     * Every console every 15 minutes (Android's shortest period) while there is a network, so saves also follow the
     * user between games, not only at launch and exit. Scheduled once a hub is set; idempotent.
     */
    fun schedulePeriodic() {
        val job = exitJob ?: return
        if (hub == null || !available) return
        val scheduler = context.getSystemService(JobScheduler::class.java)
        if (scheduler.getPendingJob(PERIODIC_JOB_ID) != null) return
        scheduler.schedule(
            JobInfo.Builder(PERIODIC_JOB_ID, ComponentName(context, job))
                .setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY)
                .setPeriodic(15 * 60_000L)
                .setExtras(PersistableBundle().apply { putString("console", EVERY) })
                .build()
        )
    }

    /** Waits until the hub reports it has all of this device's files in the saves folder. */
    private fun waitUploaded(timeoutMs: Long): Boolean {
        val end = System.currentTimeMillis() + timeoutMs
        while (System.currentTimeMillis() < end) {
            val c = runCatching { JSONObject(api("GET", "/rest/db/completion?folder=$FOLDER&device=$hub")) }.getOrNull()
            if (c != null && hubOnline() && c.optString("remoteState") == "valid" &&
                c.optDouble("completion") >= 100.0 && c.optLong("needItems", -1L) == 0L) return true
            Thread.sleep(1000)
        }
        return false
    }

    /**
     * Every console, at most every few minutes (HOME resume). Mismatched saves found by this or an earlier background
     * sync (after a game, say) are put to the user on [activity] straight away, like Steam's cloud conflict prompt.
     */
    fun syncAllInBackground(activity: Activity? = null) {
        if (hub == null || !available) return
        schedulePeriodic()
        val now = System.currentTimeMillis()
        if (now - lastAll >= 5 * 60_000) {
            lastAll = now
            worker.execute {
                runCatching { sync(consoles, 10_000) }
                    .onFailure { lastAll = 0L; Log.w(TAG, "sync all deferred", it) }
            }
        }
        // Queued behind that sync, so it asks about what it found too.
        if (activity != null) worker.execute { main.post { askPending(activity) } }
    }

    private fun askPending(activity: Activity) {
        if (asking || activity.isFinishing || activity.isDestroyed) return
        val conflicts = synchronized(pending) { pending.values.toList() }
        if (conflicts.isEmpty()) return
        asking = true
        askConflicts(activity, conflicts) { asking = false }
    }

    private fun askConflicts(
        activity: Activity,
        conflicts: List<Conflict>,
        onFailure: ((String) -> Unit)? = null,
        then: () -> Unit
    ) {
        val c = conflicts.firstOrNull() ?: return then()
        val next = { askConflicts(activity, conflicts.drop(1), onFailure, then) }
        val fmt = DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT)
        val here = c.snapshot.localSummary.modified
        val there = c.snapshot.remoteSummary.modified
        fun side(name: String, summary: SyncSaveSummary, newer: Boolean): String {
            val bytes = summary.bytes
            val size = if (bytes < 1024 * 1024) "${(bytes + 1023) / 1024} KB" else "%.1f MB".format(bytes / 1048576.0)
            return "$name${if (newer) " (newer)" else ""}\n" +
                "${if (summary.modified > 0) fmt.format(Date(summary.modified)) else "Date unknown"} · $size"
        }
        val game = c.console.name(c.unit) ?: "${c.console.label} ${c.unit}"
        val left = if (conflicts.size > 1) " (${conflicts.size} to choose)" else ""
        MaterialAlertDialogBuilder(activity)
            .setTitle("Which save do you want to keep?$left")
            .setMessage(
                "$game has different saves on this device and on $hubName.\n\n" +
                    side("This device", c.snapshot.localSummary, here > there) + "\n\n" +
                    side("Synced save", c.snapshot.remoteSummary, there > here) + "\n\n" +
                    "The save you replace is kept as a backup."
            )
            .setCancelable(false)
            .setPositiveButton("Use synced save") { _, _ -> resolveThen(activity, c, false, onFailure, next) }
            .setNegativeButton("Use this device's") { _, _ -> resolveThen(activity, c, true, onFailure, next) }
            .setNeutralButton("Decide later") { _, _ ->
                worker.execute {
                    val error = runCatching {
                        NativeSaveSession.acquire(context, c.reservationToken).use {
                            NativeSaveSession.assertIdentity(context, c.snapshot.identity)
                            forget(c)
                        }
                    }.exceptionOrNull()
                    main.post {
                        if (error == null) next()
                        else {
                            val message = error.message ?: "Saves are in use."
                            note(activity, message)
                            asking = false
                            onFailure?.invoke(message)
                        }
                    }
                }
            }
            .show()
    }

    private fun resolveThen(
        activity: Activity,
        c: Conflict,
        keepLocal: Boolean,
        onFailure: ((String) -> Unit)?,
        then: () -> Unit
    ) {
        worker.execute {
            val error = runCatching { resolve(c, keepLocal) }.exceptionOrNull()
            main.post {
                if (error == null) then()
                else {
                    val message = "Couldn't copy the save: ${error.message}"
                    note(activity, message)
                    asking = false
                    onFailure?.invoke(message)
                }
            }
        }
    }

    /** The settings entry: connect to the hub, or show sync state and its one next action. */
    fun showSettings(activity: Activity) {
        if (!available) {
            MaterialAlertDialogBuilder(activity).setTitle("Save sync")
                .setMessage("Save sync isn't included in this build (no Syncthing for this CPU).")
                .setPositiveButton(android.R.string.ok, null).show()
            return
        }
        worker.execute {
            val result = runCatching {
                NativeSaveSession.acquire(context).use {
                    consoles.firstNotNullOfOrNull { it.ownerConfirmation() }
                }
            }
            main.post {
                if (activity.isFinishing || activity.isDestroyed) return@post
                val error = result.exceptionOrNull()
                val confirmation = result.getOrNull()
                when {
                    error != null -> note(activity, "Couldn't open save sync: ${error.message}")
                    confirmation != null -> confirmLegacyOwner(activity, confirmation)
                    hub == null -> askConnect(activity)
                    else -> showStatus(activity)
                }
            }
        }
    }

    private fun confirmLegacyOwner(activity: Activity, confirmation: SyncOwnerConfirmation) {
        MaterialAlertDialogBuilder(activity)
            .setTitle("Use this profile for Save sync?")
            .setMessage(confirmation.description)
            .setPositiveButton("Use this profile") { _, _ ->
                worker.execute {
                    val error = runCatching {
                        NativeSaveSession.acquire(context).use { confirmation.confirm() }
                    }.exceptionOrNull()
                    main.post {
                        if (activity.isFinishing || activity.isDestroyed) return@post
                        if (error != null) note(activity, "Sync profile was not changed: ${error.message}")
                        else {
                            note(activity, "Profile connected. No saves copied yet. Sync now to compare saves.")
                            if (hub == null) askConnect(activity) else showStatus(activity)
                        }
                    }
                }
            }
            .setNegativeButton("Keep saves local") { _, _ ->
                note(activity, "Saves stay local and shared between native and emulated play.")
            }
            .show()
    }

    private fun askConnect(activity: Activity) {
        MaterialAlertDialogBuilder(activity)
            .setTitle("Connect to Syncthing?")
            .setMessage(
                "Your saves sync with Syncthing on your PC over Wi-Fi, before and after every game. " +
                    "$appName finds the PC; you approve this device there once."
            )
            .setPositiveButton("Connect") { _, _ ->
                if (Build.VERSION.SDK_INT >= 37) activity.requestPermissions(arrayOf("android.permission.ACCESS_LOCAL_NETWORK"), 0)
                scan(activity)
            }
            .setNeutralButton("Set up your PC") { _, _ -> openHubGuide(activity) }
            .setNegativeButton("Not now", null)
            .show()
    }

    /** The PC side: a script that installs Syncthing as the hub and starts it at sign-in. */
    private fun openHubGuide(activity: Activity) {
        runCatching { activity.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(HUB_GUIDE))) }
            .onFailure { note(activity, "Open $HUB_GUIDE on your PC to set it up.") }
    }

    private fun scan(activity: Activity) {
        val dialog = progress(activity, "Looking for your PC", "Searching your Wi-Fi network…")
            .setNegativeButton(android.R.string.cancel, null).show()
        worker.execute {
            val wifi = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
            // Syncthing finds the PC from its local-discovery broadcasts, which Android drops without this lock.
            val lock = wifi.createMulticastLock("save-sync").apply { acquire() }
            var me: String? = null
            var found = emptyList<Pair<String, String>>()
            try {
                if (start()) {
                    me = myId()
                    for (second in 0 until 15) {
                        if (!dialog.isShowing) return@execute
                        found = runCatching { nearby() }.getOrDefault(found)
                        // A moment more after the first hit, in case there are two PCs.
                        if (found.isNotEmpty() && second >= 2) break
                        Thread.sleep(1000)
                    }
                }
            } finally {
                lock.release()
            }
            main.post {
                if (!dialog.isShowing) return@post
                dialog.dismiss()
                if (me == null) {
                    MaterialAlertDialogBuilder(activity).setTitle("Save sync couldn't start")
                        .setMessage("Syncthing didn't start on this device. Try again, or restart $appName.")
                        .setPositiveButton("Try again") { _, _ -> scan(activity) }
                        .setNegativeButton(android.R.string.cancel, null).show()
                    return@post
                }
                pick(activity, me!!, found)
            }
        }
    }

    private fun pick(activity: Activity, me: String, found: List<Pair<String, String>>) {
        val identity = runCatching {
            NativeSaveSession.acquire(context).use { NativeSaveSession.identity(context) }
        }.getOrElse { note(activity, it.message ?: "Saves are in use."); return }
        val b = MaterialAlertDialogBuilder(activity)
        when {
            found.isEmpty() -> b.setTitle("No PC found")
                .setMessage(
                    "Check that Syncthing is running on your PC and that both are on the same Wi-Fi. Or add this " +
                        "device on the PC (Add Remote Device) with its ID:\n\n$me"
                )
                .setPositiveButton("Search again") { _, _ -> scan(activity) }
                .setNeutralButton("Set up your PC") { _, _ -> openHubGuide(activity) }
            found.size == 1 -> b.setTitle("Connect to this PC?")
                .setMessage("${found[0].second}\n\nYou'll approve $deviceName on the PC next.")
                .setPositiveButton("Connect") { _, _ -> choose(activity, found[0], identity) }
                .setNeutralButton("Search again") { _, _ -> scan(activity) }
            else -> b.setTitle("Choose your PC")
                .setItems(found.map { it.second }.toTypedArray()) { _, i -> choose(activity, found[i], identity) }
                .setNeutralButton("Search again") { _, _ -> scan(activity) }
        }
        b.setNegativeButton(android.R.string.cancel, null).show()
    }

    private fun choose(activity: Activity, device: Pair<String, String>, identity: String) {
        val (id, label) = device
        worker.execute {
            // A pending device carries the PC's name; otherwise leave it blank for the PC's hello to fill in.
            val error = runCatching {
                connect(id, label.takeUnless { it.startsWith("Syncthing at ") }.orEmpty(), identity)
            }.exceptionOrNull()
            main.post { if (error != null) note(activity, "Couldn't add the PC: ${error.message}") else waitAccept(activity) }
        }
    }

    private fun waitAccept(activity: Activity) {
        val ask = "On your PC, open Syncthing (http://127.0.0.1:8384) and click Add device for \"$deviceName\"."
        val dialog = progress(activity, "Approve on your PC", ask)
            .setNegativeButton("Later", null).show()
        worker.execute {
            repeat(300) {
                if (!dialog.isShowing) return@execute
                val online = hubOnline()
                if (online) refreshHubName()
                if (online && folderAccepted()) {
                    main.post { dialog.setTitle("Connected to $hubName"); dialog.setMessage("Comparing saves…") }
                    val result = runCatching { sync(consoles, 20_000) }
                    val conflicts = result.getOrNull()
                    main.post {
                        dialog.dismiss()
                        note(activity, result.exceptionOrNull()?.message
                            ?: "Connected to $hubName. Saves sync before and after every game.")
                        askConflicts(activity, conflicts.orEmpty()) {}
                    }
                    return@execute
                }
                main.post {
                    dialog.setMessage(
                        if (!online) ask else "Approved. If your PC also asks about the \"Calamansi saves\" folder, click Add."
                    )
                }
                Thread.sleep(2000)
            }
        }
    }

    private fun showStatus(activity: Activity) {
        val dialog = MaterialAlertDialogBuilder(activity).setTitle("Save sync")
            .setMessage("Checking $hubName…")
            .setPositiveButton("Sync now", null)
            .setNeutralButton("Disconnect") { _, _ ->
                MaterialAlertDialogBuilder(activity).setTitle("Stop syncing with $hubName?")
                    .setMessage("Saves stay on this device and on the PC. You can connect again later.")
                    .setPositiveButton("Disconnect") { _, _ ->
                        worker.execute { disconnect(); main.post { note(activity, "Save sync is off.") } }
                    }
                    .setNegativeButton(android.R.string.cancel, null).show()
            }
            .setNegativeButton("Close", null)
            .show()
        val primary = dialog.getButton(AlertDialog.BUTTON_POSITIVE)
        fun refresh(): Unit = worker.execute {
            val online = start() && hubOnline()
            if (online) refreshHubName()
            val waiting = online && !folderAccepted()
            val choices = synchronized(pending) { pending.size }
            val rows = db.perConsole()
            val text = buildString {
                append(
                    when {
                        !online -> "$hubName is offline. Saves sync when it's back."
                        waiting -> "Connected. On $hubName, add the \"Calamansi saves\" folder this device offers."
                        else -> "Connected to $hubName."
                    }
                )
                rows.values.maxOfOrNull { it.last }?.takeIf { it > 0 }?.let {
                    append("\nLast synced ${DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT).format(Date(it))}.")
                }
                append("\n\n")
                if (rows.isEmpty()) append("No saves synced yet.")
                for (c in consoles) {
                    val r = rows[c.id] ?: continue
                    append("${c.label}: ${r.synced} ${if (r.synced == 1) "save" else "saves"}")
                    if (r.conflicts > 0) append(", ${r.conflicts} to choose")
                    append('\n')
                }
                append("\nThis device: $deviceName")
                consoles.mapNotNull { it.scopeDescription }.forEach { append("\n\n$it") }
                runCatching {
                    NativeSaveSession.acquire(context).use { consoles.mapNotNull { it.unpairedReason() } }
                }.getOrElse { listOf(it.message ?: "Saves are in use; sync is deferred.") }
                    .distinct().forEach { append("\n\n$it") }
            }
            main.post {
                if (!dialog.isShowing) return@post
                dialog.setMessage(text.trimEnd())
                primary.text = if (choices > 0) "Choose saves ($choices)" else "Sync now"
            }
        }
        refresh()
        primary.setOnClickListener {
            if (synchronized(pending) { pending.isNotEmpty() }) {
                askConflicts(activity, synchronized(pending) { pending.values.toList() }) { refresh() }
                return@setOnClickListener
            }
            dialog.setMessage("Syncing…")
            worker.execute {
                val result = runCatching { sync(consoles, 15_000) }
                val conflicts = result.getOrNull()
                main.post {
                    if (!dialog.isShowing) return@post
                    if (conflicts == null) note(activity, result.exceptionOrNull()?.message ?: "Couldn't reach $hubName.")
                    else if (conflicts.isEmpty()) note(activity, "Saves are in sync.")
                    askConflicts(activity, conflicts.orEmpty()) { refresh() }
                }
            }
        }
    }

    /** A Material dialog with an indeterminate progress bar under its message, for waits the user sits through. */
    private fun progress(activity: Activity, title: String, message: String): MaterialAlertDialogBuilder {
        val pad = (24 * activity.resources.displayMetrics.density).toInt()
        val bar = LinearProgressIndicator(activity).apply { isIndeterminate = true }
        val box = FrameLayout(activity).apply { setPadding(pad, pad / 3, pad, 0); addView(bar) }
        return MaterialAlertDialogBuilder(activity).setTitle(title).setMessage(message).setView(box).setCancelable(false)
    }

    /** Transient feedback, as a snackbar on the activity's content. */
    private fun note(activity: Activity, text: String) {
        val root = activity.findViewById<View>(android.R.id.content) ?: return
        Snackbar.make(root, text, Snackbar.LENGTH_LONG).show()
    }

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

        class Counts(val synced: Int, val conflicts: Int, val last: Long)

        /** Per console: saves in sync, saves waiting for the user's choice, and the latest sync time. */
        fun perConsole(): Map<String, Counts> = readableDatabase.rawQuery(
            "SELECT console, SUM(state != 'conflict'), SUM(state = 'conflict'), MAX(synced_at) FROM saves" +
                " WHERE state IS NOT NULL GROUP BY console",
            null
        ).use { c ->
            generateSequence { if (c.moveToNext()) c else null }
                .associate { it.getString(0) to Counts(it.getInt(1), it.getInt(2), if (it.isNull(3)) 0 else it.getLong(3)) }
        }
    }

    private var lastAll = 0L

    /** Conflicts a background sync found, asked about as soon as an app screen is in front. */
    private val pending = linkedMapOf<String, Conflict>()
    private var asking = false

    private fun remember(conflicts: List<Conflict>) = synchronized(pending) {
        for (c in conflicts) pending["${c.console.id}/${c.unit}"] = c
    }

    private fun forget(c: Conflict) = synchronized(pending) { pending.remove("${c.console.id}/${c.unit}") }

    companion object {
        private const val TAG = "SaveSync"
        private const val HUB_GUIDE = "https://github.com/tapionallanaire/save-sync-hub"
        const val FOLDER = "calamansi-saves"
        private const val EXIT_JOB_ID = 0x5a5e0000
        private const val PERIODIC_JOB_ID = 0x5a5effff
        /** The "console" extra of the periodic job: every console. */
        const val EVERY = "*"
        private val worker = Executors.newSingleThreadExecutor()
        private val main = Handler(Looper.getMainLooper())
    }
}

/**
 * Uploads one console's saves after a game, retried by JobScheduler until the PC has them, and runs the periodic
 * all-console sync. Each app declares a subclass
 * in its manifest (android:permission="android.permission.BIND_JOB_SERVICE") that returns its [SaveSync].
 */
abstract class SaveSyncJob : JobService() {
    abstract fun saveSync(context: Context): SaveSync

    override fun onStartJob(params: JobParameters): Boolean {
        val console = params.extras.getString("console") ?: return false
        Thread {
            val done = runCatching {
                if (org.yuzu.yuzu_emu.YuzuApplication.startupDeferred) {
                    // No Activity is required to finish recovery after the native process releases its lease.
                    org.yuzu.yuzu_emu.YuzuApplication.application.initializeHost()
                }
                check(!org.yuzu.yuzu_emu.YuzuApplication.startupDeferred) { "Guarded host initialization is still deferred" }
                saveSync(applicationContext).upload(console)
            }.onFailure { Log.w("SaveSync", "exit upload $console", it) }.getOrDefault(false)
            val what = if (console == SaveSync.EVERY) "periodic sync" else "exit upload $console"
            Log.i("SaveSync", "$what: ${if (done) "on the PC" else "not yet, will retry"}")
            jobFinished(params, !done)
        }.start()
        return true
    }

    /** Android stopped the job early (network gone, say): run it again later. */
    override fun onStopJob(params: JobParameters) = true
}
