#!/bin/bash
# usage: dev/play/fps_case.sh normal|high  (Arcade flight, tfx.ini framerate set to it): frame rate with the
# page flip -> present delay, the game time step [0x4A42C0], DLSS-G status.  TFX_PRESENT_TICK=1 = old pacing
cd "$(dirname "$0")/../.."
sed -i "s/^framerate=.*/framerate=$1/" build/tfx/tfx.ini
d() { dev/drive.sh "$@" >/dev/null 2>&1; }
L=build/tfx/runtime.log
bash dev/play/arcade.sh | tail -1; sleep 3
powershell -NoProfile -Command "(New-Object -ComObject WScript.Shell).AppActivate((Get-Process tfx).Id)" >/dev/null; sleep 2
d f; sleep 10; d f; d p 4a42c0 2; sleep 12; d f; sleep 1
echo "== framerate=$1"; grep -E "^fps:|^peek 004a42c|dlss-g: status|present|not focused" $L | tail -10
d q; sleep 5
