<h1 align="center">Duet Plus</h1>

<p align="center">
  A Nintendo Switch emulator for Android dual-screen handhelds, tuned for stability on the AYN Thor.
</p>

<p align="center">
  <a href="https://github.com/tapionallanaire/duet-plus/releases/latest"><img src="https://img.shields.io/github/v/release/tapionallanaire/duet-plus?style=for-the-badge&label=Download" alt="Download"></a>
</p>

## Why Duet Plus

Duet Plus fixes crashes, freezes and rendering problems in games that are hard to run on Adreno handhelds:

| Game | What's fixed |
|------|--------------|
| FINAL FANTASY RESONANCE | Crashes, freezes, red-tinted characters, blinking battle HUD and dialogue, missing objects |
| Pokémon Legends: Z-A | 30 FPS on the Thor without vanishing geometry |
| Double Dragon Revive | Stalls and flickering scenery |
| Dragon Quest VII Reimagined, Fire Emblem Engage | Characters and lights blown out to white |

It also hardens the GPU emulation against bad command data, accepts newer audio renderer revisions and improves VP9 video. See [Releases](https://github.com/tapionallanaire/duet-plus/releases) for the full list.

## Features

- **Second-screen companion.** The game on the main screen, and a touch companion page on the second display, reading the running game's memory every frame.
- **Installable companions.** Add a companion from the game's *Add-ons* menu with its `.dsmod.zip`; packages are validated before they are installed.
- **Second screen your way.** Swap the screens, stretch the companion, or open another app on the second screen for games without a companion.
- **Per-game fixes built in.** Games that need special settings get them automatically; your own per-game settings always win.

## Requirements

- Android 13 or newer, arm64 (`arm64-v8a`).
- A device with a second display (developed and tested on the AYN Thor).
- Your own legally obtained game dump, the matching update, and your console's keys and firmware.

## Install

1. Download the latest `duet-plus-*.apk` from [Releases](https://github.com/tapionallanaire/duet-plus/releases) and install it.
2. Set up the emulator: keys, firmware, and your games folder.
3. Optional: install a companion. Long-press the game, open **Add-ons**, tap **Install**, choose **Dual screen mods** and select the `.dsmod.zip`.

Duet Plus uses its own app ID (`com.tapionallanaire.duetplus`), so it installs beside other Switch emulators and keeps separate data.

## Second Screen

Second-screen options are in **Settings → Graphics → Second Screen**. Each game can have its own values: long-press the game, open **Settings → Graphics → Second Screen**. A game's own value overrides the global one; long-press a setting to reset it.

| Setting | Options | What it does |
|---------|---------|--------------|
| **Swap Screens** | On / Off | Switch which display runs the game and which shows the companion. |
| **Companion Ratio** | Fit (default) / Stretch | Fit keeps the companion's shape and adds bars if needed. Stretch fills the screen. |
| **No Companion** | Icon (default) / Black / Off / App | What the second screen shows for a game without a companion. App launches a guide app, browser, media player or any installed app on the second screen when the game starts. |

## Build

Requirements: JDK 17, Android SDK 36, NDK 28.2.13676358, CMake 3.31.6, and `glslangValidator` on `PATH`.

```sh
cd src/android
export ANDROID_HOME=$HOME/Android/Sdk
export ANDROID_NDK_ROOT=$ANDROID_HOME/ndk/28.2.13676358

# Local test build (installs beside the release build)
./gradlew assembleMainlineRelWithDebInfo

# Release build, signed with your own key
export ANDROID_KEYSTORE_FILE=/path/to/release.jks
export ANDROID_KEYSTORE_PASS=...
export ANDROID_KEY_ALIAS=...
./gradlew assembleMainlineRelease
```

The APK is written to `src/android/app/build/outputs/apk/mainline/<buildType>/`. A desktop build (`eden-cli`) builds on Linux and on Windows with clang-cl.

## Game Assets

Duet Plus ships **no game assets, keys, firmware or ROMs**. Do not upload any to this repository or its releases.

## License

Duet Plus is free software, released under the [GNU General Public License v3.0](LICENSE.txt). It is not affiliated with or endorsed by Nintendo; all trademarks belong to their respective owners.

<sub>Based on [Eden Duo](https://github.com/igawa6/eden-duo) and [Eden](https://git.eden-emu.dev/eden-emu/eden).</sub>
