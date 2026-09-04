[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-DotNet {
    param(
        [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)]
        [string[]] $Arguments
    )

    Write-Host "dotnet $($Arguments -join ' ')"
    & dotnet @Arguments

    if ($LASTEXITCODE -ne 0) {
        throw "Команда dotnet завершилася з кодом $LASTEXITCODE."
    }
}

function Assert-PublishedFile {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path
    )

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "У portable-збірці відсутній обов'язковий файл: $Path"
    }
}

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$artifactRoot = [System.IO.Path]::GetFullPath((Join-Path $repositoryRoot 'artifacts'))
$publishDirectory = [System.IO.Path]::GetFullPath((Join-Path $artifactRoot 'win-x64'))
$solutionPath = Join-Path $repositoryRoot 'VideoPlayer.sln'
$applicationProject = Join-Path $repositoryRoot 'src\VideoPlayer\VideoPlayer.csproj'
$testProject = Join-Path $repositoryRoot 'tests\VideoPlayer.Tests\VideoPlayer.Tests.csproj'

$expectedPrefix = $artifactRoot.TrimEnd([System.IO.Path]::DirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
if (-not $publishDirectory.StartsWith($expectedPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Небезпечний шлях каталогу публікації: $publishDirectory"
}

Push-Location $repositoryRoot
try {
    if (Test-Path -LiteralPath $publishDirectory) {
        Remove-Item -LiteralPath $publishDirectory -Recurse -Force
    }

    New-Item -ItemType Directory -Path $publishDirectory -Force | Out-Null

    Invoke-DotNet -Arguments @('restore', $solutionPath)
    Invoke-DotNet -Arguments @('build', $solutionPath, '-c', 'Release', '--no-restore')
    Invoke-DotNet -Arguments @(
        'test',
        $testProject,
        '-c',
        'Release',
        '--no-build',
        '--no-restore',
        '-p:Platform=x64'
    )
    Invoke-DotNet -Arguments @(
        'publish',
        $applicationProject,
        '-c',
        'Release',
        '-r',
        'win-x64',
        '--self-contained',
        'true',
        '-p:PublishReadyToRun=true',
        '-p:PublishSingleFile=false',
        '-p:PublishTrimmed=false',
        '-o',
        $publishDirectory
    )

    $requiredFiles = @(
        'VideoPlayer.exe',
        'VideoPlayer.dll',
        'VideoPlayer.runtimeconfig.json',
        'LibVLCSharp.dll',
        'LibVLCSharp.WPF.dll',
        'THIRD_PARTY_NOTICES.md',
        'hostfxr.dll',
        'hostpolicy.dll',
        'coreclr.dll',
        'System.Private.CoreLib.dll'
    )

    foreach ($fileName in $requiredFiles) {
        Assert-PublishedFile (Join-Path $publishDirectory $fileName)
    }

    $libVlcDirectory = Join-Path $publishDirectory 'libvlc\win-x64'
    Assert-PublishedFile (Join-Path $libVlcDirectory 'libvlc.dll')
    Assert-PublishedFile (Join-Path $libVlcDirectory 'libvlccore.dll')

    $nativeRoots = Get-ChildItem -LiteralPath (Join-Path $publishDirectory 'libvlc') -Directory
    $unexpectedNativeRoots = @($nativeRoots | Where-Object { $_.Name -ne 'win-x64' })
    if ($unexpectedNativeRoots.Count -gt 0) {
        throw "У portable-збірці знайдено неочікувану архітектуру LibVLC: $($unexpectedNativeRoots.Name -join ', ')"
    }

    $pluginsDirectory = Join-Path $libVlcDirectory 'plugins'
    if (-not (Test-Path -LiteralPath $pluginsDirectory -PathType Container)) {
        throw "У portable-збірці не знайдено каталог плагінів LibVLC: $pluginsDirectory"
    }

    $plugin = Get-ChildItem -LiteralPath $pluginsDirectory -Filter '*.dll' -File -Recurse |
        Select-Object -First 1
    if ($null -eq $plugin) {
        throw "Каталог плагінів LibVLC не містить DLL: $pluginsDirectory"
    }

    Write-Host ''
    Write-Host 'Portable-збірку успішно створено та перевірено:'
    Write-Host "  $publishDirectory"
    Write-Host "LibVLC: $libVlcDirectory"
}
finally {
    Pop-Location
}
