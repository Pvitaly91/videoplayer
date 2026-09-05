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
$releaseApplication = Join-Path $repositoryRoot "build\$Platform\Release\VideoPlayer.exe"
$nativeTests = Join-Path $repositoryRoot "build\$Platform\Release\VideoPlayer.Tests.exe"
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

function Get-StreamSha256 {
    param([Parameter(Mandatory = $true)][IO.Stream]$Stream)

    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $hash = $algorithm.ComputeHash($Stream)
        return [BitConverter]::ToString($hash).Replace('-', '')
    }
    finally {
        $algorithm.Dispose()
    }
}

function Get-FileSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    $stream = [IO.File]::OpenRead($Path)
    try {
        return Get-StreamSha256 -Stream $stream
    }
    finally {
        $stream.Dispose()
    }
}

function Assert-FileContentsEqual {
    param(
        [Parameter(Mandatory = $true)][string]$Expected,
        [Parameter(Mandatory = $true)][string]$Actual,
        [Parameter(Mandatory = $true)][string]$Description
    )

    $expectedInfo = Get-Item -LiteralPath $Expected
    $actualInfo = Get-Item -LiteralPath $Actual
    if ($expectedInfo.Length -ne $actualInfo.Length -or
        (Get-FileSha256 -Path $Expected) -ne (Get-FileSha256 -Path $Actual)) {
        throw "$Description does not match: $Actual"
    }
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

function Remove-CppComments {
    param([Parameter(Mandatory = $true)][string]$Text)

    # Preserve quoted literals while blanking comments. This keeps a forbidden
    # filename inside a real runtime string detectable, but words in explanatory
    # comments (including PrivacyPolicy.h) cannot create false positives.
    $literalOrComment = @'
(?<literal>(?:u8|[uUL])?"(?:\\.|[^"\\])*"|(?:u8|[uUL])?'(?:\\.|[^'\\])*')|(?<comment>//[^\r\n]*|/\*[\s\S]*?\*/)
'@
    return [regex]::Replace($Text, $literalOrComment, {
        param($match)
        if (-not $match.Groups['comment'].Success) {
            return $match.Value
        }
        return [regex]::Replace($match.Value, '[^\r\n]', ' ')
    })
}

