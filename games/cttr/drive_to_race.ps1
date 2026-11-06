# Drive a CTTR run (PS2X_PADPIPE=<Pipe>, save in slot 1) from boot to a 1-player Tiki Turbo race via Multiplayer.
#   powershell -ExecutionPolicy Bypass -File games\cttr\drive_to_race.ps1 [-ShotDir C:\CTTR_test\race]
# Boot -> Single Player list (Start x10) -> Back to the signpost -> Multiplayer -> Split Screen -> Race -> Tiki Turbo ->
# 3 laps -> Easy -> Normal -> join -> use name -> ready -> Crash -> car -> YES.
param(
    [string]$Pipe = "C:\CTTR_test\pad.txt",
    [string]$ShotDir = ""
)
function Press([string]$k, [double]$after = 3.0) {
    Add-Content -Path $Pipe -Value $k -Encoding ascii
    Start-Sleep -Milliseconds ([int]($after * 1000))
}
function Shot([string]$name) {
    if ($ShotDir) { & (Join-Path $PSScriptRoot "shot.ps1") (Join-Path $ShotDir "$name.png") | Out-Null }
}
if ($ShotDir) { New-Item -ItemType Directory -Force $ShotDir | Out-Null }
Start-Sleep 50
for ($i = 0; $i -lt 10; $i++) { Press "start:0.2" 3 }
Shot "singleplayer"
Press "triangle:0.25"; Press "right:0.3"; Shot "signpost"
foreach ($k in "cross:0.25", "cross:0.25", "cross:0.25", "cross:0.25", "cross:0.25", "cross:0.25", "cross:0.25", "cross:0.25", "cross:0.25", "cross:0.25", "cross:0.25", "cross:0.25") { Press $k }
Shot "proceed"
Press "right:0.3" 2; Press "cross:0.3" 5
Shot "loading"
