# Package the launcher for release: dist\AkiLauncher-<Version>-Windows.zip
# + SHA256SUMS (same convention as the game recomps' releases).
#
#   . ..\WcwNwoWorldTour\tools\env-msvc.ps1   # if cmake isn't on PATH yet
#   .\tools\package.ps1 -Version v0.1.0 [-IncludeArt] [-SkipBuild]
#
# Layout inside the zip (flat, like the game zips):
#   AkiLauncher.exe          self-contained (static CRT)
#   assets\boxart\README.md  art goes here; scans NOT included by default
#   README.txt
# -IncludeArt adds the local boxart scans - they are copyrighted, so only use
# it for private/testing builds.

param(
    [string]$Version = "dev",
    [switch]$IncludeArt,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
$build = Join-Path $repo "build-msvc"
$dist = Join-Path $repo "dist"
$stage = Join-Path $dist "stage"

if (-not $SkipBuild) {
    if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
        . (Join-Path $repo "..\WcwNwoWorldTour\tools\env-msvc.ps1")
    }
    cmake --build $build
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

$exe = Join-Path $build "AkiLauncher.exe"
if (-not (Test-Path $exe)) { throw "missing $exe" }

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force "$stage\assets\boxart" | Out-Null

Copy-Item $exe $stage
Copy-Item (Join-Path $repo "assets\boxart\README.md") "$stage\assets\boxart"
if ($IncludeArt) {
    Copy-Item (Join-Path $repo "assets\boxart\*.png") "$stage\assets\boxart" -ErrorAction SilentlyContinue
    Copy-Item (Join-Path $repo "assets\boxart\*.jpg") "$stage\assets\boxart" -ErrorAction SilentlyContinue
}
if (Test-Path (Join-Path $repo "assets\sounds")) {
    New-Item -ItemType Directory -Force "$stage\assets\sounds" | Out-Null
    Copy-Item (Join-Path $repo "assets\sounds\*.wav") "$stage\assets\sounds" -ErrorAction SilentlyContinue
}

@"
AKI Launcher $Version
=====================

A hub for the AKI-engine N64 wrestling recompilations.

Getting started
  1. Run AkiLauncher.exe.
  2. Pick a game and press Enter / A: games marked "Not installed" download
     directly from their GitHub releases into the games\ folder next to this
     exe (SHA256-verified).
  3. When asked, select your own legally dumped ROM (.z64 big-endian). The
     launcher checks the hash and keeps a copy with the game. No ROMs or game
     data are distributed with this launcher or the game downloads' releases.

Controls
  Left / Right      browse games
  Enter / A         play / download / select ROM
  F / X             flip the box (front / back / cartridge)
  U / RB            install an available update
  S / Y             settings (rebind quick-back, per-game paths)
  Esc               quit
  In game           Shift+F12 or hold View+Menu ~0.6s to return here

Notes
  - Box art: drop scans into assets\boxart\ (see the README there).
  - Custom menu sounds: assets\sounds\{nav,flip,launch}.wav.
  - Settings and play stats live in %LOCALAPPDATA%\AkiLauncher\settings.ini.
"@ | Set-Content (Join-Path $stage "README.txt") -Encoding utf8

$zipName = "AkiLauncher-$Version-Windows.zip"
$zipPath = Join-Path $dist $zipName
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
Compress-Archive -Path "$stage\*" -DestinationPath $zipPath

$hash = (Get-FileHash -Algorithm SHA256 $zipPath).Hash.ToLower()
"$hash  $zipName" | Set-Content (Join-Path $dist "SHA256SUMS") -Encoding ascii

Remove-Item -Recurse -Force $stage
Write-Host "packaged: $zipPath"
Write-Host "sha256:   $hash"
