# One-click build (started by build.bat): fetches the build tools into
# toolchain\, asks for your game folder, builds tfx.exe and tfxconfig.exe from
# your own TFX.DAT / CONFIG.EXE, offers to copy them into the game folder and
# then to choose the port's settings (tfx.ini), each with what it does.
#
#   build.bat                      ask for everything
#   build.bat -Game "D:\Games\TFX" the installed game (the folder with TFX.DAT)
#   build.bat -Upscalers           also fetch AMD FidelityFX and NVIDIA Streamline
#   build.bat -NoUpscalers         don't ask about them
#   build.bat -Install / -NoInstall   copy the result into the game folder, or don't ask
#   build.bat -Settings / -NoSettings  choose the port's settings (tfx.ini), or don't ask
#
# Nothing is installed on the PC: everything goes into this folder's
# toolchain\ (about 1 GB, 2 GB with the upscaler SDKs) and build\.  Every
# download is a fixed version whose SHA-256 is checked below.
param([string]$Game = '', [switch]$Upscalers, [switch]$NoUpscalers, [switch]$Install, [switch]$NoInstall, [switch]$Settings, [switch]$NoSettings, [switch]$NoPause)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$Root = Split-Path -Parent $PSScriptRoot
$Tc = Join-Path $Root 'toolchain'
$Dl = Join-Path $Tc 'dl'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

function Finish([int]$code) {
    if (-not $NoPause) { Write-Host ''; Read-Host 'Press Enter to close' | Out-Null }
    exit $code
}
function Fail([string]$msg) {
    Write-Host ''
    Write-Host "FAILED: $msg" -ForegroundColor Red
    Finish 1
}

# name, file, address, SHA-256, size in MB, folder in toolchain\, a file that shows it is there
$PyDir = 'python-3.12.10-embed-amd64'
$Tools = @(
    @{ Name = 'compiler (llvm-mingw 20261020)'; File = 'llvm-mingw-20261020-ucrt-x86_64.zip'; Mb = 182
       Url = 'https://github.com/mstorsjo/llvm-mingw/releases/download/20261020/llvm-mingw-20261020-ucrt-x86_64.zip'
       Sha = 'fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd'
       Into = ''; Probe = 'llvm-mingw-20261020-ucrt-x86_64\bin\x86_64-w64-mingw32-clang.exe' },
    @{ Name = 'SDL 2.30.9 (development package)'; File = 'SDL2-devel-2.30.9-mingw.zip'; Mb = 8
       Url = 'https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-devel-2.30.9-mingw.zip'
       Sha = '492abbc78bbcad000d224cc56a200938f8be09a4a9aae253defa6035cbf68d9d'
       Into = ''; Probe = 'SDL2-2.30.9\x86_64-w64-mingw32\lib\libSDL2.a' },
    @{ Name = 'Python 3.12.10 (private copy)'; File = 'python-3.12.10-embed-amd64.zip'; Mb = 11
       Url = 'https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip'
       Sha = '4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3'
       Into = $PyDir; Probe = "$PyDir\python.exe" },
    @{ Name = 'capstone 5.0.7 (disassembler for the recompiler)'; File = 'capstone-5.0.7-py3-none-win_amd64.whl'; Mb = 2
       Url = 'https://files.pythonhosted.org/packages/70/39/2138d890a8e827636b9de9924fbc8527fe83e38b6d26605b30ac55e30ebe/capstone-5.0.7-py3-none-win_amd64.whl'
       Sha = '4ab8bcb7da8f221ff45926ca168ca33e76f7237d06fbf3c10780002faa2670e1'
       Into = $PyDir; Probe = "$PyDir\capstone\lib\capstone.dll" }
)

function Download([string]$url, [string]$file, [string]$sha) {
    $path = Join-Path $Dl $file
    if (-not (Test-Path $Dl)) { New-Item -ItemType Directory -Force $Dl | Out-Null }
    if (Test-Path $path) {
        if (-not $sha -or (Get-FileHash $path -Algorithm SHA256).Hash -eq $sha) { return $path }
        Remove-Item $path -Force
    }
    $curl = Join-Path $env:SystemRoot 'System32\curl.exe'       # Windows' own (10 1803 and later)
    if (Test-Path $curl) {
        & $curl -L --fail --retry 3 --progress-bar -o $path $url
        if ($LASTEXITCODE -ne 0) { Fail "could not download $url" }
    } else {
        try { Invoke-WebRequest -Uri $url -OutFile $path -UseBasicParsing } catch { Fail "could not download $url : $_" }
    }
    if ($sha) {
        $got = (Get-FileHash $path -Algorithm SHA256).Hash
        if ($got -ne $sha) {
            Remove-Item $path -Force
            Fail "$file does not have the expected checksum (got $got). The download was removed; nothing was unpacked."
        }
    }
    return $path
}

