[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Win32', 'x64')]
    [string]$Platform
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$downloadRoot = Join-Path $repositoryRoot 'third_party\ffmpeg\_downloads'
if ($Platform -eq 'Win32') {
    $version = '6.0'
    $architecture = 'win32'
    $archiveName = 'ffmpeg-n6.0-latest-win32-gpl-6.0.zip'
    $archiveRoot = 'ffmpeg-n6.0-latest-win32-gpl-6.0'
    $archiveUrl = 'https://github.com/sudo-nautilus/FFmpeg-Builds-Win32/releases/download/latest/ffmpeg-n6.0-latest-win32-gpl-6.0.zip'
    $expectedSha256 = 'CABDE0DF130C32D097099F0F45773A695E2A8F44C6C88E0313757E9B8EB5320B'
    $expectedSize = 95961101
    $assetId = 110307584
    $providerCommit = 'f7d241136493035122299d3e91e06a7ca8168ddc'
    $provider = 'sudo-nautilus/FFmpeg-Builds-Win32 GPL static Win32'
    $sourceUrl = "https://github.com/sudo-nautilus/FFmpeg-Builds-Win32/tree/$providerCommit"
} else {
    $version = '6.1.1'
    $architecture = 'win64'
    $archiveName = 'ffmpeg-6.1.1-essentials_build.zip'
    $archiveRoot = 'ffmpeg-6.1.1-essentials_build'
    $archiveUrl = 'https://github.com/GyanD/codexffmpeg/releases/download/6.1.1/ffmpeg-6.1.1-essentials_build.zip'
    $expectedSha256 = '742E32FC9F92681F9F254B925E1B613FDD8074BA40749D4879AEFDB009B94CC5'
    $expectedSize = 87520045
    $assetId = 143137017
    $providerCommit = '6473e72373c86cbf4173cda6b2f44827ed5cce2e'
    $provider = 'Gyan.dev FFmpeg 6.1.1 essentials GPL static x64'
    $sourceUrl = "https://github.com/GyanD/codexffmpeg/tree/$providerCommit"
}

$archive = Join-Path $downloadRoot $archiveName
$runtimeRoot = Join-Path $repositoryRoot "third_party\ffmpeg\$version\$architecture"
$ffmpeg = Join-Path $runtimeRoot 'ffmpeg.exe'
$ffprobe = Join-Path $runtimeRoot 'ffprobe.exe'

New-Item -ItemType Directory -Path $downloadRoot -Force | Out-Null
if (-not (Test-Path -LiteralPath $archive -PathType Leaf)) {
    $partial = "$archive.download"
    try {
        Invoke-WebRequest -UseBasicParsing -Uri $archiveUrl -OutFile $partial
        Move-Item -LiteralPath $partial -Destination $archive -Force
    } finally {
        if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Force }
    }
} else {
    Write-Host "Using cached FFmpeg archive: $archive"
}

$actualSha256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToUpperInvariant()
if ((Get-Item -LiteralPath $archive).Length -ne $expectedSize -or
    $actualSha256 -ne $expectedSha256) {
    throw "FFmpeg archive SHA-256 mismatch for $Platform. Expected $expectedSha256, found $actualSha256."
}
Write-Host "FFmpeg SHA-256 verified: $actualSha256"

if (-not (Test-Path -LiteralPath $ffmpeg -PathType Leaf) -or
    -not (Test-Path -LiteralPath $ffprobe -PathType Leaf)) {
    $extractRoot = Join-Path $runtimeRoot '_extracting'
    if (Test-Path -LiteralPath $extractRoot) {
        Remove-Item -LiteralPath $extractRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Path $extractRoot -Force | Out-Null
    Expand-Archive -LiteralPath $archive -DestinationPath $extractRoot -Force
    $expanded = Join-Path $extractRoot $archiveRoot
    $bin = Join-Path $expanded 'bin'
    $license = @('LICENSE', 'LICENSE.txt') |
        ForEach-Object { Join-Path $expanded $_ } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
    if (-not (Test-Path -LiteralPath (Join-Path $bin 'ffmpeg.exe') -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $bin 'ffprobe.exe') -PathType Leaf) -or
        [string]::IsNullOrWhiteSpace([string]$license)) {
        throw "Pinned FFmpeg archive has an unexpected layout: $archive"
    }
    New-Item -ItemType Directory -Path $runtimeRoot -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $bin 'ffmpeg.exe') -Destination $ffmpeg -Force
    Copy-Item -LiteralPath (Join-Path $bin 'ffprobe.exe') -Destination $ffprobe -Force
    Copy-Item -LiteralPath $license -Destination (Join-Path $runtimeRoot 'LICENSE.GPLv3.txt') -Force
    Remove-Item -LiteralPath $extractRoot -Recurse -Force
}

$manifest = @"
FFmpeg version: $version
Platform: $Platform ($architecture)
Provider: $provider
Archive: $archiveUrl
GitHub asset ID: $assetId
Archive size: $expectedSize bytes
Archive SHA-256: $expectedSha256
Provider commit: $providerCommit
Provider/source information: $sourceUrl
FFmpeg source releases: https://ffmpeg.org/releases/
"@
Set-Content -LiteralPath (Join-Path $runtimeRoot 'FFMPEG_BUILD.txt') -Value $manifest -Encoding UTF8

foreach ($tool in @($ffmpeg, $ffprobe)) {
    $versionOutput = @(& $tool -version 2>&1)
    $toolExitCode = $LASTEXITCODE
    $versionText = $versionOutput | Select-Object -First 1
    if ($toolExitCode -ne 0 -or $versionText -notmatch "ffmpeg version|ffprobe version") {
        throw "Bundled tool did not execute: $tool"
    }
    Write-Host $versionText
}
Write-Host "Pinned FFmpeg tools ready for $Platform at $runtimeRoot"
