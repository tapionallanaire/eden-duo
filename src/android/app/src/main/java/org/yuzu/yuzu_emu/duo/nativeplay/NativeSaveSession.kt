// SPDX-License-Identifier: GPL-3.0-or-later
package org.yuzu.yuzu_emu.duo.nativeplay

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.os.ResultReceiver
import android.system.OsConstants
import android.util.AtomicFile
import androidx.annotation.Keep
import java.io.Closeable
import java.io.File
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.CompletableFuture
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.ExecutionException
import java.util.concurrent.locks.ReentrantReadWriteLock
import org.json.JSONObject
import org.yuzu.yuzu_emu.NativeLibrary
import org.yuzu.yuzu_emu.utils.PathUtil
import org.yuzu.yuzu_emu.utils.GameHelper
import org.yuzu.yuzu_emu.getPublicFilesDir
import org.yuzu.yuzu_emu.utils.NativeConfig

/** One stable kernel lease outside all replaceable save trees, shared by every save accessor. */
@Keep
object NativeSaveSession {
    class BusyException : IllegalStateException("Saves are in use by a game or another save operation. Try again after it finishes.")

    private data class Reservation(val lease: ParcelFileDescriptor, val identity: String,
        val record: JSONObject?, @Volatile var launched: Boolean = false, var borrowers: Int = 0)
    private data class Access(val close: Closeable, val reservation: Reservation? = null, var depth: Int = 1)
    private val reservations = ConcurrentHashMap<String, Reservation>()
    private val pendingReservations = ConcurrentHashMap<String, CompletableFuture<String>>()
    private val access = ThreadLocal<Access>()
    private val childLeases = mutableMapOf<String, ParcelFileDescriptor>()
    private val main = Handler(Looper.getMainLooper())
    private val tokenPattern = Regex("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")
    private val sourceRootsMutex = Any()
    private val sourceReads = ReentrantReadWriteLock(true)
    private val knownPhysicalRoots = mutableSetOf<String>()
    private val sourceTreeHistory = linkedSetOf<String>()
    private var sourceBoundariesInvalidated = false
    @Volatile private var sourceReadRoots: List<String>? = null

    private fun directory(context: Context) = File(context.noBackupFilesDir, "native-save-sessions").apply {
        check(isDirectory || mkdirs()) { "Cannot open save session directory" }
    }
    private fun recordFile(context: Context, token: String): AtomicFile {
        require(tokenPattern.matches(token)) { "Invalid save session token" }
        return AtomicFile(File(directory(context), "$token.json"))
    }
    private fun writeRecord(context: Context, token: String, record: JSONObject) {
        val file = recordFile(context, token)
        val out = file.startWrite()
        try { out.write(record.toString().toByteArray(Charsets.UTF_8)); file.finishWrite(out) }
        catch (e: Throwable) { file.failWrite(out); throw e }
    }
    private fun readRecord(context: Context, token: String): JSONObject =
        JSONObject(recordFile(context, token).openRead().bufferedReader().use { it.readText() })

    private fun recordTokens(context: Context): List<String> =
        directory(context).listFiles().orEmpty().mapNotNull { file ->
            // AtomicFile may leave only its legacy backup if the process dies during replacement.
            file.name.removeSuffix(".bak").takeIf { it.endsWith(".json") }?.removeSuffix(".json")
                ?.takeIf(tokenPattern::matches)
        }.distinct()

    private fun open(context: Context): ParcelFileDescriptor {
        val fd = NativeSessionLock.openLease(File(directory(context), "session.lock").path)
        if (fd == -OsConstants.EAGAIN) throw BusyException()
        check(fd >= 0) { "Cannot acquire save session lease (errno ${-fd})" }
        return ParcelFileDescriptor.adoptFd(fd)
    }

    @JvmStatic fun identity(context: Context): String {
        val root = File(PathUtil.saveDir()).canonicalPath
        rememberSaveRoot(root)
        return root + "\n" + NativeLibrary.getCurrentUser().orEmpty()
    }

    /** Lock-free: own SAF source callbacks may run on a Binder thread while admission owns our monitor. */
    @JvmStatic fun sourceReadProtectedRoots(): List<String>? = sourceReadRoots

