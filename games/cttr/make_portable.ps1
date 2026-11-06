# [portable] Builds a self-contained Crash Tag Team Racing folder from the current build (re-run after each build).
#   powershell -ExecutionPolicy Bypass -File games\cttr\make_portable.ps1 [-Out <folder>]   (default: <repo>\dist\CTTR)
# Contents: CTTR.exe (the launcher, portable mode), runtime\ (game program + DLLs + .pdb for crash reports),
# assets\, README.txt. NOT included: the disc image, the game file (data\SLES_534.39 is copied from the player's own
# disc on first start) and any save. An existing savedata\, data\, disc.txt, save_backups\ and crashes\ in -Out are kept.
param([string]$Out = "")
$Repo = (Resolve-Path "$PSScriptRoot\..\..").Path
if (-not $Out) { $Out = "$Repo\dist\CTTR" }
$ErrorActionPreference = "Stop"
$build = "$Repo\build\ps2xRuntime"
$launcher = "$PSScriptRoot\launcher\CTTR.exe"   # build it with launcher\build_launcher.bat
foreach ($f in "$build\ps2EntryRunner.exe", $launcher) { if (-not (Test-Path $f)) { throw "missing: $f" } }
New-Item -ItemType Directory -Force "$Out\runtime", "$Out\assets" | Out-Null
Copy-Item "$build\ps2EntryRunner.exe", "$build\ps2EntryRunner.pdb" "$Out\runtime\" -Force
Get-ChildItem $build -Filter *.dll | Copy-Item -Destination "$Out\runtime\" -Force
Copy-Item "$build\assets\*" "$Out\assets\" -Recurse -Force
Copy-Item $launcher "$Out\CTTR.exe" -Force
@"
Crash Tag Team Racing - PC port
===============================

Start the game with CTTR.exe.

First start
  You need your own copy of the game: the PAL disc (SLES-53439) as an .iso file.
  The first time, CTTR.exe asks you to choose it (or put the .iso in this folder and it is found by itself).
  The choice is remembered in disc.txt. The game file is then copied from your disc into the data folder.

In the game
  F1 (or Shift+Tab, or Select+Start on a pad)   settings: resolution, picture, sound, controls, cheats, help
  F11   full screen / window          F8    minimise
  F10   performance readout           F12   screenshot (saved in Pictures\Crash Tag Team Racing)
  The game pauses by itself while you use another window (F1 > Display > Window to turn that off).

Your files (all in this folder)
  savedata\       memory card (your save), settings, controller bindings
  save_backups\   a copy of your save from each start (newest 10)
  crashes\        crash reports, if the game ever closes unexpectedly (CTTR.exe tells you)
  play.log        the log of the last session

To move the game, move this whole folder. To keep your save when updating, keep savedata\.
"@ | Set-Content -Path "$Out\README.txt" -Encoding UTF8
$size = (Get-ChildItem $Out -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
"Portable folder ready: $Out ({0:N0} MB)" -f $size
