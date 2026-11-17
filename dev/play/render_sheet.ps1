# Zoomed comparisons of the shots from render_shots.sh: one region of each
# shot, scaled up with whole pixels and labelled, side by side.
#   render_sheet.ps1 [-Dir work\render-options]
#   -> zoom-filters.png (cockpit filters: MFDs and radar) and
#      zoom-upscalers.png (upscalers: the aircraft in the outside view)
param([string]$Dir = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'work\render-options'))
Add-Type -AssemblyName System.Drawing

function Sheet([string]$out, [int]$x, [int]$y, [int]$w, [int]$h, [int]$scale, [int]$cols, $items) {
    $tw = $w * $scale; $th = $h * $scale; $lab = 30
    $rows = [math]::Ceiling($items.Count / $cols)
    $sheet = New-Object System.Drawing.Bitmap ($tw * $cols + 8 * ($cols - 1)), (($th + $lab) * $rows + 8 * ($rows - 1))
    $g = [System.Drawing.Graphics]::FromImage($sheet)
    $g.Clear([System.Drawing.Color]::FromArgb(24, 24, 24))
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
    $font = New-Object System.Drawing.Font 'Segoe UI', 13, ([System.Drawing.FontStyle]::Bold)
    for ($i = 0; $i -lt $items.Count; $i++) {
        $file = Join-Path $Dir ($items[$i][0] + '.png')
        $img = [System.Drawing.Image]::FromFile($file)
        $cx = ($i % $cols) * ($tw + 8); $cy = [math]::Floor($i / $cols) * ($th + $lab + 8)
        $g.DrawString($items[$i][1], $font, [System.Drawing.Brushes]::White, $cx + 4, $cy + 3)
        $g.DrawImage($img, (New-Object System.Drawing.Rectangle $cx, ($cy + $lab), $tw, $th),
                     (New-Object System.Drawing.Rectangle $x, $y, $w, $h), [System.Drawing.GraphicsUnit]::Pixel)
        $img.Dispose()
    }
    $sheet.Save((Join-Path $Dir $out), [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $sheet.Dispose()
    Write-Host "  $out"
}

Sheet 'zoom-filters.png' 360 590 560 300 2 2 @(
    @('03-widescreen-world', 'ui_filter=off'), @('05-filter-mmpx', 'ui_filter=mmpx'),
    @('06-filter-xbr', 'ui_filter=xbr'), @('07-filter-remacri', 'ui_filter=remacri'))
Sheet 'zoom-upscalers.png' 570 320 660 250 2 2 @(
    @('10-upscaler-off', 'upscaler=off'), @('11-upscaler-taa', 'upscaler=taa (100%)'),
    @('12-upscaler-fsr', 'upscaler=fsr (100%)'), @('13-upscaler-dlss', 'upscaler=dlss (100%, DLAA)'),
    @('14-taa-50', 'upscaler=taa, render_scale=50'), @('15-fsr-50', 'upscaler=fsr, render_scale=50'),
    @('16-dlss-50', 'upscaler=dlss, render_scale=50'))
