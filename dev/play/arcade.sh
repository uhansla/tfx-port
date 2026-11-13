#!/bin/bash
# Start the game (live cmdfile), wait until the presenter is up (Streamline
# can add seconds to start-up), then Enrol -> pilot 1 -> Arcade -> START and
# wait until the native renderer draws the 3D view.  Leaves the game running.
cd "$(dirname "$0")/../.."
d() { dev/drive.sh "$@" >/dev/null 2>&1; }
click() { d r "$1" "$2" 0; sleep 0.4; d r 0 0 1; sleep 0.3; d r 0 0 0; }
L=build/tfx/runtime.log
: > work/cmd.txt; : > $L
(TFX_CMDFILE=$(cygpath -m "$(pwd)/work/cmd.txt") T=${T:-900} dev/run_tfx.sh > work/run.out 2>&1 &)
for i in $(seq 1 90); do grep -q "d3d12: presenter\|presenter: " $L 2>/dev/null && break; sleep 0.5; done
sleep 6;  d k 01            # credits
sleep 3;  d k 01            # rooms
sleep 5;  click 160 102     # LOAD PREVIOUS GAME
sleep 4;  click -40 -55     # pilot in slot 1
sleep 1;  d k 1c
sleep 5;  click 25 15       # ARCADE
for i in 1 2 3 4; do sleep 7; d k 1c; done
sleep 6
d r 9 36 0; sleep 0.4; d r 0 0 1; sleep 0.3; d r 0 0 0      # START
for i in 1 2 3 4 5 6; do
  sleep 7; d S; sleep 0.5
  tail -400 $L | grep -q "^native: [1-9]" && { echo "in flight"; exit 0; }
  d k 1c
done
echo "not in flight (see work/run.out and $L)"
