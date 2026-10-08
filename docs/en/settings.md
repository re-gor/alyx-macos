# Change one setting at a time

[Русский](../ru/settings.md) · [FAQ](faq.md)

Preferences live in the private installation root. Offline writes refuse an active prefix. `--restart` explicitly stops/relaunches **this** prefix; save first if progress matters.

```sh
./scripts/settings.sh
./scripts/stop.sh
./scripts/settings.sh --msaa 4
./scripts/launch_alyx.sh
# Or explicitly restart:
./scripts/settings.sh --msaa 2 --restart
```

| Option | Meaning | Current |
|---|---|---|
| `--msaa 2` / `--msaa 4` | Alyx `+vr_msaa VALUE`; tested CLI choices | 2 |
| `--hz 72` / `--hz 90` | Nominal headset/driver/encoder frequency | 72 |
| `--bitrate 300` | Compressed stereo-video target, Mbps; not per eye or USB speed | 300 |
| `--threads 10` | SW codec threads, not all processing stages | 10 |
| `--packet-size 16384` | TCP shard setting, not IP MTU; 1400/16384 accepted | 16384 |
| `--pacing off` | Remove nominal-vsync sleep on the server | off |
| `--ffr on` | Wide-center fixed foveated encoding | on |
| `--eye 1632 1632` | Render recommendation and pre-FFR target per eye; multiples of 32 | 1632² |
| `--profile current` | Last user-selected 72Hz profile | current |
| `--profile performance90` | Observed higher-cadence 90Hz profile | optional |

## MSAA is a launch argument

Alyx does not expose this control in its normal graphics UI. We use `+vr_msaa 2` with auto-fidelity off and level 3. `-msaa2` and early console text did not prove the actual value; the original experiment used a guarded read-only probe. The public launcher stores the preference and supplies it on each launch without patching the game DLL.

## Pacing and refresh

90Hz has 11.1ms ticks; 72Hz has 13.9ms ticks. Pacing on waits for a nominal tick after Present. Off can produce more frames, but may overrun delivery.

With old 1400-byte TCP shards, pacing off caused local drops and greater latency. At 90Hz/16KiB, A/B/A delivered about 45.5→70.5→45.5 fresh video reports/s with comparable estimated latency and no queue-full events in those windows.

72Hz did not improve our numbers: unchanged 300Mbps gave the software encoder more budget/frame through its nominal framerate/VBV. Encoded payload/frame rose about 25%; encoder-stage became 16.6ms versus 12.9ms and delivery fell to 55/s. The user chose to keep 72Hz. A 240Mbps/72Hz compensation test was **not performed**.

## Resolution and compression

Current rendering is 1632×1632/eye. Wide center 0.7/edge ratio 2 FFR produces 2816×1408 encoded stereo. A 1920×1080 companion window does not prove the VR input is 1080p.

Encoding remains software H264 ultrafast/zerolatency, all-I, no B-frames. No hardware VideoToolbox path is active. Higher bitrate improved clarity but adds transport/decode work. Threads do not automatically speed Map/color conversion. FIFO 2 bounds only one queue, not kernel/adb/decoder buffering.

Separate controls:

```sh
./scripts/settings.sh --profile performance90 --restart
./scripts/settings.sh --pacing on --restart
./scripts/settings.sh --ffr off --restart
./scripts/settings.sh --eye 2048 2048 --restart
```

Keep save/scene/head motion similar, finish alignment before measuring, and change one variable per comparison. Fresh decoded-video cadence is not pure game GPU FPS.

When starting with Steam's ordinary Play button, copy the desired `+vr_msaa VALUE` and fixed-fidelity options into the game's Steam Launch Options as described in [usage](usage.md). The settings script changes this project's launcher preferences; it does not rewrite your Steam account files.
