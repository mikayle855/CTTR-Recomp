# Test-run the CTTR runner for a fixed time and keep the head of its log.
#   powershell -ExecutionPolicy Bypass -File games\cttr\run.ps1 [-Seconds 30] [-Log C:\CTTR_test\run.log] [-Env "PS2X_A=1;PS2X_B=2"]
# Expects the disc contents in $Data (with SLES_534.39) and saves in its sibling savedata\ (see README.md).
param(
    [int]$Seconds = 30,
    [string]$Data = "C:\CTTR_test\data",
    [Parameter(Mandatory = $true)][string]$Iso,   # your Crash Tag Team Racing (PAL, SLES-53439) disc image
    [string]$Log = "C:\CTTR_test\run.log",
    [string]$Env = "",
    [int]$KeepMB = 4
)
$ROOT = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$runnerDir = Join-Path $ROOT "build\ps2xRuntime"
$vars = @{ PS2X_EXEDIR = (Split-Path $Data -Parent); PS2X_NODEFAULTS = "1"; PS2X_CD_IMAGE = $Iso }
foreach ($kv in ($Env -split ";" | Where-Object { $_ -match "=" })) { $k, $v = $kv -split "=", 2; $vars[$k.Trim()] = $v }
$sets = ($vars.GetEnumerator() | ForEach-Object { "set `"$($_.Key)=$($_.Value)`"" }) -join "&& "
$raw = "$Log.full"
$cmd = "$sets&& `"$runnerDir\ps2EntryRunner.exe`" `"$Data\SLES_534.39`" > `"$raw`" 2>&1"
$p = Start-Process cmd.exe -ArgumentList "/c", $cmd -WorkingDirectory $runnerDir -PassThru -WindowStyle Hidden
if ($p.WaitForExit($Seconds * 1000)) { "runner exited by itself after <$Seconds s (cmd exit $($p.ExitCode))" }
else {
    # stop only the runner this script started (child of its cmd.exe), so overlapping test runs do not kill each other
    Get-CimInstance Win32_Process -Filter "ParentProcessId=$($p.Id) AND Name='ps2EntryRunner.exe'" -ErrorAction SilentlyContinue |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    $null = $p.WaitForExit(5000); "stopped after $Seconds s"
}
# Keep only the head of the log: a stuck guest can print hundreds of MB in seconds.
$fs = [IO.File]::OpenRead($raw)
$buf = New-Object byte[] ([Math]::Min($fs.Length, $KeepMB * 1MB))
$null = $fs.Read($buf, 0, $buf.Length); $full = $fs.Length; $fs.Close()
[IO.File]::WriteAllBytes($Log, $buf); Remove-Item $raw
"log: $Log ($([Math]::Round($full / 1MB, 1)) MB written, first $KeepMB MB kept)"