    @JvmStatic fun lockSourceReads() { if (!sourceReads.readLock().tryLock()) throw BusyException() }
    @JvmStatic fun unlockSourceReads() { sourceReads.readLock().unlock() }
    @JvmStatic fun lockSourceMutations() { if (!sourceReads.writeLock().tryLock()) throw BusyException() }
    @JvmStatic fun unlockSourceMutations() { sourceReads.writeLock().unlock() }

    @JvmStatic fun invalidateSourceReadBoundaries() {
        lockSourceMutations()
        try {
            synchronized(sourceRootsMutex) {
                sourceBoundariesInvalidated = true
                sourceReadRoots = null
            }
        } finally { sourceReads.writeLock().unlock() }
    }

    private fun rememberSaveRoot(root: String) = synchronized(sourceRootsMutex) {
        if (knownPhysicalRoots.add(root)) {
            sourceTreeHistory.add(File(root, "user/save").canonicalPath)
            sourceTreeHistory.add(File(root, "system/save").canonicalPath)
            if (!sourceBoundariesInvalidated) sourceReadRoots = sourceTreeHistory.toList()
        }
    }

    /** Refresh only after config initialization or mutation, while holding ordinary save access. */
    @JvmStatic fun refreshSourceReadBoundaries(context: Context) {
        check(access.get() != null) { "Refreshing source boundaries requires the save lease" }
        rememberSaveRoot(File(PathUtil.saveDir()).canonicalPath)
        val nand = NativeConfig.getNandDir()
        if (File(nand).isAbsolute) rememberSaveRoot(File(nand).canonicalPath)
        rememberSaveRoot(File(context.getPublicFilesDir(), "nand").canonicalPath)
        recordTokens(context).forEach { token ->
            rememberSaveRoot(readRecord(context, token).getJSONObject("binding").getString("save_root"))
        }
        synchronized(sourceRootsMutex) {
            sourceBoundariesInvalidated = false
            sourceReadRoots = sourceTreeHistory.toList()
        }
    }

    @JvmStatic fun assertIdentity(context: Context, captured: String) {
        NativeSaveBindingRules.requireIdentity(captured, identity(context))
    }

    /** Reentrant only on the acquiring thread. A token authorizes prelaunch sync, never an active writer. */
    @JvmStatic @JvmOverloads fun acquire(context: Context, reservationToken: String? = null): Closeable {
        val current = access.get()
        if (current != null) {
            current.depth++
            return Closeable { releaseAccess() }
        }
        val reserved = reservationToken?.let { reservations[it] ?: error("Save reservation expired") }
        val owned = if (reserved != null) synchronized(reservations) {
            if (reservations[checkNotNull(reservationToken)] !== reserved || reserved.launched) throw BusyException()
            ParcelFileDescriptor.dup(reserved.lease.fileDescriptor).also { reserved.borrowers++ }
        } else open(context)
        try {
            if (reserved != null) assertIdentity(context, reserved.identity)
            recoverLocked(context)
            access.set(Access(owned, reserved))
            return Closeable { releaseAccess() }
        } catch (e: Throwable) { closeAccess(owned, reserved); throw e }
    }

    /** Detached descriptor lifetime for SAF clients; no thread-local authorization and no nested access. */
    @JvmStatic fun acquireDetached(context: Context): Closeable {
        val lease = open(context)
        try { recoverLocked(context); return lease }
        catch (e: Throwable) { lease.close(); throw e }
    }

    private fun releaseAccess() {
        val current = checkNotNull(access.get()) { "Save lease closed on another thread" }
        if (--current.depth == 0) {
            access.remove()
            closeAccess(current.close, current.reservation)
        }
    }

    private fun closeAccess(lease: Closeable, reservation: Reservation?) {
        try { lease.close() }
        finally { if (reservation != null) synchronized(reservations) { reservation.borrowers-- } }
    }

