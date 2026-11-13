#!/bin/bash
# Drive a running live game (TFX_CMDFILE=work/cmd.txt) from boot to the
# Arcade cockpit, using the saved pilot in slot 1.  Assumes the game was just
# started and nobody touches the window.
cd "$(dirname "$0")/.."
d() { dev/drive.sh "$@"; }
click() { d r "$1" "$2" 0; sleep 0.4; d r 0 0 1; sleep 0.3; d r 0 0 0; }
sleep 6;  d k 01            # skip credits
sleep 3;  d k 01            # skip rooms
sleep 5;  click 160 102     # LOAD PREVIOUS GAME (cursor starts at 0,0)
sleep 4;  click -40 -55     # pilot in slot 1
sleep 1;  d k 1c            # confirm
sleep 5;  click 15 15       # ARCADE
sleep 4;  click 20 37       # START
sleep 25                    # newspaper, LEVEL ONE, mission load
echo "in cockpit (probably)"
