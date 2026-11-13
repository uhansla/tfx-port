# Crop a region of an image: crop.ps1 -In a.png -Out b.png -X 0 -Y 0 -W 100 -H 100
param([string]$In, [string]$Out, [int]$X, [int]$Y, [int]$W, [int]$H)
Add-Type -AssemblyName System.Drawing
$b = [System.Drawing.Bitmap]::FromFile($In)
$r = New-Object System.Drawing.Rectangle $X, $Y, $W, $H
$c = $b.Clone($r, $b.PixelFormat)
$c.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$b.Dispose(); $c.Dispose()
