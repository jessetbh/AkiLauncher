# Package the launcher for release: dist\AkiLauncher-<Version>-Windows.zip
# + SHA256SUMS (same convention as the game recomps' releases).
#
#   . ..\WcwNwoWorldTour\tools\env-msvc.ps1   # if cmake isn't on PATH yet
#   .\tools\package.ps1 -Version v0.1.0 [-IncludeArt] [-SkipBuild]
#
# Layout inside the zip (flat, like the game zips):
#   AkiLauncher.exe          self-contained (static CRT)
#   assets\boxart\           box/cart art (shipped - owner decision 2026-07-16)
#   assets\sounds\           optional replacement menu sounds
#   COPYING README.txt
# ROMs are NEVER packaged - a hard assertion below fails the build if any
# ROM-like file ends up in the stage.

param(
    [string]$Version = "dev",
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
Copy-Item (Join-Path $repo "assets\boxart\*.png") "$stage\assets\boxart" -ErrorAction SilentlyContinue
Copy-Item (Join-Path $repo "assets\boxart\*.jpg") "$stage\assets\boxart" -ErrorAction SilentlyContinue
Copy-Item (Join-Path $repo "COPYING") $stage -ErrorAction SilentlyContinue
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
  F / X             flip the box art (front / back)
  U / RB            install an available update
  S / Y             settings (rebind quick-back, per-game paths)
  F11 / Alt+Enter   windowed / fullscreen
  Esc               quit
  In game           Shift+F12 or hold View+Menu ~0.6s to return here

Notes
  - Box art ships in assets\boxart\ - replace with your own scans anytime.
  - Custom menu sounds: assets\sounds\{nav,flip,launch}.wav.
  - Settings and play stats live in %LOCALAPPDATA%\AkiLauncher\settings.ini.
"@ | Set-Content (Join-Path $stage "README.txt") -Encoding utf8

# Hard gate: no ROMs or ROM-derived data in a release, ever.
$strays = Get-ChildItem $stage -Recurse -Include *.z64, *.n64, *.v64, *.rom, *.bin
if ($strays) { throw "ROM-like files in the release stage: $($strays.FullName -join ', ')" }

$zipName = "AkiLauncher-$Version-Windows.zip"
$zipPath = Join-Path $dist $zipName
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
Compress-Archive -Path "$stage\*" -DestinationPath $zipPath

$hash = (Get-FileHash -Algorithm SHA256 $zipPath).Hash.ToLower()
"$hash  $zipName" | Set-Content (Join-Path $dist "SHA256SUMS") -Encoding ascii

Remove-Item -Recurse -Force $stage
Write-Host "packaged: $zipPath"
Write-Host "sha256:   $hash"
