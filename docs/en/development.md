# Build, tests and validation boundaries

[Русский](../ru/development.md) · [Architecture](architecture.md)

UTM base: `f0dd1fafded509c9a949535c773fdc4e3ab07a71`. `config/source-snapshot.json` records the packaged source. The 8 October audio snapshot includes active-scene selection, early loopback registration and empty-packet polling. The user reports sound fixed in the original installation; a fresh-machine install remains unvalidated.

```sh
./scripts/build_adapter.sh
python3 -m unittest discover -s tests -v
./scripts/test_offline.sh
```

Build requires an existing wrapper/framework, Apple compiler/SDK and pinned Meson/Ninja. It uses two jobs by default and writes `.build/` only. It does not start games, GPU tests, capture or installation. `--framework PATH`/`--tools-dir PATH` allow read-only existing development inputs.

Bridge is universal: x86_64/Rosetta functionality and arm64 helper no-op. Native sharing is x86_64. Library load paths are loader-relative and ad-hoc signed; Apple libraries are not replaced.

Python checks use mocks/temp files for archive/root/process guards, profiles and dry-run behavior. Native offline tests use fake audio backends, without HAL/tap/TCC or a game. The native part is macOS-only.

| Check | Status |
|---|---|
| Packaged native/bridge/source-ready probe build on reference Mac | Passed |
| Original interactive scene/hands/buttons/audio | User-confirmed |
| Clean install from these scripts on another Mac | Not performed |
| All Quest/new Wine/new SteamVR/Vulkan | Unvalidated |
| Hardware VT/Wi-Fi/predicted-frame codec backend | Not implemented in playback |

Historical graphics/ABI, sharing, wrong-owner/handoff and GPU-visibility tests do not constitute a fresh end-to-end test of every packaged script. Generated DirectX-header warnings and duplicate libc++ linker notices remain; a build is not proof of ABI correctness. Meson is pinned to 1.12.1.

Our additions/docs are MIT; [third-party notices](../../THIRD_PARTY_NOTICES.md) retain UTM, MinGW, DXVK and ALVR terms. No upstream is claimed to have been authored/vibe-coded by us.

This is an unsupported AI-vibe-coded hobby. Report versions, one reproducible symptom, profile and a small redacted log. Do not upload account files, device IDs or raw captures. Preserve exact guards/ownership/GPU completion; distinguish encoded attempts, fresh video cadence and actual GPU time.

Audio-update checks: universal bridge and pinned watcher build passed; watcher SHA256 matches the original reviewed artifact. ARM64/x86_64 fake tests cover empty→game→idle, scene switch, borrowed buffers, poisoned tap, Stop/Reset failure, exact mapping and stale wire data. x86_64 scope tests cover early audio registration, late graphics activation and concurrency. These do not prove arbitrary-game switching in a headset.
