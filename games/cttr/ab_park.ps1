# A/B the park start: for each variant (extra env), boot, Resume Game -> Enter the Park -> YES, wait out the story movie,
# screenshot the start, walk forward (the first coins are just ahead) and screenshot again.
#   powershell -ExecutionPolicy Bypass -File games\cttr\ab_park.ps1 -Variants "base=;sw=PS2X_GPU=0" [-OutDir C:\CTTR_test\abpark]
# Variants are separated by ";" (a value may hold commas, e.g. a PS2X_FNLOG list); "+" joins env vars within one variant.
# Needs the save in slot 1. Taps shorter than ~0.5 s are often lost on the dialog, so every press is held.
param([string[]]$Variants = @("base="), [string]$OutDir = "C:\CTTR_test\abpark", [int]$Seconds = 330)
New-Item -ItemType Directory -Force $OutDir | Out-Null
function P([string]$k, [double]$after) {
    Add-Content C:\CTTR_test\pad.txt $k -Encoding ascii
    Start-Sleep -Milliseconds ([int]($after * 1000))
}
foreach ($v in ($Variants | ForEach-Object { $_ -split ";" } | Where-Object { $_ })) {
    $name, $extra = $v -split "=", 2
    Get-Process ps2EntryRunner -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 2
    Set-Content C:\CTTR_test\pad.txt "" -Encoding ascii
    $envs = "PS2X_GPU=1;PS2X_PADPIPE=C:\CTTR_test\pad.txt" + $(if ($extra) { ";" + ($extra -replace "\+", ";") } else { "" })
    Start-Process powershell -ArgumentList "-ExecutionPolicy", "Bypass", "-File", (Join-Path $PSScriptRoot "run.ps1"), "-Seconds", "$Seconds", "-Log", (Join-Path $OutDir "$name.log"), "-KeepMB", "20", "-Env", "`"$envs`"" -WindowStyle Hidden
    Start-Sleep 40
    for ($i = 0; $i -lt 16; $i++) { P "start:0.4" 2; P "cross:0.4" 2 }   # logos, title, Resume Game, Enter the Park
    P "right:0.6" 2; P "cross:0.5" 50                                   # YES, load, story movie
    P "cross:0.5" 30; P "cross:0.5" 30
    & (Join-Path $PSScriptRoot "shot.ps1") (Join-Path $OutDir "$name.png") | Out-Null
    P "up:3" 4
    & (Join-Path $PSScriptRoot "shot.ps1") (Join-Path $OutDir "${name}_walk.png") | Out-Null
    Get-Process ps2EntryRunner -ErrorAction SilentlyContinue | Stop-Process -Force
    "$name done"
}
