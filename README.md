# VideoPlayer — native C++ portable відеоплеєр

Це незалежна паралельна реалізація VideoPlayer на C++17. Вона використовує
Win32 API та LibVLC C API, збирається у Visual Studio 2022 і не містить коду
WPF/.NET-версії.

Програма має звичайне Windows-вікно з власним дочірнім `HWND` для відео,
єдиною панеллю керування, перемотуванням, гучністю, mute, асинхронними
прев’ю на шкалі, зумом вибраної області та borderless fullscreen. LibVLC
завантажується динамічно лише з папки поруч із програмою — встановлений VLC,
`PATH` і реєстр не використовуються.

## Приватність і відсутність історії

**VideoPlayer не зберігає список відкритих файлів, позиції перегляду, прев’ю
кадрів або інші дані історії.** Поточний шлях, стан відтворення та обмежений
кеш прев’ю існують лише в оперативній пам’яті. Кеш очищається при відкритті
іншого файла й під час закриття програми; кадри або thumbnails ніколи не
записуються на диск.

Програма не створює MRU, власний Jump List, конфігурацію, базу даних, журнали,
телеметрію або файли в AppData/LocalAppData/ProgramData/Documents чи каталозі
програми. Діалог відкриття використовує `OFN_DONTADDTORECENT`, тому
VideoPlayer не додає вибраний файл до Windows Recent Items. Глобальна історія
Windows та дані інших програм не змінюються.

Коли одночасно відкривається кілька відео, шляхи наступних файлів передаються
новим процесам через анонімну спільну пам’ять і не додаються до їхнього
командного рядка.

Основний і preview LibVLC instance запускаються щонайменше з
`--ignore-config`, `--no-media-library` та `--no-video-title-show`; preview
додатково працює без звуку. Готовий `plugins/plugins.dat` є кешем модулів
пакетного LibVLC, а не історією переглядів.

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

Один або кілька файлів можна відкрити:

1. кнопкою «Відкрити» з одиночним або множинним вибором;
2. перетягуванням одного чи кількох файлів у вікно;
3. передаванням одного або кількох повних шляхів:

   ```text
   VideoPlayer.exe "D:\Відео\перший.mkv" "D:\Відео\другий.mp4"
   ```

Перший файл відкривається в поточному вікні, а кожен наступний — у новому
незалежному процесі VideoPlayer. Тому до восьми відео, вибраних за один раз,
можуть відтворюватися одночасно без спільної позиції, стану чи списку
відтворення. Службова спільна пам’ять передає шлях без дискових файлів.

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
| `Z` | почати вибір області для зуму |
| `Escape` | скасувати вибір, скинути зум або вийти з fullscreen |

Ті самі основні дії доступні стандартними кнопками та повзунками внизу
вікна, включно з кнопкою «Зум області».

### Прев’ю на шкалі

Під час наведення або перетягування повзунка перемотування одразу показуються
цільовий час і темне preview-вікно біля шкали. Кадр декодує окремий ліниво
створюваний preview-player у worker thread; основний player для цього не
перемотується. Запити об’єднуються до найновішого та обмежуються приблизно
одним на 120 мс. Ключ часу квантується до 750 мс, кеш містить не більше 24
незмінних кадрів лише в RAM, а максимальний кадр має 320×180 logical pixels
з DPI-масштабуванням popup. Виведення виконується GDI з 32-bit RV32 buffer без
GDI+, PNG/JPEG snapshot і тимчасових файлів. Якщо кадр недоступний, timestamp
залишається видимим із placeholder.

Тимчасовий timeout чи невдалий seek не блокують прев’ю для всього файла:
worker виконує обмежені повтори з backoff, а нове наведення дозволяє нову
спробу, зокрема на тому самому timestamp. Скасування інвалідує лише запит;
зум не змінює generation файла й не очищає справний RAM-cache. Новий
timestamp до отримання відповідного кадру показує placeholder. Заміна файла
швидко інвалідує запити; звільнення старого preview backend виконує worker,
без очікування його cleanup в UI. Стан завершеного через exception worker
явно відмічається, щоб запити не потрапляли у мертву чергу.

На один явний запит дозволено щонайбільше дві спроби, до 3 секунд
очікування кадру кожна, з 80-мс backoff. Це межа очікування кадру, а не
гарантія тривалості LibVLC stop/release. Для некешованого timestamp worker
створює новий preview-player/input з `start-time`, звільнивши попередній
player і його video output. Кожен player має окремий callback-контекст, який
живе до синхронного завершення release; пізній callback старого player не може
заповнити новий запит. LibVLC може спочатку показати спільний preroll-buffer,
тому перший token-matched display buffer не публікується: потрібен наступний
display callback, інакше лишається placeholder. Окремий `get_time` не
використовується як доказ часу показаних пікселів, бо clock може відставати від
display callback. LibVLC instance, один worker і RAM-cache залишаються;
основний player, його звук і crop не змінюються.

### Панель у fullscreen

