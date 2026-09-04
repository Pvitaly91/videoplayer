[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Win32', 'x64')]
    [string]$Platform,

    [switch]$FolderOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$packageArchitecture = if ($Platform -eq 'Win32') { 'win32' } else { 'x64' }
$packageName = "VideoPlayer-$packageArchitecture-portable"
$packageDirectory = Join-Path $repositoryRoot "artifacts\$packageName"
$zipPath = Join-Path $repositoryRoot "artifacts\$packageName.zip"
$application = Join-Path $packageDirectory 'VideoPlayer.exe'
$libvlc = Join-Path $packageDirectory 'libvlc.dll'
$libvlccore = Join-Path $packageDirectory 'libvlccore.dll'
$libVlcLicense = Join-Path $packageDirectory 'COPYING.LIB'
$portableReadme = Join-Path $packageDirectory 'README-portable.txt'
$thirdPartyNotices = Join-Path $packageDirectory 'THIRD_PARTY_NOTICES.md'
$plugins = Join-Path $packageDirectory 'plugins'
$pluginCache = Join-Path $plugins 'plugins.dat'
$expectedMachine = if ($Platform -eq 'Win32') { 0x014c } else { 0x8664 }
$expectedMachineName = if ($Platform -eq 'Win32') { 'x86' } else { 'x64' }

function Assert-Path {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][ValidateSet('Leaf', 'Container')][string]$Kind
    )
    if (-not (Test-Path -LiteralPath $Path -PathType $Kind)) {
        throw "Required package path is missing: $Path"
    }
}

function Get-PeMachine {
    param([Parameter(Mandatory = $true)][string]$Path)

    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 64 -or $bytes[0] -ne 0x4d -or $bytes[1] -ne 0x5a) {
        throw "Not a valid PE file: $Path"
    }
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3c)
    if ($peOffset -lt 0 -or $peOffset + 6 -gt $bytes.Length -or
        $bytes[$peOffset] -ne 0x50 -or $bytes[$peOffset + 1] -ne 0x45 -or
        $bytes[$peOffset + 2] -ne 0 -or $bytes[$peOffset + 3] -ne 0) {
        throw "Invalid PE header: $Path"
    }
    return [BitConverter]::ToUInt16($bytes, $peOffset + 4)
}

function Find-DumpBin {
    $vswhereCandidates = @(
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'),
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe')
    )
    $vswhere = $vswhereCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace([string]$vswhere)) {
        throw 'vswhere.exe is required to locate dumpbin.exe.'
    }

    $installationPath = & $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace([string]$installationPath)) {
        throw 'Visual Studio 2022 C++ tools were not found.'
    }

    $hostTarget = if ($Platform -eq 'Win32') { 'Hostx64\x86' } else { 'Hostx64\x64' }
    $toolVersions = Get-ChildItem -LiteralPath (Join-Path $installationPath 'VC\Tools\MSVC') -Directory |
        Sort-Object Name -Descending
    foreach ($toolVersion in $toolVersions) {
        $candidate = Join-Path $toolVersion.FullName "bin\$hostTarget\dumpbin.exe"
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }
    throw 'dumpbin.exe was not found in the selected Visual Studio installation.'
}

