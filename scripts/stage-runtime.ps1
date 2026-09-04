[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Win32', 'x64')]
    [string]$Platform,

    [Parameter(Mandatory = $true)]
    [string]$Destination
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$architectureFolder = if ($Platform -eq 'Win32') { 'win32' } else { 'win64' }
$sourceRoot = Join-Path $repositoryRoot "third_party\libvlc\3.0.23\$architectureFolder"
$pluginCache = Join-Path $sourceRoot 'plugins\plugins.dat'
$copiedFiles = 0
$unchangedFiles = 0

if (-not (Test-Path -LiteralPath (Join-Path $sourceRoot 'libvlc.dll') -PathType Leaf) -or
    -not (Test-Path -LiteralPath (Join-Path $sourceRoot 'libvlccore.dll') -PathType Leaf) -or
    -not (Test-Path -LiteralPath (Join-Path $sourceRoot 'plugins') -PathType Container) -or
    -not (Test-Path -LiteralPath $pluginCache -PathType Leaf) -or
    (Get-Item -LiteralPath $pluginCache -ErrorAction SilentlyContinue).Length -le 0) {
    throw "LibVLC runtime is missing for $Platform. Run scripts\fetch-libvlc.ps1 -Platform $Platform first."
}

$destinationRoot = [IO.Path]::GetFullPath($Destination)
New-Item -ItemType Directory -Path $destinationRoot -Force | Out-Null

function Copy-FileIncrementally {
    param(
        [Parameter(Mandatory = $true)][string]$SourceFile,
        [Parameter(Mandatory = $true)][string]$DestinationFile
    )

    $sourceInfo = Get-Item -LiteralPath $SourceFile
    $copyRequired = $true
    if (Test-Path -LiteralPath $DestinationFile -PathType Leaf) {
        $destinationInfo = Get-Item -LiteralPath $DestinationFile
        $copyRequired = $sourceInfo.Length -ne $destinationInfo.Length -or
                        $sourceInfo.LastWriteTimeUtc -gt $destinationInfo.LastWriteTimeUtc
    }

    if (-not $copyRequired) {
        $script:unchangedFiles++
        return
    }

    $parent = Split-Path -Parent $DestinationFile
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    Copy-Item -LiteralPath $SourceFile -Destination $DestinationFile -Force
    $script:copiedFiles++
}

function Copy-DirectoryIncrementally {
    param(
        [Parameter(Mandatory = $true)][string]$SourceDirectory,
        [Parameter(Mandatory = $true)][string]$DestinationDirectory
    )

    $sourcePrefix = $SourceDirectory
    if (-not $sourcePrefix.EndsWith([IO.Path]::DirectorySeparatorChar)) {
        $sourcePrefix += [IO.Path]::DirectorySeparatorChar
    }

    foreach ($file in Get-ChildItem -LiteralPath $SourceDirectory -File -Recurse) {
        $relativePath = $file.FullName.Substring($sourcePrefix.Length)
        Copy-FileIncrementally -SourceFile $file.FullName -DestinationFile (Join-Path $DestinationDirectory $relativePath)
    }
}

# The official ZIP also contains obsolete browser/ActiveX plugin DLLs
# (npvlc.dll and axvlc.dll). They are not LibVLC runtime dependencies and are
# intentionally omitted together with vlc.exe and uninstall.exe.
$runtimeDlls = @('libvlc.dll', 'libvlccore.dll')
$rootFiles = Get-ChildItem -LiteralPath $sourceRoot -File | Where-Object {
    $_.Name -in $runtimeDlls -or
    $_.Name -ieq 'plugins.dat' -or
    $_.Name -like 'COPYING*' -or
    $_.Name -like 'LICENSE*' -or
    $_.Name -like 'AUTHORS*' -or
    $_.Name -like 'THANKS*' -or
    $_.Name -like 'NEWS*' -or
    $_.Name -like 'README*'
}
foreach ($file in $rootFiles) {
    Copy-FileIncrementally -SourceFile $file.FullName -DestinationFile (Join-Path $destinationRoot $file.Name)
}

# Copy the official runtime directories without pruning codecs. Development
# SDK and WiX installer sources are excluded. Subsequent builds copy changed
# files only.
foreach ($directory in Get-ChildItem -LiteralPath $sourceRoot -Directory) {
    if ($directory.Name -in @('sdk', 'msi')) {
        continue
    }
    Copy-DirectoryIncrementally -SourceDirectory $directory.FullName -DestinationDirectory (Join-Path $destinationRoot $directory.Name)
}

Write-Host "LibVLC runtime staged for $Platform at $destinationRoot ($copiedFiles copied, $unchangedFiles unchanged)."
