# Eden Duo upstream-sync tools

Eden Duo tracks Eden nightly. These scripts keep that sync mechanical. They need
`bash`, `git` and `python3` (unlike the POSIX `sh` tools one level up) and must be
run from the repository root. Neither script ever pushes.

The upstream commit Duo is based on is recorded in `.upstream/eden.json`
(`source_commit`). The deliberate differences from upstream are listed in
`.upstream/DIVERGENCES.md`.

## `duo-touched-ledger.sh`

Read-only. Prints a TSV of every path where `HEAD` differs from the recorded
upstream commit, sorted by category:

| category | meaning |
|---|---|
| `duo-only` | file added by Duo; never conflicts |
| `upstream-hook` | upstream file Duo edits; a sync hotspot |
| `branding` | upstream file Duo replaces wholesale (identity, icons, About page) |

```sh
tools/duo/duo-touched-ledger.sh > .upstream/duo-touched.tsv     # refresh the ledger
tools/duo/duo-touched-ledger.sh --check .upstream/duo-touched.tsv
```

`--check` exits 1 when `HEAD` edits an upstream-owned file that the committed
ledger does not list. Run it before every release (or in CI). A failure means a
change added a new upstream hotspot: move the code into a Duo-owned file and keep
a one-line hook, or, if the edit is unavoidable, regenerate the ledger and add an
entry to `.upstream/DIVERGENCES.md` when it is a deliberate divergence.

The upstream commit must be present locally (`git fetch origin <sha>`).

## `duo-upstream-sync.sh`

```sh
tools/duo/duo-upstream-sync.sh --dry-run <upstream-commit>   # predict conflicts only
tools/duo/duo-upstream-sync.sh <upstream-commit>             # merge on a new branch
```

`--dry-run` writes nothing. It lists the predicted textual conflicts (using the
recorded upstream commit as an explicit merge base) and the upstream files that
changed and that Duo also edits; check those for API changes even when they merge
cleanly.

Without `--dry-run` it requires a clean tree, then:

1. creates `sync/eden-<sha>` from `HEAD` (adding a tree-unchanged bridge commit if
   the recorded upstream commit is not yet an ancestor);
2. enables `rerere` and adds `merge=ours` rules for Duo-owned branding files to
   `.git/info/attributes` (local only; no tracked file changes);
3. merges the upstream commit without committing, removes upstream AI-agent
   files, and updates `source_commit` in `.upstream/eden.json`;
4. lists the conflicts left to resolve.

Then resolve the conflicts, fill `release_tag` / `release_url` in
`.upstream/eden.json`, build, run the tests, review `.upstream/DIVERGENCES.md`
(drop entries upstream has fixed itself), refresh the ledger and commit.

Formatting rule: only format Duo-owned paths. Never run `clang-format` over whole
upstream files; reflowed upstream code conflicts on every sync.

## `daybreak-texture-check.py`

Not a sync tool: the pixel check from PR #12 for Trails through Daybreak II rendering
(stale or missing textures). It needs your own 1920x1080 captures of the scenes named in
its header and Pillow (plus ffmpeg for the video modes); it is not run by any test suite.

## `native-support-catalog.py`

Check Duet's native routing catalog against the private Peanuts and Banana source
worktrees; add `--write` only after reviewing the proposed diff:

```sh
python tools/duo/native-support-catalog.py --peanuts /path/to/peanuts --banana /path/to/banana
```

Peanuts uses `peanuts/release-titles.txt`, Switch 1 `nxGames` and Android
`readiness.tsv` status rows, plus reviewed private merges not declared in those
sources. Requirements alone do not declare Android support. Banana uses its
declared Android-ready Switch 2 profiles. Preserve every
registered code/module build allowed by `android_builds`; `shader_versions` is
capture provenance, not a supported-code allowlist. Prepare independently verifies
shader coverage. This does not make desktop-only profiles Android-ready.

Title recognition opens the native/emulated choice independently of exact-build
readiness. A mismatching version remains visible there but cannot run incompatible
native code. Updating this catalog also updates Gradle's embedded profile selection;
build with the same reviewed source worktrees.

Use the same private source commits for the catalog, Gradle's embedded hosts and
the pack-tool stages. Peanuts `dist/duet-20261122` at `e634a364` (the `ff366f2b` translator distribution, the
`dist/odin-sky108-20261122` Sky profiles, and prepared games kept across app updates) retains Sky 1st Chapter 1.0.4 and adds
the exact 1.0.8 build from the Odin installation; each build requires its own
shader set. The cancelled intermediate 1.0.7 generation can be retired without
removing published packs, inputs or saves. The catalog contains 72 Switch 1 and
10 Switch 2 programs (100 registered builds).

