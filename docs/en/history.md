# What we changed, step by step

[Русский](../ru/history.md) · [Architecture](architecture.md)

This records the original experiment; portable installation scripts are a later packaging step, not a completed fresh-machine VR acceptance test.

1. Created Sikarugir/Wine 10.0_6 wrapper; installed Windows Steam, SteamVR and purchased Alyx. Used Rosetta and DX11→D3DMetal 3.0→Metal.
2. Installed ALVR 20.14.1 streamer/driver and matching Quest APK; native adb TCP forwards 9943/9944 connected USB.
3. Adapted UTM's MIT `d3dmetal-native` to Wine's existing graphics host: legacy 32-bit handles, Unix broker, shared buffer/texture import/export. The compositor path blocked by SteamVR 461 started working.
4. Fixed shared-texture compatibility: shadow-heap cube-array case and delayed zero-initialization erasing peer-written pixels.
5. Added guarded ALVR error formatting fallback: failed FormatMessage no longer passes NULL to wcslen; retained HRESULT.
6. Set `allowDisplayLockedMode=true`: unsupported Wine WTS queries made SteamVR think the session was locked. Teal placeholder became an actual scene.
7. Added `CX_FWD_COMPAT_GL_CTX=1` for WebHelper GL-context creation associated with the observed Mongoose timeout/UI failure.
8. Disabled invisible Dashboard/autolaunch/Home/monitor input capture. Native Microsoft `msvcp140` only for vrserver repaired the observed mutex/condition-variable ABI behavior. Hands and trigger/menu interaction worked.
9. Fixed keyed-mutex ownership: another device could release somebody else's texture acquisition. Validate PID/canonical device identity and publish release after GPU completion. Right-eye lag/shaking synchronized in the visual check.
10. Added process-specific CoreAudio tap/aggregate and guarded Wine loopback adapter. Selected the fresh game PID; kept normal ALVR PCM transport. Headset sound was user-confirmed.
11. Increased host ring capacity to 100ms; selected client 50ms/batch 10ms. Reported gaps/robotic pitch improved. Capacity is not fixed added latency.
12. Pinned render 1632²/eye, auto-fidelity off/level 3 and verified MSAA 2/4. Higher bitrate improved detail; desktop-window size was not eye-render size.
13. Enabled wide FFR, 10 SW-codec threads and server FIFO 2. Full drops the new frame; this is not whole-pipeline latest-frame-wins.
14. TCP shard setting 1400→16384: FPS remained around 45 in sequential samples, estimated median latency 84→79ms, p95 95→83ms.
15. Pacing on/off/on at 90Hz/16KiB: about 45.5→70.5→45.5 fresh reports/s, comparable latency, no queue-full events. Selected pacing off.
16. Tried 72Hz with 300Mbps unchanged: about 55/s versus fresh 90Hz baseline 71.6/s; encoded payload/frame rose about 25%. User kept 72Hz. `performance90` remains an alternative.

We did not prove the M4 GPU was the main bottleneck. Encoder-stage includes waits/readback/conversion/codec; game-stage is not a GPU timer. VideoToolbox, Wi-Fi, DXMT replacement and further queue/GPU-event architecture remained research, not deployed playback fixes.

Remaining issues include incomplete thumb-touch animation, generalized NT-handle/fence support, audio source restart lifecycle and clean installation on other machines. This is a vibe-coded hobby experiment, not universal supported Windows VR on Mac.
