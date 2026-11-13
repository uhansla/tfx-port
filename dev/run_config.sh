#!/bin/bash
# run the recompiled CONFIG with a timeout; show log tail
cd "$(dirname "$0")/../build/config"
timeout ${T:-20} ./tfxconfig.exe --cd ../../_extract/cdfiles "$@"
echo "exit=$?"
tail -${N:-25} runtime.log
