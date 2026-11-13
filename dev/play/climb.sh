#!/bin/bash
# In a Simulator runway start (to_sim.sh, game running): start both
# engines, full throttle, release the brakes, rotate, gear up, climb for
# CLIMB seconds (default 30), then pitch down for PUSH seconds (default 3)
# and pause the game (P).
cd "$(dirname "$0")/../.."
d() { dev/drive.sh "$@" >/dev/null 2>&1; }
d k 1a; sleep 1; d k 1b; sleep 8          # [ ] engines
d d 0d; sleep 5; d u 0d; sleep 1          # = throttle up
d k 11; sleep 25                          # W brakes off, take-off run
d d 50; sleep 2.5; d u 50; d k 22         # rotate, G gear up
sleep "${1:-30}"
d d 48; sleep "${2:-3}"; d u 48; sleep 2  # nose down
d k 19; sleep 1.5                         # P pause
echo "climbed and paused"
