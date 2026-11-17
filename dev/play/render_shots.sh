#!/bin/bash
# Screenshots of the render options, for comparison: one game run (arcade.sh
# to the cockpit), paused, the options switched live with the cmdfile's O
# command, a window shot of each -> work/render-options/NN-name.png.  The
# cockpit view for the renderer, widescreen and cockpit-filter shots, an
# outside view (F7) for the upscalers.  build/tfx/tfx.ini is put back after.
#   SIZE=1600x900 dev/play/render_shots.sh
cd "$(dirname "$0")/../.."
d() { dev/drive.sh "$@" >/dev/null 2>&1; }
OUT=work/render-options
SIZE=${SIZE:-1600x900}
INI=build/tfx/tfx.ini
mkdir -p "$OUT"
cp "$INI" work/tfx.ini.before-shots
# upscaler=dlss at start: Streamline must be up before the swap chain exists,
# then every upscaler can be switched live
cat > "$INI" <<EOF
window=$SIZE
fullscreen=0
aspect=4:3
scaling=sharp
framerate=high
renderer=native
widescreen=on
widescreen_sides=world
ui_filter=off
upscaler=dlss
render_scale=100
frame_generation=off
presenter=d3d12
joystick=off
head_tracking=off
EOF
restore() { cp work/tfx.ini.before-shots "$INI"; }
trap restore EXIT

if ! T=600 bash dev/play/arcade.sh | tee /dev/stderr | grep -q "in flight$"; then
  d wshot ro_stuck; mv -f work/ro_stuck.png "$OUT/stuck.png" 2>/dev/null
  d q; sleep 5; echo "not in flight: no shots taken ($OUT/stuck.png shows where it stopped)"; exit 1
fi
sleep 8                                       # level off after the start
shot() {                                      # shot NAME: settle, then the window as presented
  sleep "${WAIT:-3}"; d wshot "ro_$1"
  [ -f "work/ro_$1.png" ] && mv -f "work/ro_$1.png" "$OUT/$1.png" && rm -f "work/ro_$1.bmp"
  echo "  $1"
}
set_() { for kv in "$@"; do d O "$kv"; done; }

d k 3b; sleep 3                               # F1: the cockpit
d k 19; sleep 1                               # P: pause
set_ upscaler=taa
set_ renderer=game;                                    shot 01-renderer-game
set_ renderer=native widescreen=off;                   shot 02-renderer-native-4x3
set_ widescreen=on;                                    shot 03-widescreen-world
set_ widescreen_sides=cockpit;                         shot 04-widescreen-cockpit
set_ widescreen_sides=world
set_ ui_filter=mmpx;                                   shot 05-filter-mmpx
set_ ui_filter=xbr;                                    shot 06-filter-xbr
set_ ui_filter=remacri;                                shot 07-filter-remacri
set_ ui_filter=off

d k 19; sleep 1; d k 41; sleep 4              # unpause, F7: an outside view
d k 19; sleep 1                               # pause
set_ upscaler=off;                                     shot 10-upscaler-off
set_ upscaler=taa;                                     shot 11-upscaler-taa
set_ upscaler=fsr;                                     shot 12-upscaler-fsr
set_ upscaler=dlss;                                    WAIT=5 shot 13-upscaler-dlss
set_ render_scale=50 upscaler=taa;                     shot 14-taa-50
set_ upscaler=fsr;                                     shot 15-fsr-50
set_ upscaler=dlss;                                    WAIT=5 shot 16-dlss-50
set_ render_scale=100
d S; sleep 1
grep "^settings:\|^dlss\|^fsr\|^d3d12: \(adapter\|presenter\)\|upscal" build/tfx/runtime.log | head -40 > "$OUT/log.txt"
d q; sleep 5
echo "shots in $OUT"
