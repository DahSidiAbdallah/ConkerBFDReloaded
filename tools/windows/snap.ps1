# Capture the game window to PNG files at the given moments.
# Wall-clock mode: seconds since start. -Log mode: waits for "[snap] N" lines the game
# prints at those game seconds (it holds still until snaps\tN.png exists).
param([string]$Out = "C:\ConkerRecompWin\snaps", [string]$AtList = "6,10,14,20", [string]$Proc = "boot_test", [string]$Log = "")
$At = $AtList.Split(",")
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W { [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string t);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; } }
"@
[W]::SetProcessDPIAware() | Out-Null
New-Item -ItemType Directory -Force -Path $Out | Out-Null
Get-ChildItem $Out -Filter *.png | Remove-Item
$start = Get-Date
function Snap($t) {
  $p = Get-Process $Proc -ErrorAction SilentlyContinue | Select-Object -First 1; $h = if ($p) { $p.MainWindowHandle } else { [IntPtr]::Zero }
  if ($h -eq [IntPtr]::Zero) { "t=$t no window"; return }
  $r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) | Out-Null
  $w = $r.R - $r.L; $hh = $r.B - $r.T
  $bmp = New-Object System.Drawing.Bitmap $w, $hh
  $g = [System.Drawing.Graphics]::FromImage($bmp); [W]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x13) | Out-Null; Start-Sleep -Milliseconds 400
  $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
  [W]::SetWindowPos($h, [IntPtr](-2), 0, 0, 0, 0, 0x13) | Out-Null
  $bmp.Save("$Out\t$t.tmp.png"); $g.Dispose(); $bmp.Dispose()
  Move-Item -Force "$Out\t$t.tmp.png" "$Out\t$t.png"; "t=$t saved"
}
foreach ($t in $At) {
  if ($Log -ne "") {
    $deadline = (Get-Date).AddSeconds(300)
    while ((Get-Date) -lt $deadline) {
      $hit = Select-String -Path $Log -Pattern ("^\[snap\] " + [regex]::Escape($t) + "$") -Quiet -ErrorAction SilentlyContinue
      if ($hit) { break }
      Start-Sleep -Milliseconds 100
    }
  } else {
    while (((Get-Date) - $start).TotalSeconds -lt $t) { Start-Sleep -Milliseconds 100 }
  }
  Snap $t
}
