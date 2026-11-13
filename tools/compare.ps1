# Original vs native, one region stacked: compare.ps1 -Game g.png -Native n.png -Out o.png -X 0 -Y 30 -W 320 -H 60
#   g.png: the game's picture as dev/drive.sh shot saves it (320x200 at 2x = 640x400)
#   n.png: the presented window (dev/drive.sh wshot); its 4:3 picture is centred
#   X/Y/W/H: region in game pixels.  Output: original on top, native below,
#   each scaled to Scale x W wide.
#   The native picture is scaled bilinearly, which blurs thin UI strokes (MFD
#   text looks dimmer than it is: DEBUG_LOG 105); -Nearest samples it instead.
param([string]$Game, [string]$Native, [string]$Out, [int]$X = 0, [int]$Y = 0, [int]$W = 320, [int]$H = 200, [int]$Scale = 4,
      [switch]$Nearest)
Add-Type -AssemblyName System.Drawing
$g = [System.Drawing.Bitmap]::FromFile($Game)
$n = [System.Drawing.Bitmap]::FromFile($Native)
$gk = $g.Width / 320.0
$nh = $n.Height; $nw43 = $nh * 4.0 / 3.0; $nx0 = ($n.Width - $nw43) / 2.0
$kx = $nw43 / 320.0; $ky = $nh / 200.0
$ow = $W * $Scale; $oh = $H * $Scale
$o = New-Object System.Drawing.Bitmap $ow, (2 * $oh + 4)
$gr = [System.Drawing.Graphics]::FromImage($o)
$gr.Clear([System.Drawing.Color]::Magenta)
$gr.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$gr.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
$src1 = New-Object System.Drawing.RectangleF ($X * $gk), ($Y * $gk), ($W * $gk), ($H * $gk)
$gr.DrawImage($g, (New-Object System.Drawing.RectangleF 0, 0, $ow, $oh), $src1, [System.Drawing.GraphicsUnit]::Pixel)
if (-not $Nearest) { $gr.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBilinear }
$src2 = New-Object System.Drawing.RectangleF ($nx0 + $X * $kx), ($Y * $ky), ($W * $kx), ($H * $ky)
$gr.DrawImage($n, (New-Object System.Drawing.RectangleF 0, ($oh + 4), $ow, $oh), $src2, [System.Drawing.GraphicsUnit]::Pixel)
$o.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$gr.Dispose(); $o.Dispose(); $g.Dispose(); $n.Dispose()
