# Install a separate wrapper

[Русский](../ru/install.md) · [README](../../README.md)

This creates a new local installation, not a copy of somebody's Steam account. The complete clean-install/headset workflow is not yet validated on a second machine.

## Prerequisites

- Apple Silicon, macOS 15+, Python 3.9+, Apple compiler/SDK and Rosetta 2.
- Your Steam account owning Alyx; enough space for Steam, the game and installer caches.
- Quest, USB data cable, Developer Mode and USB debugging. Quest 3 is the tested model.
- Internet for official vendor downloads and pinned Meson/Ninja in this repo's `.venv`.

If necessary, install Apple tools yourself:

```sh
xcode-select --install
softwareupdate --install-rosetta
```

Approve their prompts/licenses yourself. Scripts do not auto-accept licenses or disable Gatekeeper.

## 1. Get and inspect the project

```sh
git clone https://github.com/re-gor/alyx-macos.git
cd alyx-macos
./scripts/doctor.sh
./scripts/install_wine.sh --dry-run
```

Default root: `~/wine/alyx-macos`. Append `--root "$HOME/Games/alyx-macos"` to **every** command to use another empty folder. Existing unrelated wrappers are rejected.

## 2. Wine and Windows Steam

```sh
./scripts/install_wine.sh
./scripts/install_steam.sh
./scripts/start_steam.sh
```

The installer verifies WS12WineSikarugir10.0_6 and Template 1.0.11 SHA256 hashes. D3DMetal comes from that official template and is not redistributed here. Finish the Steam GUI at its default path, uncheck **Run Steam** on its last screen, then use `start_steam.sh` and log into your account. If a wizard remains after the SDK returns, finish it before proceeding.

## 3. SteamVR, Alyx and Microsoft runtime

```sh
./scripts/install_steamvr.sh
./scripts/install_alyx.sh
./scripts/install_vc_runtime.sh
```

Finish Steam's dialogs and wait for downloads. SteamVR is app 250820; Alyx is 546560. The reference experiment used SteamVR 2.17.10/Alyx build 25487405. New builds are unvalidated; scripts do not silently force undocumented depot downloads.

The Microsoft x64 v14 runtime uses its official interactive installer. Approve its license yourself. Native `msvcp140` is needed specifically for `vrserver.exe`; the original Wine builtin exhibited a synchronization/ABI failure. DLLs are not bundled here.

## 4. ALVR and adapters

```sh
./scripts/stop.sh
./scripts/install_alvr.sh
./scripts/build_adapter.sh
./scripts/configure.sh
./scripts/register_alvr.sh
./scripts/stop.sh
./scripts/add_alvr_to_steam.sh
./scripts/make_gui_launcher.sh
```

Build only compiles. Configure backs up files, installs adapters into this wrapper and sets the tested profile/native CRT override. Register adds `C:\ALVR` to this prefix's SteamVR. ALVR is a standalone streamer/driver, not a Steam-store app.

## 5. Headset client

Connect a USB data cable, wear Quest and approve **Allow USB debugging** for your computer. With existing adb:

```sh
./scripts/install_headset.sh --adb "$HOME/Library/Android/sdk/platform-tools/adb"
```

Alternatively, after reading [Google SDK terms](https://developer.android.com/studio/terms):

```sh
./scripts/install_headset.sh --download-adb --accept-platform-tools-license
```

Use `--serial YOUR_SERIAL` when several devices are connected. Do not install during another active ALVR session on that headset. The script does not kill another adb server.

Open ALVR in Quest, read its **Hostname**, for example `1234.client`, and pair that actual value:

```sh
./scripts/pair_headset.sh --hostname 1234.client
```

It stays in private local settings. Native Mac adb creates the tunnels; Windows adb wired mode is bypassed.

## 6. Launch and verify

```sh
./scripts/doctor.sh
./scripts/launch_alyx.sh
```

Wear Quest and keep ALVR foreground. Approve any macOS capture request yourself. Verify **both-eye image, hands, trigger/menu interaction and audible game sound**. `Connected` alone is not acceptance. Load a save through the game menu if required. Continue with [usage](usage.md) and [FAQ](faq.md).

Wine Retina/DPI/font details: [Wine configuration](wine.md). Steam non-Steam shortcut and Finder launcher: [usage](usage.md). The current GUI audio-start issue is still under investigation.
