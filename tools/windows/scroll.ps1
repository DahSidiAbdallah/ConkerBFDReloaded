# Scroll the mouse wheel at a window-relative position: -X -Y -Clicks (negative = down)
param([string]$Proc = "ConkerBFDReloaded", [int]$X = 400, [int]$Y = 500, [int]$Clicks = -10)
Add-Type @"
using System; using System.Runtime.InteropServices;
public class S { [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
 [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, int data, UIntPtr extra);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; } }
"@
[S]::SetProcessDPIAware() | Out-Null
$p = Get-Process $Proc | Select-Object -First 1
$r = New-Object S+RECT; [S]::GetWindowRect($p.MainWindowHandle, [ref]$r) | Out-Null
[S]::SetCursorPos($r.L + $X, $r.T + $Y) | Out-Null
for ($i = 0; $i -lt [math]::Abs($Clicks); $i++) { [S]::mouse_event(0x0800, 0, 0, 120 * [math]::Sign($Clicks), [UIntPtr]::Zero); Start-Sleep -Milliseconds 50 }
"scrolled"
