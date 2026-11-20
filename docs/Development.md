# Development guidelines

## License Headers

All commits must have proper license header accreditation.

You can easily add all necessary license headers by running:

```sh
git fetch origin master:master
.ci/license-header.sh -u -c
git push
```

Alternatively, you may omit `-c` and do an amend commit:

```sh
git fetch origin master:master
.ci/license-header.sh
git commit --amend -a --no-edit
```

If the work is licensed/vendored from other people or projects, you may omit the license headers. Additionally, if you wish to retain authorship over a piece of code, you may attribute it to yourself; however, the code may be changed at any given point and brought under the attribution of Eden.

For more information on the license header script, run `.ci/license-header.sh -h`.

## Embedded native play

Android 13+ builds can include the Peanuts Switch 1 and Banana Switch 2 modules.
`NativeGameSupport` recognizes supported titles before checking the effective
installed update, program index and full module build IDs. Recognized titles reach
the full-screen Emulated / Native play page unless a per-game mode overrides it;
exact build and readiness checks gate native Prepare/play. Unknown titles use
ordinary emulation. Switch 2 editions retain the original program 0 RomFS and the
selected program 1, rather than relabelling a base dump.

`PreparedNativeGame` calls each engine's `EmbeddedGame` facade through reflection;
stock builds need no engine dependency. Duet owns the full-screen launch, export,
`NativeCompilationActivity` and per-game settings surfaces. The selected Duet
global/per-game GPU driver is supplied to the private child process; there is no
second driver preference. Required upstream licence notices remain intact.

Native and emulated play use the canonical selected-account/device save paths
resolved from original retail control metadata. No per-mode save copy is made.
`NativeSaveSession` hands the child a duplicated kernel lease through Binder and
retains durable recovery/upload records outside replaceable save trees. Activity
destruction is not proof that a writer has stopped. The native child keeps its
lease until process death and reuses its admitted binding across recreation,
without recovering a live writer's journal. Embedded native Sync is disabled;
Duet remains the only Sync client.

The parent defers core initialization while a surviving child owns saves.
Recovery must finish under the lease before emulation, Sync or maintenance can
touch those saves. Never weaken this to an Activity flag or stopped-file check.

Per-game **Native play** settings remember Ask / Always emulate / Prefer native
when prepared. Native preference never silently grants an expensive incompatible
rebuild. Recompile creates a fresh generation while retaining the last valid pack
until verified promotion. Delete compiled data is source/title-owned and excludes
original games, saves and drivers. Private input-export ownership is recorded
before bytes are written, so cancelled pre-job exports remain maintainable.

Refresh routing after reviewing native source updates:

```sh
python tools/duo/native-support-catalog.py --peanuts <private-worktree> --banana <private-worktree> --write
python tools/duo/native-support-catalog.py --peanuts <private-worktree> --banana <private-worktree>
python tools/duo/test_native_support_catalog.py
```

Peanuts uses its release manifest, Switch 1 Android declarations (`nxGames`
or `readiness.tsv` status), and reviewed private Android merges that lack either
declaration. Never select every profile directory: that includes desktop-only
games. For collections the program ID identifies the executable: KH2 is program 3,
not the collection menu. Build the embedded modules/assets from those same
reviewed sources; regenerating this catalog alone does not update an APK.

Device acceptance still requires backed-up disposable progress, clean native
exit, startup-failure fallback, compiler cancel/retry, effective-driver checks and
both shared-save directions. Back up the user's real saves before any real-save
access; the standalone-current Hogwarts seed must precede testing on Odin.

## Android save maintenance boundaries

Save/profile maintenance holds `NativeSaveSession.acquire(context).use { ... }`
through every live-tree read, copy, replacement, deletion, and profile mutation.
Acquisition recovers pending shared-native journals first and refuses access while
a launch reservation or game owns the kernel lease. Do not replace this with an
`isRunning` check. Capture `identity(context)` when opening a picker or confirmation,
then call `assertIdentity` under the acquired lease before applying its result.
Changing the selected account or physical save root invalidates the pending action.

Ordinary leases are thread-reentrant blocking scopes: finish file work and release
them before suspending for UI updates. Startup nests ordinary access under the
application's initialization lease. `acquireDetached` is for SAF descriptors; it
has no thread-local reentry and can be closed on another thread.
Save/profile SAF descriptors retain detached leases through a mediated AppFuse
proxy; the canonical file descriptor is never exported. Client duplicates cannot
continue file access after the provider process dies. The backend serializes
offset reads/writes, append, truncate, fsync, cancellation, and release; after
release it rejects further I/O before releasing save ownership.
Read-only game-source access outside canonical save/profile/config trees uses a
lock-free protected-tree snapshot, so native metadata resolution and emulation
can read original dumps through this provider without reentering a held session.
Save ancestors, save backups, all writes, and unknown/deferred snapshots remain
guarded. Live config/root replacement invalidates the snapshot before mutation
and refreshes it under the maintenance lease after reload.
Existing unleased sources revalidate their pathname/inode and current boundary on
each callback, refusing renamed/replaced sources or newly protected paths. A short
independent read/write gate covers only each callback or metadata operation, never
the descriptor lifetime. Contention visibly refuses instead of waiting on a source
reader: no root invalidation or SAF path mutation proceeds concurrently with that
reader. Proxy callbacks report `EBUSY`, and the gate never takes the session monitor.
The default-process `SaveSyncJob` retries guarded `YuzuApplication.initializeHost`
off-main when startup is deferred, so recovery/upload resumes after native exits
without opening an Activity; busy/recovery failures reschedule.
Warm-up backups record their account/root identity and refuse restoration into a
different selection; stopped emulation restores under the emulation owner's lease.

