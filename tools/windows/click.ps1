# Click at window-relative positions: -Points "x,y;x,y" with -Delays "sec;sec" (seconds since start).
# A point "ESC" presses Escape instead (the in-game settings menu).
param([string]$Proc = "ConkerBFDReloaded", [string]$Points, [string]$Delays)
Add-Type @"
using System; using System.Runtime.InteropServices;
public class M { [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
 [DllImport("user32.dll")] public static extern void mouse_event(int f, int x, int y, int d, int e);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; } }
"@
[M]::SetProcessDPIAware() | Out-Null
$pts = $Points.Split(";"); $ds = $Delays.Split(";"); $start = Get-Date
for ($i = 0; $i -lt $pts.Length; $i++) {
  while (((Get-Date) - $start).TotalSeconds -lt [double]$ds[$i]) { Start-Sleep -Milliseconds 100 }
  $p = Get-Process $Proc -ErrorAction SilentlyContinue | Select-Object -First 1
  if (-not $p) { "no window"; continue }
  $h = $p.MainWindowHandle; $r = New-Object M+RECT; [M]::GetWindowRect($h, [ref]$r) | Out-Null
  [M]::SetForegroundWindow($h) | Out-Null
  if ($pts[$i] -eq "ESC") {
    # Click the window's middle first so it has the keyboard, then press Escape.
    [M]::SetCursorPos(($r.L + $r.R) / 2, ($r.T + $r.B) / 2) | Out-Null; Start-Sleep -Milliseconds 100
    [M]::mouse_event(2,0,0,0,0); Start-Sleep -Milliseconds 60; [M]::mouse_event(4,0,0,0,0); Start-Sleep -Milliseconds 300
    Add-Type -AssemblyName System.Windows.Forms; [System.Windows.Forms.SendKeys]::SendWait("{ESC}"); "pressed ESC"; continue
  }
  $xy = $pts[$i].Split(","); [M]::SetCursorPos($r.L + [int]$xy[0], $r.T + [int]$xy[1]) | Out-Null
  Start-Sleep -Milliseconds 150; [M]::mouse_event(2,0,0,0,0); Start-Sleep -Milliseconds 60; [M]::mouse_event(4,0,0,0,0)
  "clicked $($pts[$i])"
}
