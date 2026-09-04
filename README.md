# VideoPlayer

VideoPlayer — легкий настільний відеоплеєр для Windows 10/11 x64. Він відтворює локальні файли без запуску зовнішнього плеєра та без окремого встановлення VLC, FFmpeg, наборів кодеків або .NET Runtime у portable-збірці.

## Можливості

- швидкий старт відтворення локального файла через вбудований LibVLC;
- відкриття файла кнопкою, перетягуванням у вікно або аргументом командного рядка;
- автоматичний початок відтворення після відкриття;
- відтворення/пауза, зупинка, перемотування та індикація часу;
- гучність 0–100 %, вимкнення і відновлення звуку;
- справжній borderless fullscreen на поточному моніторі;
- темний україномовний інтерфейс;
- self-contained portable-збірка для `win-x64`.

Діалог відкриття пропонує MP4, AVI, MKV, MOV, M4V, WEBM, WMV, MPG/MPEG, TS/M2TS/MTS, FLV, 3GP, OGV і VOB, а також усі файли. Розширення контейнера саме по собі не гарантує відтворення: результат також залежить від аудіо- й відеокодеків усередині файла. Остаточну підтримку визначає LibVLC, тому файл із незнайомим розширенням теж можна відкрити через варіант «Усі файли».

## Технології

- C# і WPF;
- .NET 10, `net10.0-windows`;
- [LibVLCSharp.WPF 3.10.1](https://www.nuget.org/packages/LibVLCSharp.WPF/3.10.1);
- [VideoLAN.LibVLC.Windows 3.0.23.1](https://www.nuget.org/packages/VideoLAN.LibVLC.Windows/3.0.23.1);
- x64 SDK-style проєкти та unit-тести.

NuGet-пакет `VideoLAN.LibVLC.Windows` постачає нативний LibVLC та його плагіни разом із програмою. Звичайний VLC Player встановлювати не потрібно, і застосунок не запускає `vlc.exe`, `ffplay.exe`, `ffmpeg.exe` чи `mpv.exe` як окремий процес.

## Відкриття відео

Файл можна відкрити трьома способами:

1. Натиснути «Відкрити файл» або `Ctrl+O`.
2. Перетягнути один локальний файл у вікно.
3. Передати повний шлях першим аргументом командного рядка:

   ```powershell
   .\VideoPlayer.exe "D:\Відео з подорожі\Київ 2026.mkv"
   ```

Підтримуються шляхи з пробілами та Unicode-символами.

## Гарячі клавіші

| Клавіші | Дія |
| --- | --- |
| `Ctrl+O` | Відкрити файл |
| `Space` | Відтворити або поставити на паузу |
| `Left` / `Right` | Назад / вперед на 5 секунд |
| `Ctrl+Left` / `Ctrl+Right` | Назад / вперед на 30 секунд |
| `Up` / `Down` | Збільшити / зменшити гучність на 5 % |
| `M` | Вимкнути або увімкнути звук |
| `F` або `F11` | Увійти чи вийти з повноекранного режиму |
| `Escape` | Вийти з повноекранного режиму |

## Розробка та перевірка

Для локальної розробки потрібні Windows x64 і .NET 10 SDK. Усі команди запускаються з кореня репозиторію.

Відновлення залежностей:

```powershell
dotnet restore VideoPlayer.sln
```

Release-збірка:

```powershell
dotnet build VideoPlayer.sln -c Release --no-restore
```

Unit-тести:

```powershell
dotnet test tests/VideoPlayer.Tests/VideoPlayer.Tests.csproj -c Release --no-build --no-restore -p:Platform=x64
```

Запуск із вихідного коду:

```powershell
dotnet run --project src/VideoPlayer/VideoPlayer.csproj -- "D:\Відео\приклад.mp4"
```

Шлях до файла можна не передавати й відкрити його вже у вікні програми.

## Portable publish

Рекомендований спосіб створити готову збірку:

```powershell
pwsh -File .\scripts\publish-win-x64.ps1
```

Скрипт очищає попередній `artifacts/win-x64`, виконує restore, Release build, тести та self-contained publish, а потім перевіряє наявність `VideoPlayer.exe`, .NET Runtime, LibVLCSharp, `libvlc.dll`, `libvlccore.dll`, каталогу плагінів і `THIRD_PARTY_NOTICES.md`.

Еквівалентна команда лише для publish:

```powershell
dotnet publish src/VideoPlayer/VideoPlayer.csproj `
  -c Release `
  -r win-x64 `
  --self-contained true `
  -p:PublishReadyToRun=true `
  -p:PublishSingleFile=false `
  -p:PublishTrimmed=false `
  -o artifacts/win-x64
```

Готова portable-збірка розташована в `artifacts/win-x64`. Скопіюйте всю папку без вилучення вкладених каталогів. Великі нативні DLL і каталог `plugins` є частиною LibVLC та потрібні для декодування; це не дублікати звичайного VLC Player. Self-contained publish також містить потрібні файли .NET Runtime, тому на цільовому комп'ютері не треба встановлювати .NET.

GitHub Actions на Windows відновлює, збирає, тестує й публікує застосунок та завантажує папку як artifact `VideoPlayer-win-x64`. CI не запускає графічне WPF-вікно.

## Відомі обмеження першої версії

- немає плейлиста;
- немає вибору аудіо-, відео- та субтитрових доріжок;
- немає ручного підключення зовнішніх субтитрів;
- немає інсталятора та інтеграції з асоціаціями файлів Windows.

Відомості про сторонні компоненти та їхні ліцензії наведено в [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Цей репозиторій навмисно не отримує основну ліцензію від імені його власника.
