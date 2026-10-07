# Launch, settings UI and stopping

[Русский](../ru/usage.md) · [Install](install.md)

Call scripts from the repository or by absolute path. Append the same `--root PATH` used during installation.

| Task | Command |
|---|---|
| Windows Steam | `./scripts/start_steam.sh` |
| ALVR Dashboard directly | `./scripts/start_alvr.sh` |
| ALVR Settings | `./scripts/alvr_settings.sh` |
| Full game/VR/USB/audio launch | `./scripts/launch_alyx.sh` |
| Request existing save | `./scripts/launch_alyx.sh --save s0/quick` |
| Image-only diagnosis | `./scripts/launch_alyx.sh --no-audio` |
| Reconnect selected headset | `./scripts/connect_headset.sh` |
| Stop owned prefix | `./scripts/stop.sh` |
| Read audio-source readiness | `./scripts/audio_status.sh` |
| 30s fresh-video sample | `./scripts/measure.sh --seconds 30` |

For settings, open the Dashboard and select **Settings**; do not run the installer again. The running owned server also exposes `http://127.0.0.1:8083/`. This differs from the original development wrapper's port 8082.

## ALVR as a non-Steam game

Log into this Windows Steam once, exit it, then:

```sh
./scripts/stop.sh
./scripts/add_alvr_to_steam.sh
```

The script backs up binary `shortcuts.vdf`, retains existing entries and adds **ALVR Dashboard (alyx-macos)**. With several accounts, select the numeric local userdata directory using `--steam-userid`. Reopen `SteamVR.app` from Finder, find that entry in the Steam library and click **Play** — no Terminal is needed to open ALVR Settings afterward.

Manual alternative: Steam → Games → Add a Non-Steam Game → Browse → `C:\ALVR\ALVR Dashboard.exe` → Add Selected Programs.

This shortcut opens the Dashboard; it does not perform the complete ordered audio/game bootstrap. For a Finder entry to that bootstrap:

```sh
./scripts/make_gui_launcher.sh
```

It creates `Alyx VR.app` in the private root and logs to `logs/gui-launch.log`. Its GUI/game/audio acceptance is unvalidated; keep the repository and Python at their original paths.

## Known startup audio issue

Manual GUI starts showed silent headset audio/E_NOTIMPL; investigation is ongoing in the original experiment. A separate transient Steam exit also caused Alyx's SteamAPI initialization to fail before audio creation. The packaged launcher checks that owned Steam remains stable before starting the game, but that is **not a proven complete fix** for the GUI/audio issue. An absent source failure was not proven to be permanently cached. Historical successful audio and offline tests do not validate every new launch.

## Full launcher sequence

1. Refuse to use the headset while a different VR Wine prefix is running.
2. Stop only this prefix; clear the stale audio-source PID.
3. Open Steam and wait 30s for initialization; start ALVR, vrserver and vrcompositor without the invisible Dashboard/Home/monitor.
4. Start Alyx with DX11, fixed fidelity and saved MSAA.
5. Wait for this game's audio output, select its native PID, enable GameAudio and reconnect the USB client.

The cold restart is intentional: the audio tap caches one game source until vrserver exits. Restarting only `hlvr.exe` can leave audio attached to the old process. Scripts never globally kill Wine or stop a different wrapper.

Steam may need login/download/failure-confirmation dialogs. macOS may request capture consent; Quest must authorize USB debugging. Wear the headset, keep ALVR foreground, take both controllers, acknowledge the game warning and load a save if the menu remains.

The launcher passes `+vr_fidelity_level_auto 0 +vr_fidelity_level 3 +vr_msaa 2` using the saved MSAA value. Steam's ordinary **Play** button does not automatically inherit these project flags. `--save` requests a valid existing file; startup/menu interaction may still be needed.

## Private state and backups

The installation root contains `settings.json`, `downloads/`, `backups/`, `logs/`. Backup `index.json` maps files to original relative paths. Stop this prefix before restoring config/libraries; never hot-mix sharing protocol versions. Review/redact logs before sharing: they may contain usernames, device identifiers and network metadata. Do not upload the entire prefix, Steam account, saves or raw captures.
