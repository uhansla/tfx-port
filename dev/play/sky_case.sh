#!/bin/bash
# usage: dev/play/sky_case.sh THEATRE WEATHER [keep]
# Simulator runway start in THEATRE/WEATHER (to_sim.sh), then the original
# picture (renderer=game) and the native window, a comparison of the view
# (tools/compare.ps1) -> work/sky_THEATRE_WEATHER.png, and a primitive dump
# in runtime.log.  Quits the game unless "keep" is given.
cd "$(dirname "$0")/../.."
N="sky_$1_$2"
bash dev/play/to_sim.sh "$1" "$2" || exit 1
d() { dev/drive.sh "$@" >/dev/null 2>&1; }
d O renderer=game; sleep 1.5; d shot "${N}_g"
d O renderer=native; sleep 1.5; d wshot "${N}_n"; d D; d S
W=$(cygpath -aw work)
powershell -NoProfile -ExecutionPolicy Bypass -File tools/compare.ps1 -Game "$W\\${N}_g.png" -Native "$W\\${N}_n.png" \
  -Out "$W\\$N.png" -X 0 -Y 0 -W 320 -H 150 -Scale 2
[ "$3" == "keep" ] || { d q; sleep 5; }
echo "$N done"
