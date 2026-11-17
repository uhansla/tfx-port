#!/bin/bash
# usage: dev/play/to_sim.sh THEATRE WEATHER
#   THEATRE: yugo | libya | colombia | somalia | ocean
#   WEATHER: dawn | clear | cloud | storm | night | nightcloud
# Start the game (live cmdfile), wait for the presenter, then Enrol ->
# pilot 1 -> Simulator -> theatre -> time of day/weather -> arming OK, and
# wait on the runway.  Leaves the game running.
cd "$(dirname "$0")/../.."
d() { dev/drive.sh "$@" >/dev/null 2>&1; }
click() { d r "$1" "$2" 0; sleep 0.4; d r 0 0 1; sleep 0.3; d r 0 0 0; }
case "$1" in                                  # world map: from the cursor at (145,91)
  yugo) T="11 -34";; libya) T="7 -15";; colombia) T="-92 14";; somalia) T="39 15";; ocean) T="-3 75";;
  *) echo "theatre?"; exit 1;;
esac
case "$2" in                                  # rows of the weather menu
  dawn) W=31;; clear) W=48;; cloud) W=64;; storm) W=81;; night) W=98;; nightcloud) W=116;;
  *) echo "weather?"; exit 1;;
esac
L=build/tfx/runtime.log
: > work/cmd.txt; : > $L
(TFX_CMDFILE=$(cygpath -m "$(pwd)/work/cmd.txt") T=${T_RUN:-1800} dev/run_tfx.sh > work/run.out 2>&1 &)
for i in $(seq 1 90); do grep -q "d3d12: presenter\|presenter: " $L 2>/dev/null && break; sleep 0.5; done
sleep 6;  d k 01            # credits
sleep 3;  d k 01            # rooms
sleep 5;  d r -2000 -2000 0; sleep 0.4   # the pointer to the corner (a real mouse over the window moves it)
          click 160 102     # LOAD PREVIOUS GAME
sleep 4;  click -40 -55     # pilot in slot 1
sleep 1;  d k 1c
sleep 5;  click 25 43       # SIMULATOR
sleep 8;  click $T          # theatre
sleep 6;  d r -400 -400 0; sleep 0.4
click 150 $W                # time of day / weather
sleep 9;  d r 2000 2000 0; sleep 0.4
click -245 0                # arming screen: OK
sleep 16
echo "on the runway (probably): $1 $2"
