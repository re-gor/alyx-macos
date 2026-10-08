# Настройки самого Wine

[English](../en/wine.md) · [Установка](install.md)

`configure.sh` делает бэкап и задаёт значения только в своей бутылке:

| Параметр | Значение / назначение |
|---|---|
| Retina | `HKCU\Software\Wine\Mac Driver\RetinaMode=Y` |
| DPI | `HKCU\Control Panel\Desktop\LogPixels=192`, 200% от96 DPI |
| Сглаживание шрифтов | включено; type2, gamma1400, orientation1 как в референсе |
| Синхронизация | `WINEESYNC=1`, `WINEMSYNC=1` и флаги SDK plist |
| Графика | D3DMetal включён, DXVK выключен |
| Архитектура/персона | 64-bit prefix, Wine win10, registry label Windows10 Pro; это не установка/лицензия Windows OS |
| CRT | `msvcp140=native,builtin` только для `vrserver.exe` |
| OpenGL | `CX_FWD_COMPAT_GL_CTX=1` |
| Адаптер | свой graphics/audio bridge через `DYLD_INSERT_LIBRARIES` |

```sh
./scripts/stop.sh
./scripts/configure.sh
```

Retina/DPI/шрифты относятся к окнам и тексту Wine. Разрешение на каждый глаз задаётся отдельно в ALVR/Alyx. Исходное сглаживание могло прийти из шаблона.

Для автоматического звука нужны также `DMN_AUDIO_SOURCE_MODE=scene`, `DMN_AUDIO_TAP=1` и `WINESERVER` именно этой сборки. Configure задаёт их для скриптов и запуска через SDK оболочки, устанавливает закреплённый `C:\ALVR\AudioSceneWatcher.exe`. Захват ограничен выбранной игрой.
