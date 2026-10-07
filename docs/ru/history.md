# Что сделали, пошагово

[English](../en/history.md) · [Архитектура](architecture.md)

Это история исходного эксперимента. Переносимые установочные скрипты появились позже и ещё не прошли полный fresh-machine VR acceptance test.

1. Создали Sikarugir/Wine 10.0_6, поставили Windows Steam, SteamVR и купленную Alyx. Использовали Rosetta и DX11→D3DMetal 3.0→Metal.
2. Установили ALVR 20.14.1 streamer/драйвер и matching APK на Quest; соединили native adb TCP-туннелями 9943/9944.
3. Адаптировали MIT `d3dmetal-native` из UTM к Wine-хосту графики: legacy 32-bit handles, Unix broker, импорт/экспорт shared buffers/textures. Compositor прошёл блокировку SteamVR 461.
4. Исправили shared-текстуры: shadow-heap для cube-array и отложенное обнуление, стиравшее пиксели другого процесса.
5. Добавили guarded fallback форматирования ALVR: после неудачного FormatMessage в wcslen не попадает NULL; HRESULT сохранён.
6. Включили `allowDisplayLockedMode=true`: неподдержанные WTS-запросы заставляли SteamVR считать сеанс заблокированным. Вместо бирюзы появилась сцена.
7. Добавили `CX_FWD_COMPAT_GL_CTX=1` для GL-контекста WebHelper, связанного с наблюдавшимся Mongoose timeout/сбоем UI.
8. Выключили невидимый Dashboard/autolaunch/Home/monitor, перехватывавший ввод. Native Microsoft `msvcp140` только для vrserver исправил наблюдавшийся mutex/condition-variable ABI. Появились руки, заработали курок и меню.
9. Исправили keyed-mutex ownership: чужое устройство могло освободить занятую поверхность. Проверяем PID/каноническую identity и публикуем release после GPU completion. Правый глаз синхронизировался в визуальной проверке.
10. Написали process-specific CoreAudio tap/aggregate и guarded Wine loopback. Выбирали PID новой игры, сохранили ALVR PCM transport. Пользователь подтвердил звук в Quest.
11. Host ring увеличили до 100мс; client выбрали 50мс/batch 10мс. Прерывания/роботный голос по обратной связи улучшились. Ёмкость не равна фиксированной задержке.
12. Зафиксировали render 1632² на глаз, auto-fidelity off/level 3 и проверенные MSAA 2/4. Битрейт улучшал детали; окно на Mac не являлось размером eye-render.
13. Включили широкий FFR, 10 потоков SW-кодека и FIFO 2. При Full выбрасывается новый кадр; это не latest-frame-wins всего пайплайна.
14. TCP 1400→16384: FPS около 45, расчётная медиана задержки 84→79мс, p95 95→83мс в двух окнах.
15. Pacing on/off/on на 90Гц/16КиБ: около 45,5→70,5→45,5 новых кадров/с, сопоставимая задержка, без queue-full. Выбрали off.
16. Проверили 72Гц с прежними 300Мбит/с: около 55/с против свежего 90Гц baseline 71,6/с; размер сжатого кадра вырос примерно 25%. Пользователь оставил 72Гц. `performance90` доступен отдельно.

Слабость GPU M4 как главную причину не доказали. Encoder-stage включает ожидание/readback/конверсию/кодек; game-stage не GPU timer. VideoToolbox, Wi-Fi, DXMT и новая queue/GPU-event архитектура остались исследованиями.

Остаются thumb-touch, общая NT-handle/fence поддержка, жизненный цикл аудиоисточника и чистая установка на других машинах. Это навайбкоженный фановый эксперимент, а не поддерживаемый универсальный Windows VR для Mac.
