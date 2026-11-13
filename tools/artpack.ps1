# Upscaled art pack for ui_filter=remacri (RENDERER.md "Art pack").
#
#   artpack.ps1 prepare -Dump DIR -Work DIR
#       From a cmdfile `A DIR` dump: the panel strip (template, 320x55) and the
#       static sprites (several colours, at least 8x8) as PNGs in WORK\in, and
#       WORK\list.txt (kind, hash, name, game size).
#   artpack.ps1 upscale -Work DIR -Exe realesrgan-ncnn-vulkan.exe -Models DIR [-Model remacri-4x]
#       Runs the model over WORK\in into WORK\out (4x).
#   artpack.ps1 pack -Work DIR -Out DIR
#       WORK\out\*.png -> OUT\*.tfxa (TFXA, width, height, BGRA rows) and
#       OUT\pack.txt.  OUT is normally <game exe dir>\art\remacri.
param([Parameter(Mandatory)][ValidateSet('prepare', 'upscale', 'pack')][string]$Step,
      [string]$Dump, [string]$Work, [string]$Out, [string]$Exe, [string]$Models, [string]$Model = 'remacri-4x')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function ToPng($bmpPath, $pngPath, $rows) {
    # magenta (the dump's transparent colour) -> alpha 0; $rows: keep only the top rows
    $s = [System.Drawing.Bitmap]::FromFile($bmpPath)
    $h = if ($rows) { $rows } else { $s.Height }
    $t = New-Object System.Drawing.Bitmap $s.Width, $h, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    for ($y = 0; $y -lt $h; $y++) {
        for ($x = 0; $x -lt $s.Width; $x++) {
            $c = $s.GetPixel($x, $y)
            if ($c.R -eq 255 -and $c.G -eq 0 -and $c.B -eq 255) { $t.SetPixel($x, $y, [System.Drawing.Color]::Transparent) }
            else { $t.SetPixel($x, $y, [System.Drawing.Color]::FromArgb(255, $c.R, $c.G, $c.B)) }
        }
    }
    $t.Save($pngPath, [System.Drawing.Imaging.ImageFormat]::Png); $t.Dispose(); $s.Dispose()
}

switch ($Step) {
    'prepare' {
        New-Item -ItemType Directory -Force "$Work\in" | Out-Null
        $list = @(); $seen = @{}
        foreach ($l in Get-Content "$Dump\manifest.txt") {
            if ($l -match '^cockpit_template\.bmp template rows (\d+) hash ([0-9a-f]{8})') {
                $rows = [int]$Matches[1]; $hash = $Matches[2]
                if ($seen[$hash]) { continue }; $seen[$hash] = 1
                ToPng "$Dump\cockpit_template.bmp" "$Work\in\template_$hash.png" $rows
                $list += "template $hash template_$hash 320 $rows"
            } elseif ($l -match '^(spr_\S+)\.bmp .* size (\d+)x(\d+) colours (\d+) hash ([0-9a-f]{8})') {
                $name = $Matches[1]; $w = [int]$Matches[2]; $h = [int]$Matches[3]; $nc = [int]$Matches[4]; $hash = $Matches[5]
                if ($nc -lt 2 -or $w -lt 8 -or $h -lt 8 -or $seen[$hash]) { continue }   # one-colour: drawn by the HUD; tiny: digits
                $seen[$hash] = 1
                ToPng "$Dump\$name.bmp" "$Work\in\sprite_$hash.png" 0
                $list += "sprite $hash sprite_$hash $w $h"
            }
        }
        $list | Set-Content -Encoding ascii "$Work\list.txt"
        "prepared $($list.Count) asset(s) in $Work\in"
    }
    'upscale' {
        New-Item -ItemType Directory -Force "$Work\out" | Out-Null
        # the upscaler reports progress on stderr, which Windows PowerShell would turn into errors
        $ErrorActionPreference = 'Continue'
        cmd /c "`"$Exe`" -i `"$Work\in`" -o `"$Work\out`" -n $Model -s 4 -m `"$Models`" -f png 2>nul" | Out-Null
        $n = @(Get-ChildItem "$Work\out" -Filter *.png).Count
        if ($LASTEXITCODE -ne 0 -or $n -eq 0) { throw "the upscaler failed (exit $LASTEXITCODE, $n image(s))" }
        "upscaled $n image(s) into $Work\out"
    }
    'pack' {
        New-Item -ItemType Directory -Force $Out | Out-Null
        $lines = @('# kind hash file game-width game-height (tools/artpack.ps1)')
        foreach ($l in Get-Content "$Work\list.txt") {
            $kind, $hash, $name, $gw, $gh = $l -split ' '
            $png = "$Work\out\$name.png"
            if (-not (Test-Path $png)) { "missing $png (skipped)"; continue }
            $b = [System.Drawing.Bitmap]::FromFile($png)
            $r = New-Object System.Drawing.Rectangle 0, 0, $b.Width, $b.Height
            $d = $b.LockBits($r, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
            $bytes = New-Object byte[] ($b.Width * $b.Height * 4)
            for ($y = 0; $y -lt $b.Height; $y++) {
                [System.Runtime.InteropServices.Marshal]::Copy([IntPtr]($d.Scan0.ToInt64() + $y * $d.Stride), $bytes, $y * $b.Width * 4, $b.Width * 4)
            }
            $b.UnlockBits($d)
            $fs = [System.IO.File]::Create("$Out\$name.tfxa")
            $fs.Write([System.Text.Encoding]::ASCII.GetBytes('TFXA'), 0, 4)
            $fs.Write([BitConverter]::GetBytes([uint32]$b.Width), 0, 4)
            $fs.Write([BitConverter]::GetBytes([uint32]$b.Height), 0, 4)
            $fs.Write($bytes, 0, $bytes.Length); $fs.Close()
            $lines += "$kind $hash $name.tfxa $gw $gh"
            "$name $($b.Width)x$($b.Height)"
            $b.Dispose()
        }
        $lines | Set-Content -Encoding ascii "$Out\pack.txt"
        "pack: $($lines.Count - 1) asset(s) in $Out"
    }
}
