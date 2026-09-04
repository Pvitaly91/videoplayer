[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Win32', 'x64')]
    [string]$Platform
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$version = '3.0.23'
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$versionRoot = Join-Path $repositoryRoot "third_party\libvlc\$version"
$downloadRoot = Join-Path $versionRoot '_downloads'

$platforms = @{
    Win32 = @{
        Folder = 'win32'
        Archive = "vlc-$version-win32.zip"
        Url = "https://download.videolan.org/pub/videolan/vlc/$version/win32/vlc-$version-win32.zip"
    }
    x64 = @{
        Folder = 'win64'
        Archive = "vlc-$version-win64.zip"
        Url = "https://download.videolan.org/pub/videolan/vlc/$version/win64/vlc-$version-win64.zip"
    }
}

$selected = $platforms[$Platform]
$archiveName = [string]$selected.Archive
$archiveUrl = [string]$selected.Url
$checksumUrl = "$archiveUrl.sha256"
$archivePath = Join-Path $downloadRoot $archiveName
$checksumPath = "$archivePath.sha256"
$destination = Join-Path $versionRoot ([string]$selected.Folder)

function Invoke-VerifiedDownload {
    param(
        [Parameter(Mandatory = $true)][string]$Uri,
        [Parameter(Mandatory = $true)][string]$OutputPath
    )

    $temporaryPath = "$OutputPath.partial-$PID"
    try {
        Write-Host "Downloading $Uri"
        Invoke-WebRequest -Uri $Uri -OutFile $temporaryPath -UseBasicParsing
        Move-Item -LiteralPath $temporaryPath -Destination $OutputPath -Force
    }
    finally {
        if (Test-Path -LiteralPath $temporaryPath) {
            Remove-Item -LiteralPath $temporaryPath -Force
        }
    }
}

function Test-LibVlcLayout {
    param([Parameter(Mandatory = $true)][string]$Path)

    $plugins = Join-Path $Path 'plugins'
    if (-not (Test-Path -LiteralPath (Join-Path $Path 'libvlc.dll') -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $Path 'libvlccore.dll') -PathType Leaf) -or
        -not (Test-Path -LiteralPath $plugins -PathType Container)) {
        return $false
    }

    return $null -ne (Get-ChildItem -LiteralPath $plugins -Filter '*.dll' -File -Recurse | Select-Object -First 1)
}

function Ensure-PluginCache {
    param([Parameter(Mandatory = $true)][string]$Path)

    $plugins = Join-Path $Path 'plugins'
    $cache = Join-Path $plugins 'plugins.dat'
    if ((Test-Path -LiteralPath $cache -PathType Leaf) -and
        (Get-Item -LiteralPath $cache).Length -gt 0) {
        Write-Host "Using the existing LibVLC plugin cache: $cache"
        return
    }

    $generator = Join-Path $Path 'vlc-cache-gen.exe'
    if (-not (Test-Path -LiteralPath $generator -PathType Leaf)) {
        throw "The official LibVLC cache generator is missing: $generator"
    }

    Write-Host "Generating the LibVLC plugin cache: $cache"
    & $generator $plugins
    if ($LASTEXITCODE -ne 0) {
        throw "vlc-cache-gen.exe failed with exit code $LASTEXITCODE."
    }
    if (-not (Test-Path -LiteralPath $cache -PathType Leaf) -or
        (Get-Item -LiteralPath $cache).Length -le 0) {
        throw "vlc-cache-gen.exe did not create a non-empty plugin cache: $cache"
    }
}

New-Item -ItemType Directory -Path $downloadRoot -Force | Out-Null

# PowerShell 5.1 on older hosts may otherwise negotiate an obsolete TLS version.
[Net.ServicePointManager]::SecurityProtocol =
    [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

if (-not (Test-Path -LiteralPath $checksumPath -PathType Leaf)) {
    Invoke-VerifiedDownload -Uri $checksumUrl -OutputPath $checksumPath
}

$checksumText = Get-Content -LiteralPath $checksumPath -Raw
$checksumMatch = [regex]::Match($checksumText, '(?i)\b[0-9a-f]{64}\b')
if (-not $checksumMatch.Success) {
    throw "The official checksum file has no SHA-256 value: $checksumPath"
}
$expectedHash = $checksumMatch.Value.ToLowerInvariant()

if (-not (Test-Path -LiteralPath $archivePath -PathType Leaf)) {
    Invoke-VerifiedDownload -Uri $archiveUrl -OutputPath $archivePath
}
else {
    Write-Host "Using the cached archive: $archivePath"
}

$actualHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualHash -ne $expectedHash) {
    throw "LibVLC archive checksum mismatch. Expected $expectedHash, got $actualHash. Delete the invalid archive manually: $archivePath"
}
Write-Host "SHA-256 verified: $actualHash"

if (Test-LibVlcLayout -Path $destination) {
    Ensure-PluginCache -Path $destination
    Write-Host "LibVLC $version for $Platform is already extracted: $destination"
    return
}

$extractionRoot = Join-Path $versionRoot ".extract-$($selected.Folder)-$PID"
if (Test-Path -LiteralPath $extractionRoot) {
    Remove-Item -LiteralPath $extractionRoot -Recurse -Force
}

try {
    New-Item -ItemType Directory -Path $extractionRoot -Force | Out-Null
    Write-Host "Extracting $archiveName"
    Expand-Archive -LiteralPath $archivePath -DestinationPath $extractionRoot -Force

    $candidate = Get-ChildItem -LiteralPath $extractionRoot -Filter 'libvlc.dll' -File -Recurse |
        ForEach-Object { $_.Directory.FullName } |
        Where-Object { Test-LibVlcLayout -Path $_ } |
        Select-Object -First 1

    if ([string]::IsNullOrWhiteSpace([string]$candidate)) {
        throw 'The verified archive does not contain the expected LibVLC runtime layout.'
    }

    if (Test-Path -LiteralPath $destination) {
        Remove-Item -LiteralPath $destination -Recurse -Force
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Move-Item -LiteralPath $candidate -Destination $destination

    if (-not (Test-LibVlcLayout -Path $destination)) {
        throw "Extracted LibVLC layout is incomplete: $destination"
    }
}
finally {
    if (Test-Path -LiteralPath $extractionRoot) {
        Remove-Item -LiteralPath $extractionRoot -Recurse -Force
    }
}

Ensure-PluginCache -Path $destination

Write-Host "LibVLC $version for $Platform is ready: $destination"
