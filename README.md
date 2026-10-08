# alyx-macos

[Русский](README.ru.md) · [Install](docs/en/install.md)

Windows **Half-Life: Alyx locally on Apple Silicon macOS**, using Wine/Sikarugir, Apple D3DMetal, our resource-sharing/audio adapters, Windows SteamVR and ALVR over USB.

**A vibe-coded hobby project made with AI, for fun. Experimental, unsupported, no warranty. Nobody promises it will work on your machine. Use at your own risk.** Original additions and documentation are MIT; credited upstream components keep their licenses.

The original installation displayed Alyx scenes, tracked hands/buttons and streamed audio on an M4 Pro 48 GB/macOS 15.8/Quest 3. The packaged adapters build, but the full fresh-machine installation workflow has **not** had a complete headset/game acceptance test. See [validation](docs/en/development.md).

**Audio update, 8 October 2026:** the user reports sound fixed in the original installation. This snapshot includes automatic active-game capture for the Steam → ALVR → SteamVR → game workflow; no manual audio PID selection. See [audio and upgrading](docs/en/audio.md). Other VR games and a complete fresh-machine installation remain unvalidated.

## Guides

1. [Install Wine, Steam, SteamVR, ALVR and the headset client](docs/en/install.md).
2. [Launch Alyx and open ALVR Settings directly](docs/en/usage.md).
3. [Change MSAA, refresh, bitrate, foveation and pacing](docs/en/settings.md).
4. [FAQ and troubleshooting](docs/en/faq.md).
5. [What we changed, step by step](docs/en/history.md).
6. [Architecture](docs/en/architecture.md) and [development](docs/en/development.md).
7. [Wine Retina/DPI/fonts/synchronization](docs/en/wine.md).

```sh
./scripts/doctor.sh
./scripts/install_wine.sh --dry-run
```

Steam login/downloads, vendor licenses, USB debugging and macOS capture consent require your own interaction. The guide explains their order.

## Included

- Modified UTM MIT `d3dmetal-native` source, Wine attachment and audio bridge.
- Focused shell commands backed by a standard-library Python CLI.
- Verified Wine/template/ALVR download hashes, profiles and scoped backups.
- Separate English/Russian guides, history, FAQ and settings reference.
- Offline tests. No games, Apple/Microsoft DLLs, APKs, prefixes, credentials or personal logs.

Default installation: `~/wine/alyx-macos/SteamVR.app`. An unrelated existing wrapper is rejected. ALVR web port: **8083**. USB uses adb forwards **9943/9944**. The tools never issue global `killall wine` or `adb kill-server`.

## Observed performance

At 1632×1632 rendered pixels/eye, wide FFR, software H264/10 threads, 300 Mbps target, 16 KiB TCP shards and pacing off: about 70–72 fresh video reports/s at 90 Hz in one scene. The last user-selected 72 Hz profile delivered about 55/s. These are **ALVR video measurements, not direct game GPU FPS or physical motion-to-photon latency**. Refresh also changes encoder budgeting.

Hardware VideoToolbox encoding and Wi-Fi were researched but are not the implemented playback route.

## License

[MIT](LICENSE) for our additions/docs; [third-party notices](THIRD_PARTY_NOTICES.md) retain upstream licenses. No affiliation or endorsement by Apple, Valve, Meta, Microsoft, Sikarugir or ALVR is implied.
