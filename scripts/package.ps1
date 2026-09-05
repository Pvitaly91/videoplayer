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
2. Залиште VideoPlayer.exe, DLL і каталог plugins разом.
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
- Hover або drag шкали показує timestamp і асинхронне preview до 320x180;
  окремий preview-player не перемотує основне відео, а 24-кадровий кеш
  залишається лише в RAM.
- У fullscreen панель із alpha 160/255 накладається на відео, ховається після
  приблизно 1200 мс лише під час Playing і повертається в нижній 100-pixel
  зоні. Коли панель прихована, по нижньому краю видно тонку червону смугу
  поточного прогресу зі зумом і без нього. Minimize ховає popups, restore
  відновлює controls без скидання crop.
- Кнопка «Зум області (Z)» або клавіша Z дозволяє виділити crop. На час
  fullscreen-вибору панель ховається, тому доступна вся область кадру.
  Зум працює у звичайному вікні й не змінює його розмір або режим.
  Crop зберігається при resize, minimize/restore та F/F11 в обидва боки.
  Escape спочатку скасовує selection/zoom, а наступне натискання виходить
  із fullscreen.
- Ctrl+O відкриває файл; Space — play/pause; M — mute; F або F11 —
  fullscreen; стрілки перемотують або змінюють гучність.

Компоненти VLC/LibVLC 3.0.23 та їхні ліцензійні тексти постачаються
в цій папці. Додаткові відомості: THIRD_PARTY_NOTICES.md.
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
