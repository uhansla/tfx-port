#!/bin/bash
# Flight test (game started with TFX_CMDFILE and already in the cockpit):
# frame rate, limiter headroom, every view, weapons.  Screenshots -> work/ft_*.png
cd "$(dirname "$0")/../.."
d() { dev/drive.sh "$@" >/dev/null 2>&1; }
key() { d k "$1"; sleep "${2:-2}"; }
LOG=build/tfx/runtime.log
# frame rate and limiter polls over 10 s of level flight
d f; d z; sleep 10; d f; d h; sleep 2
py -3.12 - "$LOG" <<'PYEOF'
import sys
lines = open(sys.argv[1], errors='replace').read().splitlines()
fps = [l for l in lines if l.startswith('fps:')]
print(fps[-1] if fps else 'no fps line')
last_z = max(i for i, l in enumerate(lines) if l.startswith('cmdfile: z'))
tick = sum(int(l.split()[2]) for l in lines[last_z:] if l.startswith('hist 00435690'))
lim = sum(int(l.split()[2]) for l in lines[last_z:] if l.startswith('hist 0041b998'))
print(f'tick reads={tick} limiter calls={lim} ticks/limiter={tick / max(lim, 1):.1f}')
PYEOF
d shot ft_front
for v in "3c left" "3d right" "3e rear" "3f chase" "40 fixed" "41 tactical" "42 missile" "43 target" "44 virtual"; do
  set -- $v; key "$1" 3; d shot "ft_$2"
done
key 3b 2                       # F1 front
key 32 2; d shot ft_map; key 32 1     # M map on/off
key 2e 2; d shot ft_target2    # C change target
key 1c 1                       # Enter: air-to-air weapon
d d 39; sleep 0.3; d u 39; sleep 3; d shot ft_fire   # Space: fire
d f; sleep 10; d f; sleep 1
grep -E "^fps:" "$LOG" | tail -1
grep -E "HOST EXC|FATAL" "$LOG" | tail -2
