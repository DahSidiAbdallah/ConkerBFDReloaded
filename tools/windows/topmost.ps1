# Keep the game window on top for -Seconds, printing its screen rectangle (x y w h).
param([string]$Proc = "ConkerBFDReloaded", [int]$Seconds = 15, [string]$RectFile = "")
Add-Type @"
using System; using System.Runtime.InteropServices;
public class T { [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; } }
"@
[T]::SetProcessDPIAware() | Out-Null
$p = Get-Process $Proc | Select-Object -First 1; $h = $p.MainWindowHandle
[T]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x13) | Out-Null
$r = New-Object T+RECT; [T]::GetWindowRect($h, [ref]$r) | Out-Null
$rect = "$($r.L + 8) $($r.T + 31) $((($r.R - $r.L) - 16) -band -2) $((($r.B - $r.T) - 39) -band -2)"
# (inside: without the window frame and title bar)
if ($RectFile -ne "") { Set-Content -Path $RectFile -Value $rect } else { $rect }
Start-Sleep -Seconds $Seconds
[T]::SetWindowPos($h, [IntPtr](-2), 0, 0, 0, 0, 0x13) | Out-Null