# unpack a zip (or a wheel, which is one); only the given members when $members is set
function Unpack([string]$zip, [string]$dest, [string[]]$members) {
    if (-not (Test-Path $dest)) { New-Item -ItemType Directory -Force $dest | Out-Null }
    $tar = Join-Path $env:SystemRoot 'System32\tar.exe'         # Windows' own bsdtar reads zip files; a GNU tar on the PATH doesn't
    if (Test-Path $tar) {
        if ($members) { & $tar -xf $zip -C $dest @members } else { & $tar -xf $zip -C $dest }
        if ($LASTEXITCODE -ne 0) { Fail "could not unpack $zip" }
    } else {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $z = [IO.Compression.ZipFile]::OpenRead($zip)
        try {
            foreach ($e in $z.Entries) {
                if (-not $e.Name) { continue }
                if ($members) {
                    $hit = $false
                    foreach ($m in $members) { if ($e.FullName -eq $m -or $e.FullName.StartsWith($m.TrimEnd('/') + '/')) { $hit = $true } }
                    if (-not $hit) { continue }
                }
                $t = Join-Path $dest $e.FullName
                $d = Split-Path -Parent $t
                if (-not (Test-Path $d)) { New-Item -ItemType Directory -Force $d | Out-Null }
                [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $t, $true)
            }
        } finally { $z.Dispose() }
    }
}

function Signed-By([string]$file, [string]$who) {
    $s = Get-AuthenticodeSignature $file
    return ($s.Status -eq 'Valid' -and $s.SignerCertificate.Subject -like "*$who*")
}

Write-Host 'TFX-port: build from your own copy of the game' -ForegroundColor Cyan
Write-Host ''

