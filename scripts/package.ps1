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
Файл можна відкрити кнопкою «Відкрити», перетягуванням у вікно або
передати повним шляхом як перший аргумент командного рядка.

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
