#!/bin/bash
# usage: dev/play/mode.sh NAME DY [STEPS]
# Start the game, pick the Enrol-screen entry DY pixels below the cursor's
# position after the pilot load (Arcade 15, Training 30, Simulator 43,
# Tour of Duty 57, Flash Points 71, UN Commander 85), then press Enter
# STEPS times with a screenshot before each; contact sheet -> work/mode_NAME.png.
cd "$(dirname "$0")/../.."
NAME=$1; DY=$2; STEPS=${3:-8}
d() { dev/drive.sh "$@" >/dev/null 2>&1; }
click() { d r "$1" "$2" 0; sleep 0.4; d r 0 0 1; sleep 0.3; d r 0 0 0; }
: > work/cmd.txt
(TFX_CMDFILE=$(cygpath -m "$(pwd)/work/cmd.txt") T=900 dev/run_tfx.sh > work/run.out 2>&1 &)
sleep 6;  d k 01
sleep 3;  d k 01
sleep 5;  click 160 102
sleep 4;  click -40 -55
sleep 1;  d k 1c
sleep 5;  click 25 "$DY"
shots=()
for i in $(seq 1 "$STEPS"); do
  sleep 7; d shot "m_${NAME}_$i"; shots+=("m_${NAME}_$i.png"); d k 1c
done
sleep 6; d shot "m_${NAME}_end"; shots+=("m_${NAME}_end.png")
grep -E "HOST EXC|FATAL" build/tfx/runtime.log | tail -1
cd work && powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w ../tools/contact.ps1)" \
  -Out "$(cygpath -w "$(pwd)")\\mode_$NAME.png" -Cols 3 -W 320 "${shots[@]}"
