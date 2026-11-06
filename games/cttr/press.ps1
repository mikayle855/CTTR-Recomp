# Drive a running CTTR started with PS2X_PADPIPE=<Pipe>: press keys in order, wait, then screenshot the game window.
#   powershell -ExecutionPolicy Bypass -File games\cttr\press.ps1 -Keys "down","cross" [-Gap 0.7] [-Wait 2] [-Out C:\CTTR_test\p.png]
# A key is "<button>[+<button>...][:<hold sec>]" (start select up down left right cross circle square triangle l1 r1 l2 r2 l3 r3).
param(
    [string[]]$Keys = @(),
    [double]$Gap = 0.7,
    [double]$Wait = 2,
    [string]$Out = "C:\CTTR_test\p.png",
    [string]$Pipe = "C:\CTTR_test\pad.txt"
)
foreach ($k in $Keys) {
    Add-Content -Path $Pipe -Value $k -Encoding ascii
    Start-Sleep -Milliseconds ([int]($Gap * 1000))
}
Start-Sleep -Milliseconds ([int]($Wait * 1000))
& (Join-Path $PSScriptRoot "shot.ps1") $Out