From this repository's root, with `P` and `B` set to clean, separately owned
checkouts of `tapionallanaire/peanuts-native` and `tapionallanaire/banana-native`:

```sh
git -C "$P" switch --detach e634a364b4b6bb603b7985a5d1a2f4caf5616bf7 &&
git -C "$B" switch --detach 7296c8b090ca31f0c7cd2a21b7a61fe0212f2ff4 &&
python tools/duo/native-support-catalog.py --peanuts "$P" --banana "$B"
```

The Peanuts commit is on the owner's private distribution branch `dist/duet-20261122`, the
Banana commit on `dist/duet-20261121` (`0056ba53` plus the same launch fix). Pass `-PduetPeanutsSourceDir="$P"` and `-PduetBananaSourceDir="$B"`
using absolute paths. Generate each pack-tool stage from its corresponding
checkout: a matching catalog alone does not authenticate the staged tools.

All ten Switch 2 Native Play hosts pack small Vulkan images into the fullest existing block with
an aligned fit before requesting another 64 MiB. Dedicated images keep the driver's allocation.
The hosts also reject stale Vulkan caches before reading the payload and skip diagnostic key
formatting on ordinary mesh draws. No cache migration or new user setting is required.
Both distributions retain the embedded hosts and exclude captured prewarm seeds. Use each engine's
matching packer stage; the Hogwarts shader reference must pass its packaging check.

The pinned distribution is in `tools/duo/peanuts.rev` (commit and `DMSH_VK_CHAIN`). A build with
`-PduetPeanutsSourceDir` fails unless that source has the pinned `DMSH_VK_CHAIN` and, for a git
checkout, the pinned commit; pass `-PduetPeanutsSnapshot=true` for a sanitized release snapshot
(another commit, same contents), which is then checked by its chain version only. Update both
lines of `peanuts.rev` with the README pin above whenever Peanuts is synchronized.

The Peanuts translator fix keeps the handle word of a bindless texture handle that the
guest spilled to local memory and reloads (Hogwarts Legacy dropped its Niagara particle
writes: spell effects stuck as a bright disc with exposure flicker). It changes
`DMSH_CHAIN` to `4c58e2225984` and `DMSH_VK_CHAIN` to `b1683fc3bd6d` (ABI `cb5a89b39291`
unchanged): packs carry `shaders/v-<DMSH_VK_CHAIN>/`, so every natively prepared game must
rebuild its shader library once (Prepare again) after this update.

For Three Houses 1.2.0, Compile & play captures the nine startup shaders from
the selected game on the device. The capture process uses a disposable save;
its provisional pack is never published. Native play still requires all 1,372
shaders. Do not ship a capture directory or copy one from another device.

This sync adds Crisis Core Reunion, The Dark Prince, Fantasian Neo Dimension,
Dragon Quest I & II HD-2D Remake, and Kingdom Hearts Final Mix (collection
program 1, not the menu or program 3). Recognition is not a gameplay result:
The Dark Prince stalled before the title screen in standalone testing.
Dragon Quest VII is not enabled: its strict shader build needs a compiler
change that is not in the pinned Peanuts source.

Readiness keeps the first identified program's actual admission failure on the
fullscreen choice page. Native preparation reads the original game, selected
official update and enabled DLC directly. Emulator mods stay enabled for
emulation; they are neither applied to native packs nor used to block them.
For Switch Save Sync, local units are sixteen-hex title directories only.
Native `.lock`, `.work`, `.new`, `.re` and `.restore` siblings are not Sync units
and must not be removed to make enumeration succeed. Remote units still pass the
existing strict name and canonical-parent checks.

Native save-lease replies cross Binder with an app-defined receiver. Set the
received Bundle's app class loader before reading any field, including the
token: older Android versions may eagerly unpack every Parcelable.

Native game processes also need separate tasks: the embedded manifests use
`${applicationId}.native`, and the caller explicitly supplies `FLAG_ACTIVITY_NEW_TASK`.
Affinity and `singleTask` alone can leave a result launch in the caller's task;
Android's native-crash cleanup then finishes both activities. An immediate canceled
result is expected with the explicit flag: wait for save-lease release and the
native exit record rather than treating that result as a completed game.
Keep startup failures first in the fallback toast; Android's two-line limit can
otherwise hide the cause behind the generic emulation message.
