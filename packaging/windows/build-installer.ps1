# Builds the Windows beta installer with Inno Setup.
#   pwsh packaging/windows/build-installer.ps1 [-ArtefactDir <path>] [-Build <label>]
# The version comes from project(... VERSION x.y.z) in CMakeLists.txt.
# Expects a Release build first:  cmake --build build --config Release
# Output: build/installer/ToneLegionMultiband-<version>-beta-windows-x64-setup.exe
param(
    [string]$ArtefactDir = "",
    [string]$Build = "local"
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

$cmake = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmake -notmatch 'project\(\s*ToneLegionMultiband\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
    throw 'Could not read the project version from CMakeLists.txt'
}
$version = $Matches[1]

if ($ArtefactDir -eq '') { $ArtefactDir = Join-Path $root 'build\ToneLegionMultiband_artefacts\Release' }
$ArtefactDir = (Resolve-Path $ArtefactDir).Path
foreach ($p in @('VST3\Tone Legion Multiband.vst3', 'CLAP\Tone Legion Multiband.clap')) {
    if (-not (Test-Path (Join-Path $ArtefactDir $p))) { throw "Missing build output: $ArtefactDir\$p" }
}

$iscc = (Get-Command iscc.exe -ErrorAction SilentlyContinue).Source
if (-not $iscc) {
    $iscc = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $iscc) { throw 'Inno Setup 6 (ISCC.exe) not found. Install Inno Setup 6.3 or newer.' }

Write-Host "Inno Setup: $iscc"
Write-Host "Version $version, build $Build, artefacts $ArtefactDir"
& $iscc "/DAppVersion=$version" "/DAppBuild=$Build" "/DArtefactDir=$ArtefactDir" (Join-Path $PSScriptRoot 'ToneLegionMultiband.iss')
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }
