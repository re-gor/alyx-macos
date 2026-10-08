# Wine configuration

[Русский](../ru/wine.md) · [Install](install.md)

`configure.sh` backs up files and applies these values only to the managed prefix:

| Setting | Value / purpose |
|---|---|
| Retina | `HKCU\Software\Wine\Mac Driver\RetinaMode=Y` |
| DPI | `HKCU\Control Panel\Desktop\LogPixels=192`, 200% of96 DPI |
| Font smoothing | enabled; type2, gamma1400, orientation1, matching the observed reference |
| Synchronization | `WINEESYNC=1`, `WINEMSYNC=1`, also SDK plist flags |
| Graphics | D3DMetal on, DXVK off |
| Architecture/persona | 64-bit prefix, Wine win10, Windows10 Pro registry label; not a Windows OS installation/license |
| CRT | `msvcp140=native,builtin` only for `vrserver.exe` |
| OpenGL | `CX_FWD_COMPAT_GL_CTX=1` |
| Adapter | owned `DYLD_INSERT_LIBRARIES` graphics/audio bridge |

```sh
./scripts/stop.sh
./scripts/configure.sh
```

Retina/DPI/font settings affect Wine windows and text. They do **not** set the image resolution per eye; that is controlled separately in ALVR/Alyx. Reference font smoothing may originally have come from the template.

Automatic audio also requires `DMN_AUDIO_SOURCE_MODE=scene`, `DMN_AUDIO_TAP=1` and `WINESERVER` pointing to this exact engine. Configure writes these exports to both direct script launches and the wrapper SDK launch commands, and installs the pinned `C:\ALVR\AudioSceneWatcher.exe`. Capture remains limited to the selected game.
