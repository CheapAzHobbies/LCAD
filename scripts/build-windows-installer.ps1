# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the Windows installer: LCAD with Qt, Open CASCADE, the other libraries it needs, and
# Visual C++'s runtime, put together by Inno Setup.
#
# Usage: scripts/build-windows-installer.ps1 [-Output <folder>] [-Build <build folder>]
#   The default output folder is build\windows. The result is LCAD-<version>-windows-x64-setup.exe.
#   Without a build folder LCAD is built afresh, in the output folder; with one that is already
#   configured and built, as CI has, that one is used.
#
# Needs what LCAD builds with on Windows (docs/BUILDING.md), run where Visual Studio's compiler
# and the conda environment are both active, and Inno Setup 6 (winget install JRSoftware.InnoSetup).
param(
    [string]$Output = "",
    [string]$Build = ""
)
$ErrorActionPreference = "Stop"

# Stops when the program just run failed.
function Assert-Success([string]$what) {
    if ($LASTEXITCODE -ne 0) { throw "$what failed with exit code $LASTEXITCODE" }
}

$root = Split-Path -Parent $PSScriptRoot
if (-not $Output) { $Output = Join-Path $root "build\windows" }
New-Item -ItemType Directory -Force $Output | Out-Null
$Output = (Resolve-Path $Output).Path
$work = Join-Path $Output "work"

$iscc = Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe"
if (-not (Test-Path $iscc)) {
    $found = Get-Command iscc -ErrorAction SilentlyContinue
    if (-not $found) { throw "Inno Setup 6 is needed: winget install JRSoftware.InnoSetup" }
    $iscc = $found.Source
}

# Everything below is generated, so start from nothing.
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
if (-not $Build) {
    $Build = Join-Path $work "build"
    $prefix = @()
    if ($env:CONDA_PREFIX) { $prefix = @("-DCMAKE_PREFIX_PATH=$env:CONDA_PREFIX/Library") }
    cmake -S $root -B $Build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF @prefix
    Assert-Success "Configuring"
    cmake --build $Build
    Assert-Success "Building"
}

# Installing gathers the program, the libraries it uses, and Qt's plugin for Windows.
$stage = Join-Path $work "stage"
cmake --install $Build --prefix $stage
Assert-Success "Installing"

$version = (Select-String -Path (Join-Path $Build "CMakeCache.txt") `
    -Pattern '^CMAKE_PROJECT_VERSION:STATIC=(.+)$').Matches[0].Groups[1].Value
& $iscc /Q "/DAppVersion=$version" "/DStageDir=$stage" "/DOutputDir=$Output" `
    (Join-Path $root "packaging\windows\LCAD.iss")
Assert-Success "Inno Setup"

Write-Output "Built $(Join-Path $Output "LCAD-$version-windows-x64-setup.exe")"
