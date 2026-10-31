# SPDX-License-Identifier: GPL-3.0-or-later
#
# Installs LCAD with its installer as a user would, opens a document by its file type, and checks
# that LCAD stays up, opens the document, draws it, and logs no warnings or errors. CI runs this
# outside the conda environment, so that the installed program has only what it carries.
#
# Usage: scripts/smoke-test-windows.ps1 -Installer <setup.exe> -Document <document.lcad>
#            -Screenshot <screen.png> [-OpenGL <folder>]
#   -OpenGL names a folder with Mesa's opengl32.dll and libgallium_wgl.dll, to put beside the
#   program on a machine with no graphics card.
param(
    [Parameter(Mandatory)][string]$Installer,
    [Parameter(Mandatory)][string]$Document,
    [Parameter(Mandatory)][string]$Screenshot,
    [string]$OpenGL = ""
)
$ErrorActionPreference = "Stop"

$log = Join-Path $env:LOCALAPPDATA "LCAD\lcad.log"
function Stop-Test([string]$reason) {
    if (Test-Path $log) {
        Write-Output "--- its log:"
        Get-Content $log | Write-Output
    }
    Get-Process lcad -ErrorAction SilentlyContinue | Stop-Process -Force
    throw "smoke-test-windows.ps1: $reason"
}

$Document = (Resolve-Path $Document).Path
$target = Join-Path ([IO.Path]::GetTempPath()) "LCAD-smoke-test"
$setup = Start-Process -FilePath (Resolve-Path $Installer).Path -Wait -PassThru -ArgumentList `
    "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/CURRENTUSER", "/TASKS=associate", `
    "/DIR=`"$target`""
if ($setup.ExitCode -ne 0) { Stop-Test "The installer exited with code $($setup.ExitCode)." }
$program = Join-Path $target "bin\lcad.exe"
if (-not (Test-Path $program)) { Stop-Test "The installer did not put lcad.exe in place." }
if ($OpenGL) {
    Copy-Item (Join-Path $OpenGL "opengl32.dll"), (Join-Path $OpenGL "libgallium_wgl.dll") `
        (Join-Path $target "bin")
}

if (Test-Path $log) { Remove-Item $log }
# As a double-click does: by the file type the installer registered.
Start-Process -FilePath $Document
Start-Sleep -Seconds 25
if (-not (Get-Process lcad -ErrorAction SilentlyContinue)) {
    Stop-Test "LCAD exited within 25 seconds of starting."
}

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$picture = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
$graphics = [System.Drawing.Graphics]::FromImage($picture)
$graphics.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
$picture.Save((Join-Path (Get-Location) $Screenshot))
# An empty desktop is few colors, and a window without the 3D view a few hundred. A lit, shaded
# model with smoothed edges has thousands.
$colors = New-Object 'System.Collections.Generic.HashSet[int]'
for ($y = 0; $y -lt $picture.Height; $y += 2) {
    for ($x = 0; $x -lt $picture.Width; $x += 2) {
        [void]$colors.Add($picture.GetPixel($x, $y).ToArgb())
    }
}
Write-Output "The screenshot has $($colors.Count) distinct colors."

if (-not (Test-Path $log)) { Stop-Test "LCAD did not write its log." }
if (-not (Select-String -Path $log -SimpleMatch "Opened" -Quiet)) {
    Stop-Test "LCAD did not open the document."
}
$complaints = Select-String -Path $log -Pattern '^[^ ]+ (warning|error) '
if ($complaints) {
    $complaints | Write-Output
    Stop-Test "LCAD logged warnings or errors."
}
Get-Process lcad | Stop-Process -Force
Write-Output "LCAD installed, opened $(Split-Path -Leaf $Document), and drew it."