# ---- 1. the game folder ------------------------------------------------------
function Game-Folder([string]$d) {
    if (-not $d) { return $null }
    foreach ($sub in '', 'TFX', 'TFX\TFX') {
        $g = Join-Path $d $sub
        if (Test-Path (Join-Path $g 'TFX.DAT')) { return (Resolve-Path $g).Path.TrimEnd('\') }
    }
    return $null
}
$memo = Join-Path $Root 'build\game_dir.txt'
$g = Game-Folder $Game
if ($Game -and -not $g) { Fail "there is no TFX.DAT in `"$Game`". Give the folder of the installed game." }
if (-not $g -and (Test-Path $memo)) { $g = Game-Folder ((Get-Content $memo -Raw).Trim()) }
if (-not $g) {
    Write-Host 'Choose the folder of your installed TFX (the one that holds TFX.DAT) in the dialog...'
    Add-Type -AssemblyName System.Windows.Forms
    $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
    $dlg.Description = 'The folder of your installed TFX: the one that holds TFX.DAT'
    $dlg.ShowNewFolderButton = $false
    if ($dlg.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { Fail 'no game folder was chosen.' }
    $g = Game-Folder $dlg.SelectedPath
    if (-not $g) { Fail "there is no TFX.DAT in `"$($dlg.SelectedPath)`"." }
}
Write-Host "Game folder: $g"

# the right version?  Before any download.  Keep in step with SUPPORTED / KNOWN in tools/build.py.
$Supported = '2CC41A39FA95A2C9FD883C4220D7BDD9CCA00C543C89B019EB270763F59D1D9A'
$Known = @{ 'FE2C8D787454EBFAC1E2895BE1E82FA89C846526894E4D611D8038E847E2D6FB' =
            'the TFX.DAT on the CD itself: the original, unpatched CD release (1,013,255 bytes)' }
$Advice = 'Install TFX from its CD and apply the official CD patches tfxcd_1, tfxcd_2 and tfxcd_3, in that order (they take the game to version 2.01).'
$dat = Join-Path $g 'TFX.DAT'
$h = (Get-FileHash $dat -Algorithm SHA256).Hash
if ($h -ne $Supported) {
    $len = (Get-Item $dat).Length
    $head = [IO.File]::ReadAllBytes($dat)[0..1]
    if ($Known.ContainsKey($h)) { $what = $Known[$h] }
    elseif ($head[0] -ne 0x4D -or $head[1] -ne 0x5A) { $what = 'not a DOS program at all'; $Advice = 'Choose the folder of the installed game, where TFX.DAT is the game''s program file.' }
    elseif ($len -eq 1032503) { $what = 'the right size but different contents: changed by a crack, a trainer or another patch, or damaged'; $Advice = 'Reinstall the game from its CD and apply the official CD patches 1 to 3.' }
    else { $what = "an unknown version ($('{0:N0}' -f $len) bytes; the supported one has 1,032,503)"; $Advice = 'The port needs the CD-ROM release patched to version 2.01. ' + $Advice }
    Write-Host ''
    Write-Host "This TFX.DAT is $what." -ForegroundColor Yellow
    Write-Host $Advice
    Write-Host 'The port works on the game''s code at fixed addresses, so it needs exactly that file. Nothing was downloaded.'
    Fail 'unsupported version of TFX.DAT'
}
Write-Host 'TFX.DAT:     the supported version (v2.01 CD release)'
# the CD image: the game won't start without it (the same places tfx.exe looks)
$cue = @((Join-Path $g '..\cd'), (Join-Path $g 'cd'), $g, (Join-Path $g '..')) |
       Where-Object { Test-Path $_ } | ForEach-Object { Get-ChildItem $_ -Filter '*.cue' -ErrorAction SilentlyContinue } | Select-Object -First 1
if ($cue) { Write-Host "CD image:    $($cue.FullName)" }
else {
    Write-Host 'CD image:    not found. The game needs its CD image (TFX.cue + TFX.bin) to start: put it in a "cd" folder' -ForegroundColor Yellow
    Write-Host '             next to the game folder, or start tfx.exe with --cdimage <file.cue>. The build goes on.' -ForegroundColor Yellow
}

# ---- 2. the build tools ------------------------------------------------------
foreach ($t in $Tools) {
    if (Test-Path (Join-Path $Tc $t.Probe)) { Write-Host "  have   $($t.Name)"; continue }
    Write-Host "  fetch  $($t.Name), about $($t.Mb) MB"
    $zip = Download $t.Url $t.File $t.Sha
    Write-Host '         unpacking...'
    Unpack $zip (Join-Path $Tc $t.Into) $null
    if (-not (Test-Path (Join-Path $Tc $t.Probe))) { Fail "$($t.File) did not unpack as expected ($($t.Probe) is missing)." }
}

# ---- 3. optional: the upscaler SDKs -------------------------------------------
$sl = Join-Path $Tc 'streamline-2.14.16'
$ffx = Join-Path $Tc 'fidelityfx-2.4.0'
$haveSl = Test-Path (Join-Path $sl 'bin\x64\sl.interposer.dll')
$haveFfx = Test-Path (Join-Path $ffx 'bin\amd_fidelityfx_loader_dx12.dll')
if (-not ($haveSl -and $haveFfx) -and -not $NoUpscalers) {
    if (-not $Upscalers) {
        Write-Host ''
        Write-Host 'Optional: AMD FSR and NVIDIA DLSS (with Frame Generation) need AMD''s FidelityFX SDK and'
        Write-Host 'NVIDIA''s Streamline SDK: about 480 MB of downloads, used under their own licences'
        Write-Host '  https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.4.0/license.md'
        Write-Host '  https://github.com/NVIDIA-RTX/Streamline/blob/v2.14.16/license.txt (and the DLSS licence beside its DLLs)'
        Write-Host 'Without them the game uses the port''s own upscaler. You can run build.bat -Upscalers later.'
        $a = Read-Host 'Fetch them now and accept those licences? [y/N]'
        if ($a -match '^[yY]') { $Upscalers = $true }
    }
    if ($Upscalers -and -not $haveSl) {
        Write-Host '  fetch  NVIDIA Streamline SDK 2.14.16, about 264 MB'
        $zip = Download 'https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.14.16/streamline-sdk-v2.14.16.zip' `
                        'streamline-sdk-v2.14.16.zip' '32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b'
        Write-Host '         unpacking...'
        Unpack $zip $sl @('bin/x64', 'include', 'license.txt', '3rd-party-licenses.md')
        foreach ($n in 'sl.interposer.dll', 'nvngx_dlss.dll') {
            if (-not (Signed-By (Join-Path $sl "bin\x64\$n") 'NVIDIA')) { Fail "$n is not signed by NVIDIA." }
        }
    }
    if ($Upscalers -and -not $haveFfx) {
        Write-Host '  fetch  AMD FidelityFX SDK 2.4.0, about 200 MB'
        $zip = Download 'https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/archive/refs/tags/v2.4.0.zip' 'FidelityFX-SDK-2.4.0.zip' ''
        Write-Host '         unpacking...'
        $tmp = Join-Path $Tc 'fidelityfx-unpack'
        if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
        Unpack $zip $tmp $null
        # the SDK's own tree is deep; the build needs its API headers, the upscaler header and the two signed DLLs
        $api = Get-ChildItem $tmp -Recurse -Filter 'ffx_api.h' | Where-Object { Test-Path (Join-Path $_.DirectoryName 'dx12\ffx_api_dx12.h') } | Select-Object -First 1
        $up = Get-ChildItem $tmp -Recurse -Filter 'ffx_upscale.h' | Select-Object -First 1
        $dll = Get-ChildItem $tmp -Recurse -Filter 'amd_fidelityfx_loader_dx12.dll' | Where-Object { Signed-By $_.FullName 'Advanced Micro Devices' } | Select-Object -First 1
        $lic = Get-ChildItem $tmp -Filter 'license.md' -Recurse -Depth 1 | Select-Object -First 1
        if (-not $api -or -not $up -or -not $dll) { Fail 'the FidelityFX SDK archive does not have the expected files (headers, signed DLLs).' }
        $up2 = Join-Path $dll.DirectoryName 'amd_fidelityfx_upscaler_dx12.dll'
        if (-not (Signed-By $up2 'Advanced Micro Devices')) { Fail 'amd_fidelityfx_upscaler_dx12.dll is missing or not signed by AMD.' }
        New-Item -ItemType Directory -Force (Join-Path $ffx 'api'), (Join-Path $ffx 'upscalers'), (Join-Path $ffx 'bin') | Out-Null
        Copy-Item $api.DirectoryName (Join-Path $ffx 'api\include') -Recurse -Force
        Copy-Item $up.DirectoryName (Join-Path $ffx 'upscalers\include') -Recurse -Force
        Copy-Item $dll.FullName, $up2 (Join-Path $ffx 'bin') -Force
        if ($lic) { Copy-Item $lic.FullName (Join-Path $ffx 'license.md') -Force }
        Remove-Item $tmp -Recurse -Force
    }
}

# ---- 4. build ------------------------------------------------------------------
Write-Host ''
$py = Join-Path $Tc "$PyDir\python.exe"
& $py (Join-Path $Root 'tools\build.py') all --game $g
if ($LASTEXITCODE -ne 0) { Finish 1 }

# ---- 5. into the game folder ---------------------------------------------------
$dist = Join-Path $Root 'dist'
if (-not $NoInstall) {
    if (-not $Install) {
        Write-Host ''
        $a = Read-Host "Copy tfx.exe and tfxconfig.exe (and the upscaler DLLs, if any) into `"$g`"? Nothing there is changed or removed. [Y/n]"
        if ($a -notmatch '^[nN]') { $Install = $true }
    }
    if ($Install) {
        Copy-Item (Join-Path $dist '*') $g -Force
        Write-Host "Copied. Start the game with $g\tfx.exe ; the port's settings are in tfx.ini beside it." -ForegroundColor Green
    }
}
if (-not $Install) { Write-Host "The result is in $dist ; copy its contents into your game folder. controls.bat there sets the port's options." -ForegroundColor Green }

# ---- 6. the port's settings: tfx.ini beside tfx.exe in the game folder ----------
# tools\inisetup.py asks option by option, with what each choice does; Enter keeps
# what is set.  It writes only the lines you change (controls.bat does the same).
if ($Install -and -not $NoSettings) {
    if (-not $Settings) {
        Write-Host ''
        $a = Read-Host 'Choose the port''s settings now (window, fullscreen, renderer, upscaler ...), each explained? [Y/n]'
        if ($a -notmatch '^[nN]') { $Settings = $true }
    }
    if ($Settings) {
        Write-Host ''
        & $py (Join-Path $Root 'tools\inisetup.py') $g
        if ($LASTEXITCODE -ne 0) { Write-Host 'The settings were not changed; controls.bat (Settings page) sets them later.' -ForegroundColor Yellow }
    } else { Write-Host "controls.bat in $g sets them later (Settings page)." }
}
Finish 0
