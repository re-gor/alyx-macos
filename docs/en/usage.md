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
| Read scene observer metadata | `./scripts/audio_status.sh` |
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

## Normal Steam workflow

1. Connect Quest by USB, wear it and open ALVR in the headset. Create the native Mac adb tunnel with `./scripts/connect_headset.sh` after connecting the cable; repeat if the tunnel is lost. The script uses the selected authorized device and does not stop other adb servers.
2. Open `SteamVR.app` in Finder. Despite its name, the wrapper opens **Windows Steam**. Wait for your library/login to finish.
3. Open **ALVR Dashboard (alyx-macos)** from that Steam library, then start SteamVR from ALVR or Steam.
4. Keep **Settings → Audio → Game audio** enabled. The adapter selects the active SteamVR game automatically; do not write an `audio-source.pid` file or choose a PID by hand.
5. In Alyx's Steam **Properties → General → Launch Options**, set these options once:

```text
-dx11 -nomultiview -novid +vr_fidelity_level_auto 0 +vr_fidelity_level 3 +vr_msaa 2
```

6. Start Alyx from Steam. Wear Quest, take both controllers and verify image, controls and audible game sound.

The observer runs in the background and selects only the active scene process. With no eligible game, the source is empty. The implementation can retarget without restarting vrserver; switching between arbitrary VR titles has not been accepted on a headset. Details and upgrade steps: [audio](audio.md).

## Scripted launch

`launch_alyx.sh` remains an optional cold bootstrap: it refuses another running VR prefix, stops only this prefix, starts Steam/ALVR/vrserver/compositor, checks Steam stability, starts Alyx with saved fidelity/MSAA and reconnects USB. Game audio is enabled before the game and follows the scene automatically. `--no-audio` keeps Game audio off for diagnosis.

The cold restart is a launcher choice, **not a requirement to select a new audio PID**. `--save` requests a valid existing save; menu interaction may still be needed. macOS may request capture consent; approve it for the wrapper/terminal identified by macOS. No prompt by itself does not prove failure.

`make_gui_launcher.sh` can create an optional `Alyx VR.app` for this full scripted bootstrap. It is not required for normal Steam launches and has not received a separate fresh-install GUI acceptance test. Keep the repository/Python paths in place; its log is `logs/gui-launch.log`. Old separately created launchers may load old adapters; do not use one as an upgrade method.

## Private state and backups

The installation root contains `settings.json`, `downloads/`, `backups/`, `logs/`. Backup `index.json` maps files to original relative paths. Stop this prefix before restoring config/libraries; never hot-mix sharing protocol versions. Review/redact logs before sharing: they may contain usernames, device identifiers and network metadata. Do not upload the entire prefix, Steam account, saves or raw captures.