Native install/start/run failures persist a fallback phase, wait for actual kernel
lease release and explicit journal recovery, then enter normal emulation preflight
with the failure reason. Save/Sync admission or recovery errors visibly refuse
instead of auto-looping. Compile **Cancel** remains the user's preparation-page
choice. Kernel-lifetime regression sources do not establish Android lifecycle
behavior; the corresponding device proof remains pending.

An existing legacy Switch peer without recorded ownership remains blocked until
the user opens **Save sync** and explicitly confirms its original account. The
confirmation shows the selected account, physical account root, and account-only
`switch/<save-owner ID>` scope. Its callback revalidates that selection under the
lease before persisting ownership; a pinned peer cannot be repointed. Disconnect
or a profile switch never clears unknown ownership implicitly. Confirmation alone
does not copy, seed, restore, or merge saves; reconciliation remains a later
normal launch/exit sync or explicit **Sync now** action.

Source regression command, from `src/android` (not device acceptance):

```sh
./gradlew :app:testMainlineDebugUnitTest --tests org.yuzu.yuzu_emu.features.DocumentProviderPathsTest --tests org.yuzu.yuzu_emu.features.DocumentProxyFileTest
```

After integration, use disposable profiles/saves only to check: reserve or run a
native/emulated game and attempt import/export/delete, profile changes, data
replacement, save transfer, and a save/profile SAF open; each must visibly refuse without a
success message. Open a chooser/overwrite confirmation, change account or save
root before accepting it, and verify no old selection is applied. Keep a save SAF
descriptor open and verify a game cannot reserve saves until it closes. Duplicate
that proxy descriptor, kill the provider, and confirm the surviving client cannot
read/write canonical saves after a new session starts. Confirm ordinary read-only
game dumps outside protected trees still load through this provider during native
metadata resolution and emulation. These Binder/FUSE/provider-death checks require
Android; host backend tests do not establish them. With no
session active, repeat normal operations, including transfer browsing after an
account change and warm-up backup/restore. Crash an isolated native test session
with a pending journal and verify the next maintenance access recovers before
reading; these are pending manual checks, not automated-test coverage.


## Adding new settings

When adding new settings, use `tr("Setting:")` if the setting is meant to be a field, otherwise use `tr("Setting")` if the setting is meant to be a Yes/No or checkmark type of setting, see [this short style guide](https://learn.microsoft.com/en-us/style-guide/punctuation/colons#in-ui).

- The majority of software must work with the default option selected for such setting. Unless the setting significantly degrades performance.
- Debug settings must never be turned on by default.
- Provide reasonable bounds (for example, a setting controlling the amount of VRAM should never be 0).
- The description of the setting must be short and concise, if the setting "does a lot of things" consider splitting the setting into multiple if possible.
- Try to avoid excessive/redundant explainations "recommended for most users and games" can just be "(recommended)".
- Try to not write "slow/fast" options unless it clearly degrades/increases performance for a given case, as most options may modify behaviour that result in different metrics accross different systems. If for example the option is an "accuracy" option, writing "High" is sufficient to imply "Slow". No need to write "High (Slow)".

Some examples:

- "[...] negatively affecting image quality", "[...] degrading image quality": Same wording but with less filler.
- "[...] this may cause some glitches or crashes in some games", "[...] this may cause soft-crashes": Crashes implies there may be glitches (as crashes are technically a form of a fatal glitch). The entire sentence is structured as "may cause [...] on some games", which is redundant, because "may cause [...] in games" has the same semantic meaning ("may" is a chance that it will occur on "some" given set).
- "FIFO Relaxed is similar to FIFO [...]", "FIFO Relaxed [...]": The name already implies similarity.
- "[...] but may also reduce performance in some cases", "[...] but may degrade performance": Again, "some cases" and "may" implies there is a probability.
- "[...] it can [...] in some cases", "[...] it can [...]": Implied probability.

Before adding a new setting, consider:

- Does the piece of code that the setting pertains to, make a significant difference if it's on/off?
- Can it be auto-detected?

# IDE setup

## VSCode

Copy this to `.vscode/settings.json`, get CMake tools and it should be ready to build:

```json
{
    "editor.tabSize": 4,
    "files.watcherExclude": {
        "**/target": true
    },
    "files.associations": {
      "*.inc": "cpp"
    },
    "git.enableCommitSigning": true,
    "git.alwaysSignOff": true
}
```

You may additionally need the `Qt Extension Pack` extension if building Qt.

# Build speedup

If you have an HDD, use ramdisk (build in RAM), approximately you need 4GB for a full build with debug symbols:

```sh
mkdir /tmp/ramdisk
chmod 777 /tmp/ramdisk
# about 8GB needed
mount -t tmpfs -o size=4G myramdisk /tmp/ramdisk
cmake -B /tmp/ramdisk
cmake --build /tmp/ramdisk -- -j32
umount /tmp/ramdisk
```

# Assets and large files

A general rule of thumb, before uploading files:

- PNG files: Use [optipng](https://web.archive.org/web/20240325055059/https://optipng.sourceforge.net/).
- SVG files: Use [svgo](https://github.com/svg/svgo).

May not be used but worth mentioning nonetheless:

- OGG files: Use [OptiVorbis](https://github.com/OptiVorbis/OptiVorbis).
- Video files: Use ffmpeg, preferably re-encode as AV1.
