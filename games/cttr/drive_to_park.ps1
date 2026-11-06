# Drive a CTTR run (started with PS2X_PADPIPE=<Pipe>, save present in slot 1) from boot into the park, then wander.
#   powershell -ExecutionPolicy Bypass -File games\cttr\drive_to_park.ps1 [-WanderSeconds 180] [-ShotDir C:\CTTR_test\drive]
# Boot -> logos/load notice -> title -> Resume Game -> Enter the Park -> YES -> (story intro) -> hints -> random walk.
param(
    [int]$WanderSeconds = 180,
    [string]$Pipe = "C:\CTTR_test\pad.txt",
    [string]$ShotDir = ""
)
function Press([string]$k, [double]$after = 1.0) {
    Add-Content -Path $Pipe -Value $k -Encoding ascii
    Start-Sleep -Milliseconds ([int]($after * 1000))
}
function Shot([string]$name) {
    if ($ShotDir) { & (Join-Path $PSScriptRoot "shot.ps1") (Join-Path $ShotDir "$name.png") | Out-Null }
}
if ($ShotDir) { New-Item -ItemType Directory -Force $ShotDir | Out-Null }
Start-Sleep 40
# logos, "Load Successful" OK, intro movie, title: start/cross alternately
for ($i = 0; $i -lt 16; $i++) { Press "start:0.25" 2; Press "cross:0.25" 2 }
Shot "menu"
# whatever menu we are on, back out to the title, then go forward deterministically
Press "triangle:0.25" 3; Press "triangle:0.25" 3
Press "start:0.25" 4            # title -> New/Resume/Load (Resume highlighted)
Press "cross:0.25" 4            # Resume Game -> (load notice)
Press "cross:0.25" 6            # -> Single Player menu, Enter the Park highlighted (waits are real time: before
                                # [eetimerirq] the game ran ~5.7x fast and the old waits were tuned to that)
Press "cross:0.3" 4             # Enter the Park -> dialog
Press "right:0.3" 3             # YES
Press "cross:0.3" 45            # load
Shot "afterload"
for ($i = 0; $i -lt 8; $i++) { Press "start:0.3" 12 }   # story intro (when unseen) / tutorial
for ($i = 0; $i -lt 4; $i++) { Press "cross:0.25" 2 }
Shot "park"
$dirs = @("up", "left", "up", "right", "down", "up", "cross", "up")
$r = New-Object System.Random 11
$t0 = Get-Date
while (((Get-Date) - $t0).TotalSeconds -lt $WanderSeconds) {
    Press ("{0}:{1}" -f $dirs[$r.Next($dirs.Count)], [Math]::Round(0.5 + $r.NextDouble() * 2.5, 1)) 3.2
}
Shot "end"
