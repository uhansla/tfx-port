# Convert a BMP screenshot to PNG, scaled by -Scale (default 2) with nearest-neighbour.
param([string]$In, [string]$Out, [int]$Scale = 2)
Add-Type -AssemblyName System.Drawing
$b = [System.Drawing.Bitmap]::FromFile($In)
$s = New-Object System.Drawing.Bitmap ($b.Width * $Scale), ($b.Height * $Scale)
$g = [System.Drawing.Graphics]::FromImage($s)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
$g.DrawImage($b, 0, 0, $b.Width * $Scale, $b.Height * $Scale)
$s.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $b.Dispose(); $s.Dispose()
