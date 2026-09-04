[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Win32', 'x64')]
    [string]$Platform,

    [Parameter(Mandatory = $true)]
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration,

    [switch]$SkipTests
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$solution = Join-Path $repositoryRoot 'VideoPlayer.sln'

function Find-VsWhere {
    $candidates = @(
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'),
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe')
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }
    throw 'Visual Studio Installer vswhere.exe was not found. Install Visual Studio 2022 with Desktop development with C++.'
}

function Find-MSBuild {
    $vswhere = Find-VsWhere
    $matches = @(& $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.Component.MSBuild Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\**\Bin\MSBuild.exe')
    $msbuild = $matches | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace([string]$msbuild)) {
        throw 'MSBuild from Visual Studio 2022 was not found.'
    }
    return [string]$msbuild
}

if (-not (Test-Path -LiteralPath $solution -PathType Leaf)) {
    throw "Solution not found: $solution"
}

$msbuild = Find-MSBuild
& (Join-Path $PSScriptRoot 'fetch-libvlc.ps1') -Platform $Platform

Write-Host "Building $Configuration|$Platform with $msbuild"
& $msbuild $solution /nologo /m /t:Build "/p:Configuration=$Configuration" "/p:Platform=$Platform" /verbosity:minimal
if ($LASTEXITCODE -ne 0) {
    throw "MSBuild failed for $Configuration|$Platform with exit code $LASTEXITCODE."
}

$tests = Join-Path $repositoryRoot "build\$Platform\$Configuration\VideoPlayer.Tests.exe"
if (-not (Test-Path -LiteralPath $tests -PathType Leaf)) {
    throw "Native test executable was not produced: $tests"
}

if (-not $SkipTests) {
    Write-Host "Running native tests: $tests"
    & $tests
    if ($LASTEXITCODE -ne 0) {
        throw "Native tests failed for $Configuration|$Platform with exit code $LASTEXITCODE."
    }
}

Write-Host "Build completed successfully: $Configuration|$Platform"
