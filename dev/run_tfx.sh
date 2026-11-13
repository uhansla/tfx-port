#!/bin/bash
# run the recompiled TFX game with a timeout; show log tail
cd "$(dirname "$0")/../build/tfx"
timeout ${T:-20} ./tfx.exe --cd ../../_extract/cdfiles --cdimage ../../_extract/installed/TFX/cd/TFX.cue --env DID=D: tfx "$@"
echo "exit=$?"
tail -${N:-25} runtime.log
