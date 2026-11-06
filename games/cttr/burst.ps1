# Press keys through the pad pipe while grabbing the runner window every -Interval seconds, then save one contact sheet.
#   powershell -ExecutionPolicy Bypass -File games\cttr\burst.ps1 -Keys "cross:0.2@0","circle:0.3@0.3" [-Frames 12] [-Interval 0.1]
# A key is "<button>[+<button>...][:<hold>]@<seconds after start>".
param(
    [string[]]$Keys = @(),
    [int]$Frames = 12,
    [double]$Interval = 0.1,
    [string]$Out = "C:\CTTR_test\burst.png",
    [string]$Pipe = "C:\CTTR_test\pad.txt"
)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class CttrBurst {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
}
"@
$p = Get-Process ps2EntryRunner -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne [IntPtr]::Zero } | Select-Object -First 1
if (-not $p) { "runner window not found"; exit 1 }
$h = $p.MainWindowHandle
$r = New-Object CttrBurst+RECT
[void][CttrBurst]::GetClientRect($h, [ref]$r)
$w = [Math]::Max(1, $r.R - $r.L); $hgt = [Math]::Max(1, $r.B - $r.T)
$Keys = @($Keys | ForEach-Object { $_ -split "," } | Where-Object { $_ })   # -File passes an array as one "a,b" string
$pending = @($Keys | ForEach-Object { $k, $t = $_ -split "@", 2; [pscustomobject]@{ Key = $k; At = [double]$t } } | Sort-Object At)
$tw = 320; $th = [int]($hgt * $tw / $w); $cols = 4; $rows = [Math]::Ceiling($Frames / $cols)
$sheet = New-Object System.Drawing.Bitmap ($tw * $cols), ($th * $rows)
$sg = [System.Drawing.Graphics]::FromImage($sheet)
$font = New-Object System.Drawing.Font "Arial", 12
$sw = [Diagnostics.Stopwatch]::StartNew()
for ($i = 0; $i -lt $Frames; $i++) {
    $target = $i * $Interval
    while ($sw.Elapsed.TotalSeconds -lt $target) {
        $now = $sw.Elapsed.TotalSeconds
        while ($pending.Count -gt 0 -and $pending[0].At -le $now) { Add-Content -Path $Pipe -Value $pending[0].Key -Encoding ascii; $pending = @($pending | Select-Object -Skip 1) }
        Start-Sleep -Milliseconds 5
    }
    while ($pending.Count -gt 0 -and $pending[0].At -le $sw.Elapsed.TotalSeconds) { Add-Content -Path $Pipe -Value $pending[0].Key -Encoding ascii; $pending = @($pending | Select-Object -Skip 1) }
    $bmp = New-Object System.Drawing.Bitmap $w, $hgt
    $g = [System.Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc()
    [void][CttrBurst]::PrintWindow($h, $hdc, 3)
    $g.ReleaseHdc($hdc); $g.Dispose()
    $sg.DrawImage($bmp, ($i % $cols) * $tw, [Math]::Floor($i / $cols) * $th, $tw, $th)
    $sg.DrawString(("{0:N2}s" -f $sw.Elapsed.TotalSeconds), $font, [System.Drawing.Brushes]::Red, ($i % $cols) * $tw + 4, [Math]::Floor($i / $cols) * $th + 4)
    $bmp.Dispose()
}
$sg.Dispose(); $sheet.Save($Out); $sheet.Dispose()
"saved $Out"
