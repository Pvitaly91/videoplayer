# VideoPlayer — native C++ portable відеоплеєр

Це незалежна паралельна реалізація VideoPlayer на C++17. Вона використовує
Win32 API та LibVLC C API, збирається у Visual Studio 2022 і не містить коду
WPF/.NET-версії.

Програма має звичайне Windows-вікно з власним дочірнім `HWND` для відео,
панеллю керування, перемотуванням, гучністю, mute та borderless fullscreen.
LibVLC завантажується динамічно лише з папки поруч із програмою — встановлений
VLC, `PATH` і реєстр не використовуються.

## Portable-запуск

Готові пакети мають дві архітектури:

- `VideoPlayer-win32-portable.zip` — x86/Win32;
- `VideoPlayer-x64-portable.zip` — x64.

Повністю розпакуйте потрібний ZIP і запустіть `VideoPlayer.exe`. Зберігайте
всю папку разом: `VideoPlayer.exe`, `libvlc.dll`, `libvlccore.dll`, інші
DLL, `plugins` та runtime-каталоги. Перенесення лише
`VideoPlayer.exe` не підтримується.

Для запуску не потрібні:

- .NET;
- Qt;
- встановлений VLC або FFmpeg;
- кодек-паки;
- Visual C++ Redistributable;
- інсталятор чи права адміністратора.

Release збирається зі статичним MSVC runtime (`/MT`), Debug — з `/MTd`.
VLC не запускається як зовнішній процес.

## Відкриття відео

Файл можна відкрити:

1. кнопкою «Відкрити»;
2. перетягуванням у вікно;
3. передаванням повного шляху першим аргументом:

   ```text
   VideoPlayer.exe "D:\Відео з пробілами\фільм.mkv"
   ```

Діалог показує MP4, AVI, MKV, MOV, M4V, WEBM, WMV, MPG, MPEG, TS, M2TS,
MTS, FLV, 3GP, OGV і VOB, а також усі файли. Розширення не є жорстким
обмеженням: фактичну підтримку контейнера та кодека визначають модулі LibVLC.
Unicode-шляхи, пробіли, абсолютні та UNC-шляхи передаються без тимчасового
копіювання файла. Для extended-length шляхів програма відкриває файл через
Unicode Win32 API та надає LibVLC потокові read/seek callbacks, тому не
залежить від обмеження `MAX_PATH` у файловому шарі LibVLC 3.

## Керування

| Клавіша | Дія |
| --- | --- |
| `Ctrl+O` | відкрити файл |
| `Space` | відтворити / пауза |
| `Left`, `Right` | назад / вперед на 5 секунд |
| `Ctrl+Left`, `Ctrl+Right` | назад / вперед на 30 секунд |
| `Up`, `Down` | гучність +5 / −5 |
| `M` | звук / без звуку |
| `F`, `F11` | увімкнути або вимкнути fullscreen |
| `Escape` | вийти з fullscreen |

Ті самі основні дії доступні стандартними кнопками та повзунками внизу
вікна.

## Збірка

Потрібні Visual Studio 2022 (workload **Desktop development with C++**),
Platform Toolset v143 і Windows SDK. NuGet, vcpkg, Conan і CMake не потрібні.

Скрипт завантаження бере офіційні архіви VLC/LibVLC 3.0.23 з
`download.videolan.org`, завантажує офіційний файл SHA-256, перевіряє
контрольну суму, розпаковує runtime окремо для Win32 і x64 та один раз створює
`plugins/plugins.dat` офіційною утилітою `vlc-cache-gen.exe`. До portable-
пакета потрапляє готовий кеш, але не утиліта його генерації:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/fetch-libvlc.ps1 -Platform Win32
powershell -ExecutionPolicy Bypass -File scripts/fetch-libvlc.ps1 -Platform x64
```

Архіви та runtime зберігаються у
`third_party/libvlc/3.0.23/{win32,win64}` та не комітяться. Після першого
завантаження post-build крок Visual Studio інкрементально додає runtime до
папки результату, тому F5 не потребує ручного копіювання сотень мегабайт.

Команди окремих збірок:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build.ps1 -Platform Win32 -Configuration Debug
powershell -ExecutionPolicy Bypass -File scripts/build.ps1 -Platform Win32 -Configuration Release
powershell -ExecutionPolicy Bypass -File scripts/build.ps1 -Platform x64 -Configuration Debug
powershell -ExecutionPolicy Bypass -File scripts/build.ps1 -Platform x64 -Configuration Release
```

`build.ps1` знаходить Visual Studio через `vswhere`, викликає MSBuild,
збирає обидва native-проєкти та запускає `VideoPlayer.Tests.exe`.
Конфігурації solution: Debug/Release × Win32/x64.

## Пакування та перевірка

Після відповідної Release-збірки можна створити один пакет:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/package.ps1 -Platform Win32
powershell -ExecutionPolicy Bypass -File scripts/package.ps1 -Platform x64
```

Повний локальний цикл очищає `artifacts`, збирає чотири конфігурації,
запускає native-тести, створює обидва пакети, запускає headless self-test і
перевіряє вміст та залежності:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build-all.ps1
```

Результати:

```text
artifacts/VideoPlayer-win32-portable/
artifacts/VideoPlayer-win32-portable.zip
artifacts/VideoPlayer-x64-portable/
artifacts/VideoPlayer-x64-portable.zip
```

Окрему перевірку можна повторити командами:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/verify-package.ps1 -Platform Win32
powershell -ExecutionPolicy Bypass -File scripts/verify-package.ps1 -Platform x64
```

Перевірка контролює PE-архітектуру EXE та всіх DLL, наявність непорожніх
`plugins` і `plugins/plugins.dat`, заборонені SDK/PDB/.NET-файли, результат
`VideoPlayer.exe --self-test`, ZIP та `dumpbin /dependents`. Імпорти
`MSVCP140.dll` або `VCRUNTIME140.dll` вважаються помилкою.

GitHub Actions запускає той самий `build-all.ps1` на
`windows-latest` для push у `codex/native-cpp-portable` або вручну та
публікує два artifacts: `VideoPlayer-win32-portable` і
`VideoPlayer-x64-portable`.

## Сумісність Windows

Проєкти задають `WINVER=0x0601`, `_WIN32_WINNT=0x0601` і мінімальну
версію subsystem 6.01. Код не вимагає AVX/AVX2/AVX-512, WinRT, Windows App
SDK або обов’язкових Windows 10/11 API.

Налаштовані compatibility targets:

- Windows 7 SP1;
- Windows 8.1;
- Windows 10;
- Windows 11.

Локально збірки, native tests, `--self-test` і перевірку portable-пакетів
фактично виконано на Microsoft Windows 11 Pro x64, версія 10.0, build 26200.
Windows 7 SP1, Windows 8.1 і Windows 10 залишаються лише налаштованими
compatibility targets: реального запуску на них не виконано. Ручне відтворення
репрезентативного набору медіафайлів і перевірку заявлених форматів у цьому
циклі також не виконано; `--self-test` перевіряє runtime, ABI, версію LibVLC та
створення MediaPlayer, але не декодування відео. Результат CI для поточного
commit наведено в GitHub Actions після push.

## Відомі обмеження першої версії

- немає плейлиста;
- немає медіатеки;
- немає вибору аудіодоріжки;
- немає ручного підключення субтитрів;
- немає історії;
- немає інсталятора;
- немає асоціації файлів;
- панель керування може залишатися видимою у fullscreen.

Відомості про компоненти VLC/LibVLC та їхні ліцензії наведено у
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
