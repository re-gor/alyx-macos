# Сборка, тесты и границы проверки

[English](../en/development.md) · [Архитектура](architecture.md)

База UTM: `f0dd1fafded509c9a949535c773fdc4e3ab07a71`. `config/source-snapshot.json` описывает снимок. Снимок звука от 8 октября включает выбор активной сцены, раннюю регистрацию loopback и опрос при пустой очереди. Пользователь подтвердил исправленный звук в исходной установке; установка с нуля на другом Mac не проверена.

```sh
./scripts/build_adapter.sh
python3 -m unittest discover -s tests -v
./scripts/test_offline.sh
```

Нужны wrapper/framework, Apple compiler/SDK и pinned Meson/Ninja. По умолчанию два job, записи только в `.build/`. Игра, GPU tests, захват и установка не стартуют. `--framework PATH`/`--tools-dir PATH` позволяют read-only использовать имеющиеся входы.

Bridge universal: x86_64/Rosetta functionality и arm64 helper no-op. Sharing library — x86_64. Пути loader-relative, подпись ad-hoc; Apple-библиотеки не заменяются.

Python использует mocks/temp files для archive/root/process guards, профилей и dry-run. Native offline tests — fake audio без HAL/tap/TCC или игры. Native-часть только для macOS.

| Проверка | Статус |
|---|---|
| Сборка native/bridge/source-ready probe пакета | Прошла на референсном Mac |
| Исходные сцены/руки/кнопки/звук | Подтверждены пользователем |
| Fresh install по скриптам на другом Mac | Не выполнялся |
| Все Quest/новые Wine/SteamVR/Vulkan | Не проверены |
| Hardware VT/Wi-Fi/predicted-frame codec backend | Не реализованы в playback |

Исторические graphics/ABI, sharing, wrong-owner/handoff и GPU visibility tests не являются новым end-to-end тестом всех скриптов. Остаются warnings generated DirectX headers/duplicate libc++; сборка не доказывает ABI-корректность. Meson — 1.12.1.

Наши дополнения/дока — MIT; [сторонние лицензии](../../THIRD_PARTY_NOTICES.md) сохранены. Авторство/vibe-coding upstream себе не приписываем.

Проект AI-навайбкоженный и фановый, без поддержки. В отчёте нужны версии, воспроизводимый симптом, профиль и короткий отредактированный лог. Не выкладывайте аккаунт, device ID или raw captures. Сохраняйте guards/ownership/GPU completion и различайте encoded attempts, fresh video cadence и GPU time.

Проверки обновления звука: universal bridge и закреплённый watcher собираются; SHA256 watcher совпадает с исходным проверенным артефактом. Fake-тесты ARM64/x86_64 покрывают пустой источник→игра→idle, смену сцены, выданные буферы, повреждённое состояние tap, ошибки Stop/Reset, mapping и устаревшие сообщения. x86_64 scope-тест проверяет раннюю регистрацию звука, позднее включение графики и конкурентность. Это не доказывает переключение произвольных игр в шлеме.
