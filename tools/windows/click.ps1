# Click at window-relative positions: -Points "x,y;x,y" with -Delays "sec;sec" (seconds since start)
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
  $xy = $pts[$i].Split(","); [M]::SetCursorPos($r.L + [int]$xy[0], $r.T + [int]$xy[1]) | Out-Null
  Start-Sleep -Milliseconds 150; [M]::mouse_event(2,0,0,0,0); Start-Sleep -Milliseconds 60; [M]::mouse_event(4,0,0,0,0)
  "clicked $($pts[$i])"
}
