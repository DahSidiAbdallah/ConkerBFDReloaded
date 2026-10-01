# Combine all snaps\t*.png into snaps\sheet.png (4 per row, half size, labelled).
param([string]$Dir = "C:\ConkerRecompWin\snaps", [string]$Filter = "t*.png")
Add-Type -AssemblyName System.Drawing
$files = Get-ChildItem $Dir -Filter $Filter | Sort-Object { [double](($_.BaseName -replace '[^0-9.]', '')) }
$w = 400; $h = 300; $cols = 4; $rows = [math]::Ceiling($files.Count / $cols)
$sheet = New-Object System.Drawing.Bitmap ($w * $cols), ($h * $rows)
$g = [System.Drawing.Graphics]::FromImage($sheet); $g.Clear([System.Drawing.Color]::DimGray)
$font = New-Object System.Drawing.Font "Arial", 14
for ($i = 0; $i -lt $files.Count; $i++) {
  $img = [System.Drawing.Image]::FromFile($files[$i].FullName)
  $x = ($i % $cols) * $w; $y = [math]::Floor($i / $cols) * $h
  $g.DrawImage($img, $x, $y, $w, $h); $img.Dispose()
  $g.DrawString($files[$i].BaseName, $font, [System.Drawing.Brushes]::Yellow, $x + 4, $y + 4)
}
$sheet.Save("$Dir\sheet.png"); $g.Dispose(); $sheet.Dispose()