    /** Held from PLAY, through sync/dialogs and surface setup, until the actual writer stops. */
    private fun reserve(context: Context, token: String, expectedIdentity: String,
        createRecord: () -> JSONObject?): String {
        require(tokenPattern.matches(token))
        if (reservations.containsKey(token)) return token
        val future = CompletableFuture<String>()
        val previous = pendingReservations.putIfAbsent(token, future)
        if (previous != null) {
            try { previous.get() } catch (e: ExecutionException) { throw e.cause ?: e }
            return token
        }
        var lease: ParcelFileDescriptor? = null
        try {
            if (reservations.containsKey(token)) return token.also { future.complete(it) }
            lease = open(context)
            assertIdentity(context, expectedIdentity)
            recoverLocked(context)
            val record = createRecord()
            assertIdentity(context, expectedIdentity)
            if (record != null) writeRecord(context, token, record)
            synchronized(reservations) {
                check(!future.isCancelled) { "Save reservation was cancelled" }
                reservations[token] = Reservation(checkNotNull(lease), expectedIdentity, record)
                lease = null
                future.complete(token)
            }
            return token
        } catch (e: Throwable) {
            future.completeExceptionally(e)
            throw e
        } finally {
            lease?.close()
            pendingReservations.remove(token, future)
        }
    }

    internal fun reserveEmulated(context: Context, expectedIdentity: String, token: String): String {
        reserve(context, token, expectedIdentity) { null }
        val reservation = checkNotNull(reservations[token])
        check(reservation.identity == expectedIdentity && reservation.record == null && !reservation.launched)
        return token
    }

    internal fun reserveNative(context: Context, source: String, contentKey: String,
        engine: NativeEngine, programIndex: Int, programId: String, expectedIdentity: String, token: String): String {
        reserve(context, token, expectedIdentity) {
            val binding = GameHelper.withPrepareContent(source, { programId.toULong(16).toString() }) {
                val observed = NativeGameSupport.findBuildCandidate(context, source, programIndex)
                check(observed != null && observed.contentKey == contentKey && observed.profile.programId == programId) {
                    "The original effective retail content changed before save admission"
                }
                JSONObject(NativeLibrary.getNativeSaveBinding(source, programIndex))
            }
            validateBinding(binding)
            require(binding.getString("program_id") == programId) { "Canonical save program changed" }
            require(binding.getString("save_root") == expectedIdentity.substringBefore('\n')) { "Canonical save root changed" }
            PreparedNativeGame.recoverSharedSaves(engine, binding.optionalPath("account_path"), binding.optionalPath("device_path"))
            JSONObject().put("token", token).put("identity", expectedIdentity)
                .put("source", source).put("content_key", contentKey).put("engine", engine.name)
                .put("binding", binding).put("recovered", false).put("account_uploaded", false)
                .put("device_upload_state", if (binding.isNull("device_path")) "not_declared" else "unpaired")
        }
        val reservation = checkNotNull(reservations[token])
        val record = checkNotNull(reservation.record)
        check(reservation.identity == expectedIdentity && !reservation.launched &&
            record.getString("source") == source && record.getString("content_key") == contentKey &&
            record.getJSONObject("binding").getString("program_id") == programId)
        return token
    }

    internal fun hasReservation(token: String): Boolean = reservations.containsKey(token)
    internal fun isEmulating(token: String): Boolean =
        reservations[token]?.let { it.record == null && it.launched } == true

    internal fun occupied(context: Context, ownToken: String): Boolean {
        if (reservations.containsKey(ownToken) || pendingReservations.containsKey(ownToken)) return false
        return try { open(context).use { }; false }
        catch (_: BusyException) { true }
    }

    internal fun cancel(token: String) = synchronized(reservations) {
        pendingReservations.remove(token)?.cancel(false)
        val reservation = reservations[token]
        if (reservation != null && !(reservation.record == null && reservation.launched)) {
            reservations.remove(token)
            reservation.lease.close()
        }
        // Retain native records: even a failed start is recovered under the next lease.
    }