function Assert-PrivateProductionSource {
    $sourceRoot = Join-Path $repositoryRoot 'src\VideoPlayer'
    if (-not (Test-Path -LiteralPath $sourceRoot -PathType Container)) {
        throw "Production source directory is missing: $sourceRoot"
    }

    $sourceFiles = @(Get-ChildItem -LiteralPath $sourceRoot -File -Recurse | Where-Object {
        $_.Extension.ToLowerInvariant() -in @('.cpp', '.cxx', '.h', '.hpp', '.rc')
    })
    if ($sourceFiles.Count -eq 0) {
        throw "No production source files were found under $sourceRoot."
    }

    $forbiddenSourcePatterns = [ordered]@{
        'Windows Recent Items API' = '\bSHAddToRecentDocs(?:A|W)?\b'
        'Jump List destination-list API' = '\b(?:ICustomDestinationList|IApplicationDestinations|ApplicationDestinations)\b'
        'registry key creation API' = '\bRegCreateKey(?:Ex)?(?:A|W)?\b'
        'registry value write API' = '\bRegSetValue(?:Ex)?(?:A|W)?\b'
        'C++ output file stream' = '\bstd\s*::\s*(?:o|wo|basic_o)fstream\b'
        'write-capable Win32 file access' = '\bGENERIC_WRITE\b'
        'Win32 file-write API' = '\bWriteFile(?:Ex)?\b'
        'Win32 file-creation disposition' = '\b(?:CREATE_ALWAYS|CREATE_NEW|OPEN_ALWAYS|TRUNCATE_EXISTING)\b'
        'temporary-file API' = '\b(?:GetTempPath|GetTempFileName)(?:A|W)?\b'
        'LibVLC snapshot-to-disk API' = '\blibvlc_video_take_snapshot\b'
        'runtime persistence filename' = '(?i)(?:recent\.json|history\.json|settings\.json|config\.ini|playback-history)'
        'disk preview-cache path' = '(?i)(?:preview-cache|[\\/]thumbnails?[\\/])'
    }
    foreach ($sourceFile in $sourceFiles) {
        $relativeName = $sourceFile.FullName.Substring($sourceRoot.Length).TrimStart('\')
        $sourceText = Remove-CppComments -Text ([IO.File]::ReadAllText($sourceFile.FullName))
        foreach ($entry in $forbiddenSourcePatterns.GetEnumerator()) {
            $match = [regex]::Match($sourceText, [string]$entry.Value)
            if ($match.Success) {
                $sampleStart = [Math]::Max(0, $match.Index - 40)
                $sampleLength = [Math]::Min(120, $sourceText.Length - $sampleStart)
                $sample = $sourceText.Substring($sampleStart, $sampleLength).Replace("`r", ' ').Replace("`n", ' ').Trim()
                throw "Production source contains forbidden $($entry.Key) in $relativeName`: $sample"
            }
        }
    }

    $privacyPolicyPath = Join-Path $sourceRoot 'PrivacyPolicy.h'
    $mainWindowPath = Join-Path $sourceRoot 'MainWindow.cpp'
    $mediaOpenPath = Join-Path $sourceRoot 'MediaOpen.cpp'
    $playerEnginePath = Join-Path $sourceRoot 'PlayerEngine.cpp'
    $previewEnginePath = Join-Path $sourceRoot 'PreviewEngine.cpp'
    foreach ($requiredSource in @($privacyPolicyPath, $mainWindowPath, $mediaOpenPath, $playerEnginePath, $previewEnginePath)) {
        if (-not (Test-Path -LiteralPath $requiredSource -PathType Leaf)) {
            throw "Privacy-critical production source is missing: $requiredSource"
        }
    }

    $privacyPolicy = Remove-CppComments -Text ([IO.File]::ReadAllText($privacyPolicyPath))
    foreach ($requiredToken in @(
        'OFN_DONTADDTORECENT',
        '--ignore-config',
        '--no-media-library',
        '--no-video-title-show')) {
        if ($privacyPolicy.IndexOf($requiredToken, [StringComparison]::Ordinal) -lt 0) {
            throw "PrivacyPolicy.h is missing required token: $requiredToken"
        }
    }

    $mainWindowSource = Remove-CppComments -Text ([IO.File]::ReadAllText($mainWindowPath))
    if ($mainWindowSource -notmatch '\bkPrivateOpenDialogFlags\b') {
        throw 'GetOpenFileNameW does not consume kPrivateOpenDialogFlags/OFN_DONTADDTORECENT.'
    }

    $mediaOpenSource = Remove-CppComments -Text ([IO.File]::ReadAllText($mediaOpenPath))
    foreach ($requiredToken in @(
        'CreateFileMappingW',
        'INVALID_HANDLE_VALUE',
        'PAGE_READWRITE',
        'MapViewOfFile',
        'FILE_MAP_READ',
        'FILE_MAP_WRITE',
        'PROC_THREAD_ATTRIBUTE_HANDLE_LIST',
        '--media-map=')) {
        if ($mediaOpenSource.IndexOf($requiredToken, [StringComparison]::Ordinal) -lt 0) {
            throw "MediaOpen.cpp is missing anonymous shared-memory privacy control: $requiredToken"
        }
    }
    if ($mediaOpenSource -notmatch '\bCreateFileMappingW\s*\(\s*INVALID_HANDLE_VALUE\s*,') {
        throw 'MediaOpen.cpp IPC mapping is not explicitly pagefile-backed.'
    }
    foreach ($playerSourcePath in @($playerEnginePath, $previewEnginePath)) {
        $playerSource = Remove-CppComments -Text ([IO.File]::ReadAllText($playerSourcePath))
        if ($playerSource -notmatch '\bkPrivateLibVlcArguments\b') {
            throw "LibVLC player does not consume the shared private arguments: $playerSourcePath"
        }
    }

    Write-Host "Static no-history source verification passed ($($sourceFiles.Count) production files)."
}

function Test-BannedRelativePath {
    param([Parameter(Mandatory = $true)][string]$RelativePath)

    $normalized = $RelativePath.Replace('/', '\').TrimStart('\').TrimEnd('\').ToLowerInvariant()
    $leaf = [IO.Path]::GetFileName($normalized)
    if ($normalized -eq 'copying.lib') {
        return $false
    }
    $segments = @($normalized -split '\\')
    if ($segments | Where-Object { $_ -in @('history', 'recent', 'settings', 'preview-cache', 'thumbnails', 'log', 'logs') }) {
        return $true
    }
    if ($leaf -match '^(?:history|recent|settings)(?:\.|$)' -or
        $leaf -eq 'config.ini' -or
        $leaf -match '\.(?:db|log|tmp)$') {
        return $true
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

Assert-PrivateProductionSource

Assert-Path -Path $releaseApplication -Kind Leaf
Assert-Path -Path $nativeTests -Kind Leaf
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

Assert-FileContentsEqual `
    -Expected $releaseApplication `
    -Actual $application `
    -Description 'Portable VideoPlayer.exe and the current Release build'

$buildInputs = @(
    Get-ChildItem -LiteralPath (Join-Path $repositoryRoot 'src') -File -Recurse
    Get-ChildItem -LiteralPath (Join-Path $repositoryRoot 'tests') -File -Recurse
) | Where-Object {
    $_.Extension.ToLowerInvariant() -in @(
        '.cpp', '.cxx', '.h', '.hpp', '.rc', '.vcxproj', '.props', '.targets')
}
$latestBuildInput = $buildInputs | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
if ($null -eq $latestBuildInput) {
    throw 'No native build inputs were found for freshness verification.'
}
foreach ($buildOutput in @($releaseApplication, $nativeTests)) {
    if ((Get-Item -LiteralPath $buildOutput).LastWriteTimeUtc -lt $latestBuildInput.LastWriteTimeUtc) {
        throw "Native build output is older than $($latestBuildInput.FullName): $buildOutput. Re-run scripts\build.ps1."
    }
}

Write-Host "Running native tests, including multi-instance IPC: $nativeTests"
& $nativeTests
if ($LASTEXITCODE -ne 0) {
    throw "Native tests failed for $Platform with exit code $LASTEXITCODE."
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
    if (Test-BannedRelativePath -RelativePath $relative) {
        throw "Banned directory in portable folder: $relative"
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

        $folderFilesByPath = @{}
        foreach ($file in Get-ChildItem -LiteralPath $packageDirectory -File -Recurse) {
            $relative = $file.FullName.Substring($packagePrefix.Length).Replace('/', '\').TrimStart('\')
            if ($folderFilesByPath.ContainsKey($relative)) {
                throw "Duplicate case-insensitive path in portable folder: $relative"
            }
            $folderFilesByPath[$relative] = $file
        }

        $zipEntriesByPath = @{}
        foreach ($entry in $archive.Entries | Where-Object { -not [string]::IsNullOrEmpty($_.Name) }) {
            $relative = $entry.FullName.Replace('/', '\').TrimStart('\')
            if ($zipEntriesByPath.ContainsKey($relative)) {
                throw "Duplicate case-insensitive path in portable ZIP: $relative"
            }
            $zipEntriesByPath[$relative] = $entry
        }

        foreach ($relative in $folderFilesByPath.Keys) {
            if (-not $zipEntriesByPath.ContainsKey($relative)) {
                throw "Portable ZIP is missing folder file: $relative"
            }

            $file = $folderFilesByPath[$relative]
            $entry = $zipEntriesByPath[$relative]
            if ($entry.Length -ne $file.Length) {
                throw "Portable ZIP entry length differs from folder file: $relative"
            }

            $folderStream = [IO.File]::OpenRead($file.FullName)
            $entryStream = $entry.Open()
            try {
                $folderHash = Get-StreamSha256 -Stream $folderStream
                $entryHash = Get-StreamSha256 -Stream $entryStream
                if ($folderHash -ne $entryHash) {
                    throw "Portable ZIP entry content differs from folder file: $relative"
                }
            }
            finally {
                $entryStream.Dispose()
                $folderStream.Dispose()
            }
        }

        foreach ($relative in $zipEntriesByPath.Keys) {
            if (-not $folderFilesByPath.ContainsKey($relative)) {
                throw "Portable ZIP contains a file absent from the portable folder: $relative"
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
Write-Host "Package verification passed for $Platform ($expectedMachineName): source is no-history clean; $scope contains no persistence artifacts; self-test returned 0 and no dynamic MSVC runtime import was found."
