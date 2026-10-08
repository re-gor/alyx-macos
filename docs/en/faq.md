# FAQ and troubleshooting

[Русский](../ru/faq.md) · [Settings](settings.md)

This is a vibe-coded experiment. A workaround on one machine is not a guaranteed fix. Keep backups and change one variable at a time.

## Menu camera is tilted, offset or follows the head strangely

Finish Quest's tracking/play-space setup first. Keep ALVR foreground and take both controllers. Removing/re-wearing Quest or putting it to sleep and waking it sometimes helped alignment in the original session; reopen ALVR if needed. No factory reset was required or tested. Reconnecting may pause Alyx: resume/load the save before judging FPS.

## Connected, but black/teal video

Connected is a control connection, not proof of pixels. Check pinned engine/ALVR, adapter loading and `allowDisplayLockedMode=true`. Run `doctor.sh`, then cold `launch_alyx.sh`. The original teal case involved shared-resource problems and Wine's false locked-session state. Avoid random backend changes or removing GPU completion waits.

## SteamVR 461 or critical failure

Check Wine 10.0_6/template 1.0.11 and the ALVR DLL hash. Inspect owned `logs/vrserver.log` and `vrcompositor.log` for missing adapters or rejected versions. Rebuild/configure while stopped, then cold-start. New SteamVR/Wine may require ABI work; do not remove guards to load an unknown binary.

## Mongoose timeout / WebHelper startup

Use the scripts providing `CX_FWD_COMPAT_GL_CTX=1`. Separate SteamVR WebUI textures still had limitations in the original work; Dashboard is disabled intentionally. Enabling it can capture input invisibly.

## No hands or buttons

Take both controllers. Confirm official x64 MSVC runtime/native override only for `vrserver.exe` and disabled Dashboard/autolaunch. Use the full launcher. Thumb-touch animation remains incomplete; it is a separate limitation, not proof that all tracking failed.

## Right eye shakes or trails

Use the current owner-checked keyed mutex. Cold-restart all participants after library updates; do not hot-mix protocols. A foreign release caused the demonstrated ownership violation; GPU visibility tests and a worn-headset check followed the fix. Removing completion synchronization is not a safe performance workaround.

## No headset sound / E_NOTIMPL

Update and configure the [automatic audio adapter](audio.md), then cold-start once to load the new library. Keep Game audio enabled and Quest's ALVR foreground. The current adapter follows SteamVR's active scene; manually caching the Alyx PID is obsolete. `audio_status.sh` reads observer metadata, not PCM; `ready` proves process mapping only, not audible output. No game means a quiet empty source.

Check capture consent for the wrapper/terminal macOS identifies. A missing new prompt does not prove failure. Unknown Wine/watcher binaries deliberately fail guards; retain their checks. A repeated `0x80004001` does not identify its cause alone. If the adapter enters an unsafe/poisoned state, stop this prefix and restart; do not destroy a tap still owned by Wine. Games emitting sound in another process need additional support.

## Robotic voices or gaps

Reference: host capacity 100ms, client average 50ms/batch 10ms. Capacity is not fixed added latency. Compare the same sound on Mac/Quest. Do not force 44100→48000 by guess; normal hardware resampling may occur. More video bitrate cannot restore audio already lost upstream.

## Menu/ambient exists, dialogue/interactions disappear

Compare Mac output first. If both outputs lack those sounds, the source game is already missing them. A save/reload restored them in the original session without a proven root cause or game-engine audio patch.

## Vulkan initialization failed

Use DX11:

```sh
./scripts/stop.sh
./scripts/fix_vulkan.sh
./scripts/launch_alyx.sh
```

The script backs up and restores the observed `boot.vcfg` choice; it does not implement Vulkan VR.

## Nominal high resolution still looks blurry

Check auto-fidelity off/level 3 and actual render/encode dimensions. The desktop window has a separate size. Raising bitrate improved detail in the experiment; all-I software encoding needs bandwidth. FFR reduces edge detail deliberately, so inspect the center too. See [settings](settings.md).

## Video lags by seconds while sound responds

Queued video is possible; that is not proof of a weak game GPU. FIFO 2 limits one queue only. Old small shards/pacing off showed Full drops and higher latency; later 16KiB/90Hz/off delivered more frames without observed queue-full. Kernel/adb/client queues remain separate. Try a cold reconnect and a stable-scene measurement.

## Should refresh match average FPS?

Not automatically. 90Hz can display/reproject cached images between new video frames. 72Hz also changes SW-codec framerate/VBV; our test increased bytes/frame and reduced delivery. Pacing affects waits/backpressure. Compare smoothness and estimated latency alongside the correct FPS metric.

## UDP/Wi-Fi?

The packaged path is USB/TCP. adb TCP forwards do not tunnel UDP; loopback/wired forces TCP in ALVR. 16KiB is unsuitable as a general UDP datagram size. Wi-Fi was parked, not tested end-to-end. Return packet size 1400 before a separately engineered wireless test.

## adb: missing, unauthorized or multiple devices

Use a data cable, Developer Mode, USB debugging and Quest's computer confirmation. Pass `--serial`. Do not globally kill adb during another session. Removing Quest or backgrounding ALVR can normally disconnect streaming.

## Are the scripts a supported product?

No. Original interactive playback was observed; package build/offline checks are separate, and a complete clean install is still unvalidated. Unknown binary/layout guards intentionally decline. Report small redacted version/symptom excerpts, not account directories or raw captures.