    internal fun attachIntent(context: Context, token: String, intent: Intent) = synchronized(reservations) {
        val reservation = checkNotNull(reservations[token]) { "Save reservation expired" }
        check(reservation.record != null)
        if (reservation.launched || reservation.borrowers != 0) throw BusyException()
        assertIdentity(context, reservation.identity)
        val component = checkNotNull(intent.component) { "Native launch must be explicit" }
        val info = context.packageManager.getActivityInfo(component, 0)
        check(info.packageName == context.packageName && !info.exported) { "Native launch must be private" }
        val record = reservation.record
        check(intent.getStringExtra("duet_game_path") == record.getString("source") &&
            intent.getStringExtra("content_key") == record.getString("content_key") &&
            intent.getStringExtra("title_id") == record.getJSONObject("binding").getString("program_id")) {
            "Prepared launch no longer matches its trusted source/content/program"
        }
        reservation.launched = true
        intent.putExtra("duet_launch_token", token)
        intent.putExtra("duet_save_receiver", object : ResultReceiver(main) {
            override fun onReceiveResult(code: Int, data: Bundle?) {
                // Binder result bundles default to the boot loader; the reply receiver belongs to this app.
                data?.classLoader = NativeSaveSession::class.java.classLoader
                synchronized(reservations) {
                    val active = reservations[token] ?: return
                    if (data?.getString("token") != token) return
                    if (code == 2) { cancel(token); return }
                    if (code != 1 || data.getString("program_id") != record.getJSONObject("binding").getString("program_id") ||
                        data.getString("content_key") != record.getString("content_key")) return
                    @Suppress("DEPRECATION")
                    val reply = data.getParcelable<ResultReceiver>("reply") ?: return
                    ParcelFileDescriptor.dup(active.lease.fileDescriptor).use { duplicate ->
                        reply.send(1, Bundle().apply {
                            putString("token", token)
                            putString("binding", record.getJSONObject("binding").toString())
                            putParcelable("lease", duplicate)
                        })
                    }
                }
            }
        })
        // If handover/ACK is lost, drop only our description. A live child's duplicate still owns the lock.
        main.postDelayed({ cancel(token) }, 30_000)
    }

    /** Native child reflection entry. Never references NativeLibrary or loads the emulator core. */
    @JvmStatic fun attachNative(context: Context, intent: Intent, programId: String, contentKey: String): String {
        val token = intent.getStringExtra("duet_launch_token") ?: error("Missing trusted save session")
        val record = readRecord(context, token)
        val binding = record.getJSONObject("binding")
        check(record.getString("token") == token && record.getString("content_key") == contentKey &&
            intent.getStringExtra("duet_game_path") == record.getString("source") &&
            intent.getStringExtra("content_key") == contentKey &&
            intent.getStringExtra("title_id").equals(programId, true) &&
            binding.getString("program_id").equals(programId, true)) { "Trusted save launch identity changed" }
        validateBinding(binding)
        synchronized(childLeases) {
            if (childLeases.containsKey(token)) return binding.toString()
        }
        check(Looper.myLooper() != Looper.getMainLooper()) { "Save attachment must run on a worker thread" }
        @Suppress("DEPRECATION")
        val receiver = intent.getParcelableExtra<ResultReceiver>("duet_save_receiver")
            ?: error("Missing save lease capability")
        val done = CountDownLatch(1)
        var received: ParcelFileDescriptor? = null
        var response: String? = null
        var accepting = true
        val reply = object : ResultReceiver(Handler(Looper.getMainLooper())) {
            override fun onReceiveResult(code: Int, data: Bundle?) {
                @Suppress("DEPRECATION")
                val lease = data?.getParcelable<ParcelFileDescriptor>("lease")
                synchronized(done) {
                    if (accepting && code == 1 && data?.getString("token") == token &&
                        data.getString("binding") == binding.toString() && lease != null) {
                        received = lease; response = data.getString("binding")
                    } else lease?.close()
                    done.countDown()
                }
            }
        }
        receiver.send(1, Bundle().apply {
            putString("token", token); putString("program_id", binding.getString("program_id"))
            putString("content_key", contentKey); putParcelable("reply", reply)
        })
        val completed = done.await(15, TimeUnit.SECONDS)
        synchronized(done) {
            accepting = false
            if (!completed || received == null) { received?.close(); error("Save lease handover timed out; no saves were opened") }
            synchronized(childLeases) { childLeases[token] = received!! }
        }
        receiver.send(2, Bundle().apply { putString("token", token) })
        return checkNotNull(response)
        // No Activity/onDestroy release: only process death proves that all native writer threads are gone.
    }

