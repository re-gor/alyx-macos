# Установка отдельной оболочки

[English](../en/install.md) · [README](../../README.ru.md)

Создаётся новая установка без чужого аккаунта Steam. Полный fresh install со шлемом на втором Mac ещё не проверен.

## Что нужно

- Apple Silicon, macOS 15+, Python 3.9+, Apple compiler/SDK и Rosetta 2.
- Свой Steam с купленной Alyx; место для Steam, игры и архивов.
- Quest, USB-кабель с передачей данных, Developer Mode и USB debugging. Проверяли Quest 3.
- Интернет для официальных загрузок и pinned Meson/Ninja в `.venv` проекта.

При необходимости установите Apple-инструменты:

```sh
xcode-select --install
softwareupdate --install-rosetta
```

Лицензии и запросы подтвердите сами. Скрипты не принимают лицензии автоматически и не отключают Gatekeeper.

## 1. Скачать проект и посмотреть план

```sh
git clone https://github.com/re-gor/alyx-macos.git
cd alyx-macos
./scripts/doctor.sh
./scripts/install_wine.sh --dry-run
```

По умолчанию: `~/wine/alyx-macos`. Для другого пустого каталога добавляйте `--root "$HOME/Games/alyx-macos"` к **каждой** команде. Существующая посторонняя оболочка отклоняется.

## 2. Wine и Windows Steam

```sh
./scripts/install_wine.sh
./scripts/install_steam.sh
./scripts/start_steam.sh
```

Проверяются SHA256 WS12WineSikarugir10.0_6 и Template 1.0.11. D3DMetal находится в официальном шаблоне, а не в этом репозитории. Завершите Steam GUI по стандартному пути, снимите **Run Steam** на последнем экране, затем откройте `start_steam.sh` и войдите. Если окно установщика осталось после возврата SDK, закончите его перед следующим шагом.

## 3. SteamVR, Alyx и Microsoft runtime

```sh
./scripts/install_steamvr.sh
./scripts/install_alyx.sh
./scripts/install_vc_runtime.sh
```

Пройдите окна Steam и дождитесь загрузок. SteamVR — app 250820, Alyx — 546560. Референс: SteamVR 2.17.10/Alyx build 25487405. Новые версии не проверены; недокументированные depot-загрузки не выполняются.

Microsoft x64 v14 ставится официальным интерактивным installer с вашим принятием лицензии. Native `msvcp140` нужен только `vrserver.exe`: исходный Wine builtin давал проблему синхронизации/ABI. DLL в проекте не распространяются.

## 4. ALVR и адаптеры

```sh
./scripts/stop.sh
./scripts/install_alvr.sh
./scripts/build_adapter.sh
./scripts/configure.sh
./scripts/register_alvr.sh
./scripts/stop.sh
./scripts/add_alvr_to_steam.sh
./scripts/make_gui_launcher.sh
```

Build только компилирует. Configure создаёт бэкапы, ставит адаптеры в эту оболочку и задаёт профиль/native CRT override. Register добавляет `C:\ALVR` в SteamVR данного prefix. ALVR — отдельный streamer/драйвер, не игра из магазина Steam.

## 5. Клиент на шлеме

Подключите data-кабель, наденьте Quest и разрешите **Allow USB debugging** компьютеру. При существующем adb:

```sh
./scripts/install_headset.sh --adb "$HOME/Library/Android/sdk/platform-tools/adb"
```

Другой вариант, после прочтения [Google SDK terms](https://developer.android.com/studio/terms):

```sh
./scripts/install_headset.sh --download-adb --accept-platform-tools-license
```

Если устройств несколько, добавьте `--serial YOUR_SERIAL`. Установка может прервать другой ALVR-сеанс на этом шлеме; выполняйте её вне такого сеанса. Чужой adb server не завершается.

Откройте ALVR в Quest, прочитайте **Hostname**, например `1234.client`, и подставьте его:

```sh
./scripts/pair_headset.sh --hostname 1234.client
```

Значение остаётся в приватных настройках. Туннели создаёт Mac adb; Windows adb wired mode обходится.

## 6. Запуск и проверка

```sh
./scripts/doctor.sh
./scripts/launch_alyx.sh
```

Quest должен быть надет, ALVR — открыт. Запрос macOS на захват подтвердите сами. Проверьте **оба глаза, руки, курок/меню и слышимый звук**. Одного `Connected` недостаточно. При необходимости загрузите save в игре. Дальше: [запуск](usage.md) и [FAQ](faq.md).

Retina/DPI/шрифты: [Wine](wine.md). Сторонняя игра Steam и Finder-ярлык: [запуск](usage.md). Проблема GUI-старта звука ещё исследуется.
