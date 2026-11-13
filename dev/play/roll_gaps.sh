#!/bin/bash
# usage: dev/play/roll_gaps.sh [FRAMES]   (game in flight, e.g. after arcade.sh)
# Holds a roll and saves FRAMES (default 40) window captures, about 0.2 s
# apart, as work/roll_NN.bmp; then counts the frames with uncovered pixels
# (alpha 0 in the capture: nothing was drawn there) on a grid of samples.
# Gaps in the corners of the wide view showed as black triangles (DEBUG_LOG 112).
cd "$(dirname "$0")/../.."
N=${1:-40}
W=$(cygpath -m "$(pwd)/work")
rm -f work/roll_*.bmp
echo "d 4b" >> work/cmd.txt
for i in $(seq -w 1 "$N"); do echo "W $W/roll_$i.bmp" >> work/cmd.txt; sleep 0.2; done
echo "u 4b" >> work/cmd.txt; sleep 1
powershell -NoProfile -Command "
Add-Type -AssemblyName System.Drawing
\$bad = 0; \$n = 0
foreach (\$f in Get-ChildItem '$W' -Filter 'roll_*.bmp') {
  \$b = [System.Drawing.Bitmap]::FromFile(\$f.FullName); \$n++; \$holes = 0
  for (\$y = 2; \$y -lt \$b.Height; \$y += [math]::Max(1, [int](\$b.Height / 40))) {
    for (\$x = 2; \$x -lt \$b.Width; \$x += [math]::Max(1, [int](\$b.Width / 80))) { if (\$b.GetPixel(\$x, \$y).A -eq 0) { \$holes++ } }
  }
  if (\$holes) { \$bad++; '{0}: {1} uncovered sample(s)' -f \$f.Name, \$holes }
  \$b.Dispose()
}
'{0} of {1} frames with gaps' -f \$bad, \$n"