    internal fun <T> runEmulated(context: Context, token: String, block: () -> T): T {
        val reservation = synchronized(reservations) {
            checkNotNull(reservations[token]) { "Emulation save reservation expired" }.also {
                check(it.record == null)
                if (it.launched || it.borrowers != 0) throw BusyException()
                assertIdentity(context, it.identity)
                it.launched = true
            }
        }
        check(access.get() == null)
        access.set(Access(Closeable { }))
        try { return block() }
        finally {
            access.remove()
            synchronized(reservations) { reservations.remove(token); reservation.lease.close() }
        }
    }

    /** False until *all* Binder descriptions are closed, regardless of Activity stopped markers. */
    internal fun recoverAfterExit(context: Context): Boolean = try { acquire(context).use { }; true }
        catch (_: BusyException) { false }

    private fun recoverLocked(context: Context) {
        val active = reservations.values.mapNotNull { it.record?.getString("token") }.toSet()
        recordTokens(context).filter { it !in active }.forEach { token ->
                val record = readRecord(context, token)
                if (!record.getBoolean("recovered")) {
                    val binding = record.getJSONObject("binding")
                    validateBinding(binding)
                    PreparedNativeGame.recoverSharedSaves(NativeEngine.valueOf(record.getString("engine")),
                        binding.optionalPath("account_path"), binding.optionalPath("device_path"))
                    record.put("recovered", true)
                    writeRecord(context, token, record)
                }
            }
    }

    @JvmStatic fun recoveryUploadsPending(context: Context): Boolean =
        recordTokens(context).any { token ->
            val record = readRecord(context, token)
            !record.getBoolean("recovered") ||
                (!record.getJSONObject("binding").isNull("account_path") && !record.optBoolean("account_uploaded"))
        }

    /** Called under the save lease, only after a real successful upload for this account/root. */
    @JvmStatic fun completeRecoveryUploads(context: Context) {
        check(access.get() != null) { "Upload completion requires the save lease" }
        val current = identity(context)
        recordTokens(context).forEach { token ->
            val record = readRecord(context, token)
            if (record.getBoolean("recovered") && record.getString("identity") == current) {
                if (record.getJSONObject("binding").isNull("device_path"))
                    recordFile(context, token).delete()
                else {
                    record.put("account_uploaded", true)
                    writeRecord(context, token, record)
                }
            }
        }
    }

    private fun JSONObject.optionalPath(key: String): String? = if (isNull(key)) null else getString(key)

    internal fun validateBinding(binding: JSONObject) {
        require(binding.getInt("format") == 1 && binding.getBoolean("supported") &&
            binding.getJSONArray("unsupported_save_kinds").length() == 0) { "This title declares unsupported native save kinds" }
        require(Regex("[0-9A-F]{16}").matches(binding.getString("program_id")) &&
            Regex("[0-9A-F]{16}").matches(binding.getString("save_owner_id")) &&
            Regex("[0-9A-F]{32}").matches(binding.getString("user_id"))) { "Invalid canonical save identity" }
        val words = binding.getJSONArray("user_uid_words")
        NativeSaveBindingRules.requireUserWords(binding.getString("user_id"),
            (0 until words.length()).map { words.getString(it) })
        val parts = listOf("account", "device").mapNotNull { kind ->
            val size = binding.getString("${kind}_size").toBigInteger()
            val journal = binding.getString("${kind}_journal_size").toBigInteger()
            require(size.signum() >= 0 && journal.signum() >= 0)
            val path = binding.optionalPath("${kind}_path")
            require((path == null) == (size.signum() == 0 && journal.signum() == 0))
            path
        }
        NativeSaveBindingRules.requirePhysicalParts(binding.getString("save_root"), parts)
        require(binding.getString("control_source").isNotBlank()) { "Missing authoritative control provenance" }
    }
}
