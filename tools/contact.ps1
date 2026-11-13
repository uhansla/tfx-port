# Tile PNG/BMP screenshots into one contact sheet with labels.
# usage: contact.ps1 -Out sheet.png -Cols 4 -W 320 file1 file2 ...
param([string]$Out, [int]$Cols = 4, [int]$W = 320, [Parameter(ValueFromRemainingArguments = $true)][string[]]$Files)
Add-Type -AssemblyName System.Drawing
$H = [int]($W * 0.625) + 14
$rows = [math]::Ceiling($Files.Count / $Cols)
$sheet = New-Object System.Drawing.Bitmap ($W * $Cols), ($H * $rows)
$g = [System.Drawing.Graphics]::FromImage($sheet)
$g.Clear([System.Drawing.Color]::Black)
$font = New-Object System.Drawing.Font 'Consolas', 9
for ($i = 0; $i -lt $Files.Count; $i++) {
    $img = [System.Drawing.Image]::FromFile((Resolve-Path $Files[$i]))
    $x = ($i % $Cols) * $W; $y = [math]::Floor($i / $Cols) * $H
    $g.DrawImage($img, $x, $y + 14, $W, $H - 14)
    $g.DrawString([System.IO.Path]::GetFileNameWithoutExtension($Files[$i]), $font, [System.Drawing.Brushes]::Yellow, $x + 2, $y)
    $img.Dispose()
}
$sheet.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $sheet.Dispose()
