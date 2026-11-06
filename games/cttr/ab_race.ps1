# A/B the race start: for each variant (extra env), boot, drive to Tiki Turbo, screenshot the start line.
#   powershell -ExecutionPolicy Bypass -File games\cttr\ab_race.ps1 -Variants "base=","nomac=PS2X_MACPIPE=0" [-OutDir C:\CTTR_test\ab]
param([string[]]$Variants = @("base="), [string]$OutDir = "C:\CTTR_test\ab")
New-Item -ItemType Directory -Force $OutDir | Out-Null
foreach ($v in ($Variants | ForEach-Object { $_ -split "," } | Where-Object { $_ })) {
    $name, $extra = $v -split "=", 2
    Get-Process ps2EntryRunner -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 2
    Set-Content C:\CTTR_test\pad.txt "" -Encoding ascii
    $envs = "PS2X_GPU=1;PS2X_PADPIPE=C:\CTTR_test\pad.txt" + $(if ($extra) { ";" + ($extra -replace "\+", ";") } else { "" })
    Start-Process powershell -ArgumentList "-ExecutionPolicy", "Bypass", "-File", (Join-Path $PSScriptRoot "run.ps1"), "-Seconds", "330", "-Log", (Join-Path $OutDir "$name.log"), "-KeepMB", "20", "-Env", "`"$envs`"" -WindowStyle Hidden
    & (Join-Path $PSScriptRoot "drive_to_race.ps1")
    Start-Sleep 45
    & (Join-Path $PSScriptRoot "shot.ps1") (Join-Path $OutDir "$name.png") | Out-Null
    Get-Process ps2EntryRunner -ErrorAction SilentlyContinue | Stop-Process -Force
    "$name done"
}