У звичайному вікні панель керування завжди видима. У fullscreen відео займає
всю client area, а напівпрозора панель (alpha 160/255 непрозорості) накладається знизу.
У Windows High Contrast вона автоматично стає непрозорою. Лише під час стану
Playing панель ховається приблизно через 1200 мс бездіяльності; 100-мс timer
працює тільки у fullscreen. Рух у нижній activation zone висотою близько 100
logical pixels повертає панель протягом одного-двох tick. На паузі, під час відкриття,
помилки, завершення чи будь-якої взаємодії з seek, volume, preview або focus
панель залишається видимою. Виняток — активний вибір області зуму: панель
одразу ховається й не повертається від руху миші, паузи або фокуса, доки
вибір не завершено чи не скасовано. Курсор не ховається, global hook і
окремий thread для панелі не використовуються.

Коли fullscreen-панель прихована й тривалість відео відома, по самому нижньому
краю показується червона смуга прогресу заввишки 3 logical pixels. Її ширина
відповідає поточній позиції; вона не приймає фокус і не перехоплює мишу в
нижній activation zone. Після появи панелі або виходу з fullscreen смуга
одразу зникає.

Смуга є окремим owned non-activating popup поверх video output LibVLC,
без global topmost і без залежності від зуму. Slider, час і смуга беруть
узгоджений snapshot основного плеєра кожні 250 мс та після seek/restore.
Мінімізація, деактивація й модальний діалог ховають fullscreen popups,
скасовують незавершений drag/selection та preview. Втрата capture не
підтверджує seek. Restore узгоджує parent, enabled state і layout; застосований
crop зберігається, а preview з’являється лише після нового наведення.

### Зум області

Кнопка «Зум області» або `Z` вмикає неактивуючий selection overlay над
відео. Прямокутник можна тягнути в будь-якому напрямку; мінімальна область —
приблизно 24–32 logical pixels. Координати перетворюються в decoded source
coordinates з урахуванням letterbox/pillarbox, тому чорні поля не входять у
crop. Валідний crop застосовується через LibVLC без спотворення пропорцій.
Зум і fullscreen незалежні: вибір у звичайному вікні залишає його розмір і
положення незмінними; viewport не включає docked toolbar. Preview завжди
показує повний оригінальний кадр.

У fullscreen після натискання кнопки або `Z` нижня панель ховається до
завершення вибору. Overlay охоплює весь видимий кадр, включно з нижньою
частиною, яку перед цим перекривала панель. Після застосування або скасування
зуму панель повертається до звичайного режиму автоприховування.

`Escape` виконує рівно одну дію: Selecting → скасувати selection; Applied →
скинути crop, зберігши режим вікна; None + fullscreen → вийти з fullscreen;
None + windowed → нічого не закривати. `Z` також починає або скидає зум.
Crop зберігається в source coordinates при resize, minimize/restore, seek,
Pause/Play і F/F11 в обидва боки. Новий файл, критична помилка, явний reset та
закриття скидають crop. Кожен процес має незалежні crop, preview й overlay.

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
`VideoPlayer.exe --self-test`, ZIP та `dumpbin /dependents`. Вона також
перевіряє production source на API/рядки runtime persistence, а portable-
папку й ZIP — на log/tmp/history/recent/settings та дискові preview-кеші.
Імпорти `MSVCP140.dll` або `VCRUNTIME140.dll` вважаються помилкою.

GitHub Actions запускає той самий `build-all.ps1` на
`windows-2022` для push у `codex/native-cpp-portable` або вручну та
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
compatibility targets: реального запуску на них не виконано.

Регресійні тести викликають production-компоненти з fake backend, ін'єкцією
помилок і прихованими HWND. Окремо виконано інтеграційні запуски справжнього
LibVLC на згенерованих відео з таймкодом. Це не інтерактивна GUI-перевірка.
Desktop automation у цьому середовищі не запустилась через помилку sandbox
helper; фактично виконано **0 інтерактивних GUI-циклів**. Не перевірено
візуальний Z-order смуги над LibVLC без crop, читабельність alpha 160,
30 повних GUI-циклів, три видимі процеси поруч, інші монітори та різні DPI.
Приховані controller fixtures і `--self-test` не замінюють цих перевірок.
`--self-test` перевіряє runtime, ABI, версію LibVLC і створення двох MediaPlayer,
але не декодує відео.

Фактичні результати та відомі затримки backend наведені у
[`VALIDATION.md`](VALIDATION.md). Результат CI поточного commit доступний
у GitHub Actions після push.

## Відомі обмеження

- немає плейлиста;
- немає медіатеки;
- немає вибору аудіодоріжки;
- немає ручного підключення субтитрів;
- немає інсталятора;
- немає асоціації файлів;
- preview є best-effort: точність кадру залежить від seek/index конкретного
  контейнера й кодека;
- відповідність зуму перевірена для не оберненого відео з квадратними
  пікселями; non-square SAR/DAR та metadata rotation окремо не перевірялися;
- зум області доступний лише після того, як активна відеодоріжка віддала
  decoded video size.

Відомості про компоненти VLC/LibVLC та їхні ліцензії наведено у
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
