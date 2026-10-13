[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Win32', 'x64')]
    [string]$Platform,

    [ValidateSet('Release')]
    [string]$Configuration = 'Release',

    [switch]$FolderOnly,

    [switch]$ArchiveOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$artifactRoot = Join-Path $repositoryRoot 'artifacts'
$packageArchitecture = if ($Platform -eq 'Win32') { 'win32' } else { 'x64' }
$packageName = "VideoPlayer-$packageArchitecture-portable"
$packageDirectory = Join-Path $artifactRoot $packageName
$zipPath = Join-Path $artifactRoot "$packageName.zip"
$buildDirectory = Join-Path $repositoryRoot "build\$Platform\$Configuration"
$application = Join-Path $buildDirectory 'VideoPlayer.exe'

if ($FolderOnly -and $ArchiveOnly) {
    throw 'FolderOnly and ArchiveOnly cannot be used together.'
}

if (-not $ArchiveOnly) {
    if (-not (Test-Path -LiteralPath $application -PathType Leaf)) {
        throw "Release executable not found: $application. Run scripts\build.ps1 first."
    }

    if (Test-Path -LiteralPath $packageDirectory) {
        Remove-Item -LiteralPath $packageDirectory -Recurse -Force
    }
    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }
    New-Item -ItemType Directory -Path $packageDirectory -Force | Out-Null

    Copy-Item -LiteralPath $application -Destination (Join-Path $packageDirectory 'VideoPlayer.exe')
    & (Join-Path $PSScriptRoot 'stage-runtime.ps1') -Platform $Platform -Destination $packageDirectory

    $notices = Join-Path $repositoryRoot 'THIRD_PARTY_NOTICES.md'
    if (-not (Test-Path -LiteralPath $notices -PathType Leaf)) {
        throw "Third-party notices are missing: $notices"
    }
    Copy-Item -LiteralPath $notices -Destination (Join-Path $packageDirectory 'THIRD_PARTY_NOTICES.md')

    $libVlcLicense = Join-Path $repositoryRoot 'licenses\COPYING.LIB'
    if (-not (Test-Path -LiteralPath $libVlcLicense -PathType Leaf)) {
        throw "LibVLC LGPL license text is missing: $libVlcLicense"
    }
    Copy-Item -LiteralPath $libVlcLicense -Destination (Join-Path $packageDirectory 'COPYING.LIB')

    $portableReadme = @"
VideoPlayer — portable native C++ відеоплеєр ($packageArchitecture)

1. Повністю розпакуйте ZIP в окрему папку.
2. Копіюйте всю папку: VideoPlayer.exe, DLL, plugins і tools мають бути разом.
3. Запустіть VideoPlayer.exe. Інсталятор, .NET, встановлений VLC,
   кодек-паки та Visual C++ Redistributable не потрібні.

Окреме перенесення лише VideoPlayer.exe не підтримується.
Один чи кілька файлів можна відкрити кнопкою «Відкрити», перетягуванням
у вікно або передати шляхами в командному рядку. Перший файл відкривається
в поточному вікні, кожен наступний — в окремому інстансі VideoPlayer.
За один раз можна відкрити до восьми відео.

ПРИВАТНІСТЬ
VideoPlayer не зберігає список відкритих файлів, позиції перегляду, прев’ю
кадрів або інші дані історії. Шлях поточного файла та обмежений кеш прев’ю
існують лише в RAM і очищаються при зміні файла або закритті програми.
Програма не створює MRU/Jump List, налаштування, базу даних, log-файли,
телеметрію чи thumbnails на диску та не додає вибрані файли до Windows
Recent Items. Основний і preview LibVLC запускаються з --ignore-config,
--no-media-library та --no-video-title-show.
Шляхи додаткових відео передаються новим процесам через анонімну спільну
пам’ять і не додаються до їхнього командного рядка.

МОЖЛИВОСТІ
- «Зберегти відео…» або Ctrl+E відкриває exporter: усе відео чи A–B,
  повний кадр чи поточна видима область зуму, source canvas чи native crop,
  з аудіо або без нього. FFprobe показує параметри й план до запуску.
- Повний кадр без зуму, включно з A–B, зберігається лише копіюванням відео
  й аудіо без перекодування. Для A–B межі розширюються до ключових кадрів;
  запитаний і фактичний інтервали показано до підтвердження.
  Перекодування використовується лише при збереженні картинки зуму.
- Позначте A/B і натисніть «Додати A–B до списку» для кожного відрізка.
  Поточний A–B видно синім ще до збереження, A з'являється окремою міткою.
  Тягніть прапорці A/B над шкалою для редагування поточного чи збереженого
  відрізка. Перетини блокуються; Escape скасовує drag.
  Виберіть відрізок у списку й натисніть «Прибрати зі списку», або натисніть
  правою кнопкою на помаранчевий відрізок. Саме відео від цього не змінюється.
  «Видалити відрізки…» видаляє їх за один раз без перекодування, дає
  прибрати окремі відрізки й пропонує новий файл або перезапис відкритого.
  Фактичні межі округлюються до ключових кадрів; перекриття об'єднуються.
  Оригінал замінюється лише після перевірки результату.
- Для видимого зуму за замовчуванням обирається фізичний native crop:
  encoded frame зменшується до crop, без чорних полів, а video bitrate
  масштабується за площею. Source canvas лишається окремою опцією.
- Exporter використовує лише tools\export\ffmpeg.exe і ffprobe.exe з цієї
  папки, не PATH. Progress асинхронний; Cancel прибирає partial. Перезапис
  підтверджується явно; звичайний exporter не замінює source/hard-link.
- Після підтвердження фонового export плеєр знову інтерактивний: можна
  продовжувати перегляд, preview або відкрити інший файл. Job використовує
  незмінний snapshot source identity/A-B/crop. Закриття плеєра пропонує
  скасувати job і чекає cleanup власного FFmpeg-процесу.
- Hover або drag шкали показує timestamp і асинхронне preview до 320x180;
  окремий preview-player не перемотує основне відео, а кеш до 128 кадрів
  (приблизно до 28 МіБ пікселів) залишається лише в RAM.
- Після відкриття відео прев’ю готуються автоматично у фоні: до 121 кадру
  на всю тривалість. Спочатку початок, кінець і середина, потім решта.
  Крок не дрібніше 5 секунд і зростає для довгих відео. Сусідні позиції
  використовують один кадр, швидкий рух миші об’єднує запити після 100 мс
  стабілізації, а готовий кадр із кешу з’являється одразу. Точність перемотки
  не змінюється; під кадром також вказано його приблизний час.
- Поки готується нове прев’ю, попередній/найближчий кадр лишається видимим
  із власним часом і позначкою оновлення. Один worker готує сусідні кадри
  та заповнює кеш усього відео; кадр під курсором має пріоритет.
  Приховування прев’ю не зупиняє підготовку. Початкове очікування можливе,
  поки кеш заповнюється; зміна відео або закриття очищає кеш.
- У fullscreen панель із alpha 160/255 накладається на відео, ховається після
  приблизно 1200 мс лише під час Playing і повертається в нижній 100-pixel
  зоні. Коли панель прихована, по нижньому краю видно тонку червону смугу
  поточного прогресу зі зумом і без нього. Minimize ховає popups, restore
  відновлює controls без скидання crop.
- Кнопка «Сховати панель керування» або Ctrl+H вмикає ручний compact mode
  також у звичайному
  вікні: відео займає всю client area, а тонка смуга прогресу лишається.
  Наведення у нижні 48 logical pixels показує напівпрозору стрілку; клік по
  ній або Ctrl+H відновлює панель. Режим зберігається через F11 і minimize.
- Кнопка «Зум області (Z)» або клавіша Z дозволяє виділити crop. На час
  fullscreen-вибору панель ховається, тому доступна вся область кадру.
  Зум працює у звичайному вікні й не змінює його розмір або режим.
  Crop зберігається при resize, minimize/restore та F/F11 в обидва боки.
  Escape спочатку скасовує selection/zoom, а наступне натискання виходить
  із fullscreen.
- Наведіть мишу на край/кут картинки й тягніть лівою кнопкою, щоб
  змінити область зуму: бічний край змінює ширину, верхній/нижній —
  висоту, кут — обидві незалежно. Пропорції області можна змінювати,
  але зображення всередині не спотворюється. Зум вмикається автоматично.
  Працює також для вже вибраного зуму, у вікні та fullscreen.
  F11 і compact зберігають зум; колесо враховує нову форму області.
- В активному зумі стрілки рухають область, затиснута ліва кнопка всередині дозволяє
  перетягувати зображення, а колесо над відео наближає/віддаляє відносно
  курсора. Віддалення розширює область зі збереженням її останніх пропорцій
  до меж відео; колесо не скидає режим зуму. Ctrl+Left/Right лишаються для
  перемотування; Z або Escape скидають зум і розмір відображення.
- Ctrl+O відкриває файл; Space — play/pause; M — mute; Ctrl+H — compact mode;
  F або F11 — fullscreen; стрілки перемотують або змінюють гучність.
- «Повтор відео» або L вмикає цикл усього файла.
- Для фрагмента перейдіть на початок і натисніть «Початок A» або [,
  потім на кінець і натисніть «Кінець B» або ]. B має бути мінімум на
  0,5 секунди пізніше A. Повтор A–B вмикається автоматично; якщо відео
  на паузі, натисніть Play. Межі видно на шкалі та поруч із кнопками.
- «Повтор A–B» вмикає/вимикає готовий фрагмент; «Скинути A–B» очищає його.
  Нове A вимикає повтор для вільного вибору B. Повтор відео й A–B взаємовиключні.
  Новий файл скидає повтор, Pause/Stop не запускають його самостійно.
  Межі існують лише в RAM; перехід через seek/restart не є безшовним.

Компоненти VLC/LibVLC 3.0.23, FFmpeg/ffprobe та їхні ліцензійні тексти
постачаються в цій папці. Додаткові відомості: THIRD_PARTY_NOTICES.md.
"@
    Set-Content -LiteralPath (Join-Path $packageDirectory 'README-portable.txt') -Value $portableReadme -Encoding UTF8

    Write-Host "Portable folder: $packageDirectory"
    if ($FolderOnly) {
        return
    }
}
elseif (-not (Test-Path -LiteralPath $packageDirectory -PathType Container)) {
    throw "Portable folder not found for ArchiveOnly: $packageDirectory"
}

# The package directory is created from an allowlist: application, runtime DLLs,
# official runtime directories, notices and the portable readme. No PDB or SDK is copied.
if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}
Compress-Archive -Path (Join-Path $packageDirectory '*') -DestinationPath $zipPath -CompressionLevel Optimal

$zip = Get-Item -LiteralPath $zipPath
if ($zip.Length -le 0) {
    throw "Portable ZIP is empty: $zipPath"
}

Write-Host "Portable folder: $packageDirectory"
Write-Host "Portable ZIP:    $zipPath ($($zip.Length) bytes)"
