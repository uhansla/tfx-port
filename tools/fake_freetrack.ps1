# A stand-in FreeTrack source for tests: writes a head pose to the FT_SharedMem
# block that head trackers (opentrack, ...) write and runtime/track.c reads.
# usage: fake_freetrack.ps1 -Yaw 30 -Pitch 10 -Seconds 8     (degrees, as opentrack writes them: yaw positive = left, pitch positive = down)
#        fake_freetrack.ps1 -Sweep -Seconds 12               (yaw swings +-120 degrees, pitch +-20)
param([double]$Yaw = 0, [double]$Pitch = 0, [double]$Seconds = 8, [switch]$Sweep)
$mm = [System.IO.MemoryMappedFiles.MemoryMappedFile]::CreateOrOpen('FT_SharedMem', 108)
$v = $mm.CreateViewAccessor(0, 108)
$d2r = [math]::PI / 180
$sw = [System.Diagnostics.Stopwatch]::StartNew()
[uint32]$id = $v.ReadUInt32(0)
while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
    $y = $Yaw; $p = $Pitch
    if ($Sweep) {
        $t = $sw.Elapsed.TotalSeconds
        $y = 120 * [math]::Sin($t * 2 * [math]::PI / 8)
        $p = 20 * [math]::Sin($t * 2 * [math]::PI / 5)
    }
    $v.Write(12, [single]($y * $d2r))
    $v.Write(16, [single]($p * $d2r))
    $v.Write(20, [single]0)
    $id++
    $v.Write(0, $id)
    Start-Sleep -Milliseconds 10
}
$v.Dispose(); $mm.Dispose()
