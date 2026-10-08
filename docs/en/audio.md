# Automatic audio from the active VR game

[Русский](../ru/audio.md) · [Usage](usage.md) · [Build checks](development.md)

On 8 October 2026 the user reported sound fixed in the original installation. This repository now includes that active-scene audio implementation. The complete installation on another Mac and switching between arbitrary VR games remain unvalidated.

## Update an existing managed installation

From your local project checkout, with no uncommitted changes:

```sh
git pull --ff-only
./scripts/stop.sh
./scripts/build_adapter.sh
./scripts/configure.sh
```

Use your original `--root PATH` on each script if it differs from the default. This update is for installations created by this repository's installer; it refuses unrelated wrappers. Build does not install or capture. Configure backs up the adapters/plist/settings, installs the new bridge and `C:\ALVR\AudioSceneWatcher.exe`, enables Game audio, and writes scene-mode exports into the wrapper. Existing saved profile values and paired clients are retained. Steam remains stopped afterward.

Restart through the [normal Steam workflow](usage.md#normal-steam-workflow). Keep Game audio enabled before starting the game. A separate app for each game and manual `audio-source.pid` selection are no longer needed. Do not combine a newly built bridge with a different watcher or Wine engine.

## What changed

1. The Wine audio table registers independently of graphics readiness; GUI startup no longer has to wait for a graphics hook to select audio interception.
2. A read-only OpenVR Background observer checks the existing vrserver and uses `GetCurrentSceneProcessId`. It does not start games or take scene/input focus.
3. The bridge maps Windows→native PID through the exact pinned Wine protocol and verifies process creation time, user, prefix and executable identity. Stale/foreign observations are rejected.
4. The private, nonexclusive stereo CoreAudio tap starts with an empty included-process list. It follows the verified game and preserves tap UID/ASBD across retargets; it never falls back to all Mac audio or the microphone. Mac playback stays unmuted.
5. Owned capture Stop/Reset and borrowed-buffer checks coordinate changes. Empty `GetNextPacketSize` calls also poll the scene, avoiding a deadlock where CPAL never reaches GetBuffer on an empty tap. If validation fails, the adapter withholds managed PCM rather than selecting another source.

The intended sequence is idle → active game → idle → new game. Fake tests exercise these transitions. The user's sound report does not independently validate every transition or title with a real headset. Audio emitted by a separate helper process is not automatically associated with its game.

## Diagnostics and recovery

```sh
./scripts/audio_status.sh
./scripts/doctor.sh
```

Audio status reads only the running owned vrserver's observer metadata. `ready=1` confirms identity mapping, not PCM delivery or audible headset sound. Check observation age; a state file can outlive its observer. Before an eligible game appears, `ready=0`/empty source is expected. Without an active ALVR client, capture callbacks may not run yet.

Approve macOS audio-capture permission for the wrapper/terminal identified by the system. If the error is `0x80004001`, inspect version/guard/loading errors; that code alone does not establish the cause. An unsafe tap or failed Stop/Reset requires stopping this prefix and restarting, not removing checks or destroying a live Wine-owned device. See [FAQ](faq.md).

Client audio buffering remains average 50 ms/batch 10 ms; host capture capacity is 100 ms. The audio fix does not require different video, bitrate, pacing, refresh or MSAA values. Configure reapplies the profile saved in `settings.json`; manual Dashboard changes not reflected there can be overwritten. Review your saved profile before configuring. Those controls are documented [separately](settings.md).
