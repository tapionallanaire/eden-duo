# Android

## Dependencies

* [Android Studio](https://developer.android.com/studio)
* [NDK 30+ and CMake 3.31.6](https://developer.android.com/studio/projects/install-ndk#default-version)
  * `$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager "cmake;3.31.6" "ndk;30.0.16138531"`
* [Git](https://git-scm.com/download)

## WINDOWS ONLY - Additional Dependencies

* **[Visual Studio](https://visualstudio.microsoft.com/downloads/)** - **Make sure to select "Desktop development with C++" support in the installer. Make sure to update to the latest version if already installed.**
* **[Vulkan SDK](https://vulkan.lunarg.com/sdk/home#windows)** - **Make sure to select Latest SDK.**
  * A convenience script to install the latest SDK is provided in `.ci\windows\install-vulkan-sdk.ps1`.

## Cloning Eden with Git

```sh
git clone --recursive https://git.eden-emu.dev/eden-emu/eden.git
```

Eden by default will be cloned into:

* `C:\Users\<user-name>\eden` on Windows
* `~/eden` on Linux and macOS

## Building

1. Start Android Studio, on the startup dialog select `Open`.
2. Navigate to the `eden/src/android` directory and click on `OK`.
3. In `Build > Select Build Variant`, select `release` or `relWithDebInfo` as the "Active build variant".
4. Build the project with `Build > Make Project` or run it on an Android device with `Run > Run 'app'`.

## Building with Terminal

1. Download the SDK and NDK from Android Studio.
2. Navigate to SDK path.
3. Then set ANDROID_HOME in terminal via
`export ANDROID_HOME=path/to/sdk`
4. Navigate to `eden/src/android`.
5. Then Build with `./gradlew assembleRelWithDebInfo`.
6. To build the optimised build use `./gradlew assembleGenshinSpoofRelWithDebInfo`.
7. You can pass extra variables to cmake via `-PYUZU_ANDROID_ARGS="-D..."`

Remember to have a Java SDK installed if not already, on Debian and similar this is done with `sudo apt install openjdk-17-jdk`.

### Script

A convenience script for building is provided in `.ci/android/build.sh`. On Windows, this must be run in Git Bash or MSYS2. This script provides the following options:

```txt
Usage: build.sh [-c|--chromeos] [-t|--target FLAVOR] [-b|--build-type BUILD_TYPE]
                [-h|--help] [-r|--release] [extra options]

Build script for Android.
Associated variables can be set outside the script,
and will apply both to this script and the packaging script.
bool values are "true" or "false"

Options:
    -c, --chromeos          Build for ChromeOS (x86_64) (variable: CHROMEOS, bool)
                            Default: false
    -r, --release           Enable update checker. If set, sets the DEVEL bool variable to false.
                            By default, DEVEL is true.
    -t, --target <FLAVOR>   Build flavor (variable: TARGET)
                            Valid values are: legacy, optimized, standard
                            Default: standard
    -b, --build-type <TYPE> Build type (variable: TYPE)
                            Valid values are: Release, RelWithDebInfo, Debug
                            Default: Debug

Extra arguments are passed to CMake (e.g. -DCMAKE_OPTION_NAME=VALUE)
Set the CCACHE variable to "true" to enable build caching.
The APK and AAB will be output into "artifacts".
```

Examples:

* Build legacy release with update checker:
  * `.ci/android/build.sh -r -t legacy`
* Build standard release with debug info without update checker for phones:
  * `.ci/android/build.sh -b RelWithDebInfo`
* Build optimized release with update checker for ChromeOS:
  * `.ci/android/build.sh -c -r -t optimized`

### Duet Plus: Odin 2 in-place update

Build from the integration checkout. The `mainlineRelease` APK contains the emulator;
both Native Play hosts are optional Gradle inputs, **not** included by a bare
`assembleMainlineRelease`. Use separate build worktrees for the exact Peanuts and
Banana revisions in [the native catalog notes](../../tools/duo/README.md#native-support-catalogpy).
Set `PEANUTS` and `BANANA` to their absolute paths and `PEANUTS_STAGE` and
`BANANA_STAGE` to the corresponding engine packer `stage` directories. Generate
them from those same revisions with Peanuts `peanuts/scripts/ondevice-stage.sh`
and Banana `android/ondevice/stage-linux.sh`, respectively. Do not reuse captured
game data or set `PN_PREWARM_SEEDS`. Install both the app NDK (`30.0.16248370`)
and the native hosts' NDK (`28.2.13676358`).

From the integration repository root, in Bash:

```bash
set -eu
python3 tools/duo/native-support-catalog.py --peanuts "$PEANUTS" --banana "$BANANA"
cd src/android
./gradlew :app:assembleMainlineRelease -Pnightly=false \
  "-PduetPeanutsSourceDir=$PEANUTS" "-PnxPackStage=$PEANUTS_STAGE" \
  "-PduetBananaSourceDir=$BANANA" "-PduetBananaPackStage=$BANANA_STAGE"
apk=app/build/outputs/apk/mainline/release/app-mainline-release.apk
```

The build can use the bundled development signer as an intermediate. **Do not
install that APK.** On the owner's signing host, set `OWNER_KEYSTORE`,
`OWNER_ALIAS`, and `OWNER_LINEAGE` to the existing release keystore, alias and
proof-of-rotation file. Put Android SDK Build Tools 36.0.0 on `PATH`; let
`apksigner` prompt for the password. Write a separate APK, without publishing it:

```bash
signed=duetplus-odin-update.apk
apksigner sign --ks "$OWNER_KEYSTORE" --ks-key-alias "$OWNER_ALIAS" \
  --lineage "$OWNER_LINEAGE" --rotation-min-sdk-version 28 --min-sdk-version 33 \
  --v1-signing-enabled false --v2-signing-enabled false --v4-signing-enabled false \
  --out "$signed" "$apk"
```

Before handing off the signed APK, run this offline gate with `PREVIOUS_APK`
pointing to the retained **owner-signed** Odin build `34372248`, not a development
APK. Android orders updates by `versionCode`, not the display name. Gradle derives
the code from build time (ten-second intervals since 2016-01-01 UTC).

```bash
set -euo pipefail
package=$(aapt2 dump badging "$signed" | sed -n '1p')
[[ $package == "package: name='com.tapionallanaire.duetplus' "* ]]
[[ $package =~ versionCode=\'([0-9]+)\' ]]
(( 10#${BASH_REMATCH[1]} > 34372248 ))
cert='Signer #1 certificate SHA-256 digest: 2d7c097ce3e35549c94e29d6e96ceb4b64b6fc568afb219dc514d4e057303add'
for file in "$PREVIOUS_APK" "$signed"; do
  apksigner verify --min-sdk-version 33 --print-certs "$file" | tr -d '\r' | grep -Fx "$cert"
done
old=$(apksigner lineage --in "$PREVIOUS_APK" --print-certs -v)
new=$(apksigner lineage --in "$signed" --print-certs -v)
[[ -n $old && $old == "$new" ]]
```

Stop on failure; do not uninstall, clear data or force a downgrade. For side-by-side
development keep using `assembleMainlineRelWithDebInfo` (`com.tapionallanaire.duetplus.dev`).
Neither signing nor checking the APK requires a connected device or changing an earlier branch.

### Additional Resources

<https://developer.android.com/studio/intro>