function Test-BannedRelativePath {
    param([Parameter(Mandatory = $true)][string]$RelativePath)

    $normalized = $RelativePath.Replace('/', '\').TrimStart('\').ToLowerInvariant()
    $leaf = [IO.Path]::GetFileName($normalized)
    if ($normalized -eq 'copying.lib') {
        return $false
    }
    if ($leaf -in @('vlc.exe', 'vlc-cache-gen.exe', 'uninstall.exe', 'install.exe', 'setup.exe', 'dotnet.exe', 'hostfxr.dll', 'hostpolicy.dll', 'coreclr.dll', 'clrjit.dll', 'libvlcsharp.dll')) {
        return $true
    }
    if ($normalized -match '(^|\\)(sdk|msi|obj|\.vs|debug|release)(\\|$)') {
        return $true
    }
    if ($normalized -match '\.(pdb|c|cc|cpp|cxx|h|hpp|ixx|cs|fs|vb|xaml|csproj|fsproj|vbproj|vcxproj|sln|rc|obj|lib|exp|ilk|idb|pch|res|tlog|suo|props|targets|msi|msix|msixbundle|wix|wxs|wxi)$' -or
        $normalized -match '\.(deps|runtimeconfig)(\.dev)?\.json$') {
        return $true
    }
    if ($leaf -match '^(?:system\..*|mscorlib)\.dll$') {
        return $true
    }
    if ($leaf -match '^(msvcp|vcruntime)140(?:_[0-9]+)?d\.dll$' -or $leaf -eq 'ucrtbased.dll') {
        return $true
    }
    return $false
}

Assert-Path -Path $packageDirectory -Kind Container
Assert-Path -Path $application -Kind Leaf
Assert-Path -Path $libvlc -Kind Leaf
Assert-Path -Path $libvlccore -Kind Leaf
Assert-Path -Path $libVlcLicense -Kind Leaf
Assert-Path -Path $portableReadme -Kind Leaf
Assert-Path -Path $thirdPartyNotices -Kind Leaf
Assert-Path -Path $plugins -Kind Container
Assert-Path -Path $pluginCache -Kind Leaf
if ((Get-Item -LiteralPath $pluginCache).Length -le 0) {
    throw "The LibVLC plugin cache is empty: $pluginCache"
}
foreach ($document in @($libVlcLicense, $portableReadme, $thirdPartyNotices)) {
    if ((Get-Item -LiteralPath $document).Length -le 0) {
        throw "Required package document is empty: $document"
    }
}
if (-not $FolderOnly) {
    Assert-Path -Path $zipPath -Kind Leaf
}

$pluginFile = Get-ChildItem -LiteralPath $plugins -Filter '*.dll' -File -Recurse | Select-Object -First 1
if ($null -eq $pluginFile) {
    throw "The plugins directory has no plugin DLLs: $plugins"
}

foreach ($requiredBinary in @($application, $libvlc, $libvlccore)) {
    $machine = Get-PeMachine -Path $requiredBinary
    if ($machine -ne $expectedMachine) {
        throw ('Architecture mismatch in {0}: expected {1} (0x{2:x4}), found 0x{3:x4}.' -f
            $requiredBinary, $expectedMachineName, $expectedMachine, $machine)
    }
}

# Detect architecture mixing in every staged runtime DLL, including plugins.
foreach ($binary in Get-ChildItem -LiteralPath $packageDirectory -Filter '*.dll' -File -Recurse) {
    $machine = Get-PeMachine -Path $binary.FullName
    if ($machine -ne $expectedMachine) {
        throw ('Mixed architecture DLL: {0} is machine 0x{1:x4}, expected 0x{2:x4}.' -f
            $binary.FullName, $machine, $expectedMachine)
    }
}

$packagePrefix = $packageDirectory
if (-not $packagePrefix.EndsWith([IO.Path]::DirectorySeparatorChar)) {
    $packagePrefix += [IO.Path]::DirectorySeparatorChar
}
foreach ($file in Get-ChildItem -LiteralPath $packageDirectory -File -Recurse) {
    $relative = $file.FullName.Substring($packagePrefix.Length)
    if (Test-BannedRelativePath -RelativePath $relative) {
        throw "Banned file in portable folder: $relative"
    }
}
foreach ($directory in Get-ChildItem -LiteralPath $packageDirectory -Directory -Recurse) {
    $relative = $directory.FullName.Substring($packagePrefix.Length)
    if ($relative -match '(^|[\\/])(sdk|msi|obj|\.vs|debug|release)($|[\\/])') {
        throw "Development or installer directory in portable folder: $relative"
    }
}

if (-not $FolderOnly) {
    $zipInfo = Get-Item -LiteralPath $zipPath
    if ($zipInfo.Length -le 0) {
        throw "Portable ZIP is empty: $zipPath"
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        if ($archive.Entries.Count -eq 0) {
            throw "Portable ZIP has no entries: $zipPath"
        }
        $entryNames = @($archive.Entries | ForEach-Object { $_.FullName.Replace('/', '\').TrimStart('\') })
        foreach ($requiredEntry in @(
            'VideoPlayer.exe',
            'libvlc.dll',
            'libvlccore.dll',
            'COPYING.LIB',
            'README-portable.txt',
            'THIRD_PARTY_NOTICES.md',
            'plugins\plugins.dat')) {
            $requiredArchiveEntry = $archive.Entries | Where-Object {
                $_.FullName.Replace('/', '\').TrimStart('\') -eq $requiredEntry
            } | Select-Object -First 1
            if ($null -eq $requiredArchiveEntry -or $requiredArchiveEntry.Length -le 0) {
                throw "Portable ZIP is missing non-empty $requiredEntry."
            }
        }
        $zippedPlugin = $archive.Entries | Where-Object {
            $_.FullName.Replace('/', '\') -like 'plugins\*.dll' -and $_.Length -gt 0
        } | Select-Object -First 1
        if ($null -eq $zippedPlugin) {
            throw 'Portable ZIP has no plugin DLLs.'
        }
        foreach ($entryName in $entryNames) {
            if (Test-BannedRelativePath -RelativePath $entryName) {
                throw "Banned file in portable ZIP: $entryName"
            }
        }
    }
    finally {
        $archive.Dispose()
    }
}

Write-Host "Running portable self-test: $application --self-test"
$selfTest = Start-Process -FilePath $application -ArgumentList '--self-test' -WorkingDirectory $repositoryRoot -WindowStyle Hidden -Wait -PassThru
if ($selfTest.ExitCode -ne 0) {
    throw "Portable self-test failed for $Platform with exit code $($selfTest.ExitCode)."
}

$dumpbin = Find-DumpBin
$dependents = (& $dumpbin /nologo /dependents $application 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0) {
    throw "dumpbin /dependents failed for $application."
}
if ($dependents -match '(?i)\b(?:MSVCP|VCRUNTIME)140(?:_[0-9]+)?\.DLL\b') {
    throw "VideoPlayer.exe imports a dynamic MSVC runtime despite the /MT requirement:`n$dependents"
}

Write-Host $dependents.Trim()
$scope = if ($FolderOnly) { 'portable folder' } else { 'portable folder and ZIP' }
Write-Host "Package verification passed for $Platform ($expectedMachineName): $scope; self-test returned 0 and no dynamic MSVC runtime import was found."
