# Test-only UI driver for one Foundation test instance (window found by process id).
# ui.ps1 -GamePid <pid> -Action shot -Out <png>      screenshot of the game window only
# ui.ps1 -GamePid <pid> -Action click -X <x> -Y <y>  click at window-relative coordinates
# ui.ps1 -GamePid <pid> -Action key -Key <vk hex>    press one virtual key (e.g. 0x1B = Esc)
param([int]$GamePid, [string]$Action, [string]$Out, [int]$X, [int]$Y, [string]$Key, [int]$X2, [int]$Y2)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class U {
  [StructLayout(LayoutKind.Sequential)] public struct R { public int L, T, Rt, B; }
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint x, uint y, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern void keybd_event(byte v, byte s, uint f, UIntPtr e);
}
"@
[U]::SetProcessDPIAware() | Out-Null
$p = Get-Process -Id $GamePid -ErrorAction Stop
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { throw "no window for pid $GamePid" }
[U]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 300
$r = New-Object U+R; [U]::GetWindowRect($h, [ref]$r) | Out-Null
switch ($Action) {
  "shot" {
    $w = $r.Rt - $r.L; $hh = $r.B - $r.T
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
    $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
    "window $($r.L),$($r.T) ${w}x${hh} -> $Out"
  }
  "click" {
    [U]::SetCursorPos($r.L + $X, $r.T + $Y) | Out-Null; Start-Sleep -Milliseconds 150
    [U]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 60; [U]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
    "clicked $X,$Y"
  }
  "drag" {
    # left-drag from -X/-Y to -X2/-Y2 in small steps (zone painting)
    [U]::SetCursorPos($r.L + $X, $r.T + $Y) | Out-Null; Start-Sleep -Milliseconds 200
    [U]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 150
    for ($i = 1; $i -le 30; $i++) {
      [U]::SetCursorPos($r.L + $X + [int](($X2 - $X) * $i / 30), $r.T + $Y + [int](($Y2 - $Y) * $i / 30)) | Out-Null
      Start-Sleep -Milliseconds 40
    }
    Start-Sleep -Milliseconds 150; [U]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
    "dragged $X,$Y -> $X2,$Y2"
  }
  "scroll" {
    # -X/-Y: where to scroll; -Key: wheel notches (negative = toward the user = zoom out)
    [U]::SetCursorPos($r.L + $X, $r.T + $Y) | Out-Null; Start-Sleep -Milliseconds 100
    $n = [int]$Key
    $d = [math]::Sign($n) * 120; if ($d -lt 0) { $w = [uint32](4294967296 + $d) } else { $w = [uint32]$d }
    for ($i = 0; $i -lt [math]::Abs($n); $i++) { [U]::mouse_event(0x0800, 0, 0, $w, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80 }
    "scrolled $n at $X,$Y"
  }
  "key" {   # one key or a combination like 0x11+0x53 (Ctrl+S): pressed in order, released in reverse
    $vs = @($Key -split '\+' | ForEach-Object { [byte][Convert]::ToInt32($_, 16) })
    foreach ($v in $vs) { [U]::keybd_event($v, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 40 }
    Start-Sleep -Milliseconds 60
    [array]::Reverse($vs); foreach ($v in $vs) { [U]::keybd_event($v, 0, 2, [UIntPtr]::Zero); Start-Sleep -Milliseconds 20 }
    "key $Key" }
}
