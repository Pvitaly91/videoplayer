[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$artifactRoot = Join-Path $repositoryRoot 'artifacts'
$buildRoot = Join-Path $repositoryRoot 'build'

function Invoke-PortableSelfTest {
    param(
        [Parameter(Mandatory = $true)][ValidateSet('Win32', 'x64')][string]$Platform
    )

    $architecture = if ($Platform -eq 'Win32') { 'win32' } else { 'x64' }
    $packageDirectory = Join-Path $artifactRoot "VideoPlayer-$architecture-portable"
    $application = Join-Path $packageDirectory 'VideoPlayer.exe'
    if (-not (Test-Path -LiteralPath $application -PathType Leaf)) {
        throw "Portable executable is missing: $application"
    }

    Write-Host "Running $Platform portable self-test"
    $process = Start-Process -FilePath $application -ArgumentList '--self-test' -WorkingDirectory $artifactRoot -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) {
        throw "$Platform portable self-test failed with exit code $($process.ExitCode)."
    }
}

if (Test-Path -LiteralPath $artifactRoot) {
    Remove-Item -LiteralPath $artifactRoot -Recurse -Force
}
if (Test-Path -LiteralPath $buildRoot) {
    Remove-Item -LiteralPath $buildRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $artifactRoot -Force | Out-Null

Write-Host 'Fetching and verifying official LibVLC runtimes'
& (Join-Path $PSScriptRoot 'fetch-libvlc.ps1') -Platform Win32
& (Join-Path $PSScriptRoot 'fetch-libvlc.ps1') -Platform x64

Write-Host 'Building Debug|Win32'
& (Join-Path $PSScriptRoot 'build.ps1') -Platform Win32 -Configuration Debug -SkipTests
Write-Host 'Building Release|Win32 and running Win32 native tests'
& (Join-Path $PSScriptRoot 'build.ps1') -Platform Win32 -Configuration Release

Write-Host 'Building Debug|x64'
& (Join-Path $PSScriptRoot 'build.ps1') -Platform x64 -Configuration Debug -SkipTests
Write-Host 'Building Release|x64 and running x64 native tests'
& (Join-Path $PSScriptRoot 'build.ps1') -Platform x64 -Configuration Release

Write-Host 'Creating Win32 and x64 portable folders'
& (Join-Path $PSScriptRoot 'package.ps1') -Platform Win32 -FolderOnly
& (Join-Path $PSScriptRoot 'package.ps1') -Platform x64 -FolderOnly

Invoke-PortableSelfTest -Platform Win32
Invoke-PortableSelfTest -Platform x64

Write-Host 'Verifying portable folders before archiving'
& (Join-Path $PSScriptRoot 'verify-package.ps1') -Platform Win32 -FolderOnly
& (Join-Path $PSScriptRoot 'verify-package.ps1') -Platform x64 -FolderOnly

Write-Host 'Creating Win32 and x64 ZIP archives'
& (Join-Path $PSScriptRoot 'package.ps1') -Platform Win32 -ArchiveOnly
& (Join-Path $PSScriptRoot 'package.ps1') -Platform x64 -ArchiveOnly

# The default verifier additionally opens the completed ZIP and checks its
# entries. This final pass preserves the requested folder/self-test/verify/ZIP
# ordering while also enforcing ZIP validation.
Write-Host 'Verifying completed portable ZIP archives'
& (Join-Path $PSScriptRoot 'verify-package.ps1') -Platform Win32
& (Join-Path $PSScriptRoot 'verify-package.ps1') -Platform x64

Write-Host 'All native configurations, tests, self-tests, portable folders and ZIP archives completed successfully.'
Get-ChildItem -LiteralPath $artifactRoot -Filter '*.zip' -File | Select-Object FullName, Length
