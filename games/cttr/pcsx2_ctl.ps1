# Drive a running PCSX2 (reference runs for CTTR comparisons) without touching the focused window: key presses are
# posted to PCSX2's own window handles (WM_KEYDOWN/WM_KEYUP), screenshots grab its render window.
#   powershell -ExecutionPolicy Bypass -File games\cttr\pcsx2_ctl.ps1 -Keys "Return:0.2","K:0.3" [-Gap 0.7] [-Wait 2] [-Out C:\CTTR_test\ref.png]
# Keys use .NET Keys names (Return, K = Cross, L = Circle, J = Square, I = Triangle, Up/Down/Left/Right); "A+B:hold" holds both.
param(
    [string[]]$Keys = @(),
    [double]$Gap = 0.7,
    [double]$Wait = 1.5,
    [string]$Out = "",
    [switch]$Focus   # bring PCSX2's window to the front first: Qt ignores posted keys while it is in the background
)
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class P2Ctl {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    public static List<IntPtr> Windows(uint pid) {
        var all = new List<IntPtr>();
        EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid && IsWindowVisible(h)) { all.Add(h); EnumChildWindows(h, (c, l2) => { all.Add(c); return true; }, IntPtr.Zero); } return true; }, IntPtr.Zero);
        return all;
    }
}
"@
$p = Get-Process pcsx2-qt -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { "pcsx2 not running"; exit 1 }
$wins = [P2Ctl]::Windows([uint32]$p.Id)
if ($Focus -and $wins.Count) { [void][P2Ctl]::SetForegroundWindow($wins[0]); Start-Sleep -Milliseconds 300 }
function Post([int]$vk, [bool]$down) {
    $scan = [P2Ctl]::MapVirtualKey([uint32]$vk, 0)
    $ext = if (@(0x25, 0x26, 0x27, 0x28) -contains $vk) { 1 } else { 0 }
    $lp = 1 -bor ($scan -shl 16) -bor ($ext -shl 24)
    if (-not $down) { $lp = $lp -bor (3 -shl 30) }
    foreach ($w in $wins) { [void][P2Ctl]::PostMessage($w, $(if ($down) { 0x100 } else { 0x101 }), [IntPtr]$vk, [IntPtr]$lp) }
}
foreach ($k in ($Keys | ForEach-Object { $_ -split "," } | Where-Object { $_ })) {
    $name, $hold = $k -split ":", 2
    $hold = if ($hold) { [double]$hold } else { 0.15 }
    $vks = $name -split "\+" | ForEach-Object { [int][System.Windows.Forms.Keys]$_ }
    foreach ($vk in $vks) { Post $vk $true }
    Start-Sleep -Milliseconds ([int]($hold * 1000))
    foreach ($vk in $vks) { Post $vk $false }
    Start-Sleep -Milliseconds ([int]([Math]::Max(0, $Gap - $hold) * 1000))
}
Start-Sleep -Milliseconds ([int]($Wait * 1000))
if ($Out) {
    # the largest child window is the render surface
    $best = [IntPtr]::Zero; $area = 0
    foreach ($w in $wins) { $r = New-Object P2Ctl+RECT; [void][P2Ctl]::GetClientRect($w, [ref]$r); $a = ($r.R - $r.L) * ($r.B - $r.T); if ($a -gt $area) { $area = $a; $best = $w } }
    $r = New-Object P2Ctl+RECT; [void][P2Ctl]::GetClientRect($best, [ref]$r)
    $bmp = New-Object System.Drawing.Bitmap ([Math]::Max(1, $r.R)), ([Math]::Max(1, $r.B))
    $g = [System.Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc()
    [void][P2Ctl]::PrintWindow($best, $hdc, 2); $g.ReleaseHdc($hdc); $g.Dispose()
    $bmp.Save($Out); $bmp.Dispose(); "saved $Out"
}
