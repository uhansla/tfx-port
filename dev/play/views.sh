#!/bin/bash
# View keys from the chase view (game in flight, e.g. after arcade.sh): F5, then
# another view key, each shown in a window shot -> work/view_*.png.  At
# framerate=high the chase view once locked every view key (DEBUG_LOG 111).
cd "$(dirname "$0")/../.."
d() { dev/drive.sh "$@" >/dev/null 2>&1; }
for v in "3b f1" "3c f2" "3d f3" "3e f4" "40 f6" "41 f7" "42 f8" "43 f9" "44 f10"; do
  set -- $v
  d k 3f; sleep 3; d wshot view_f5_before_$2
  d k "$1"; sleep 2.5; d wshot "view_f5_then_$2"
done
d k 3b
echo "views done: compare work/view_f5_then_*.png (each must differ from the chase view)"
