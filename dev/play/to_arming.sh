#!/bin/bash
# Boot (live cmdfile) -> Enrol -> Training -> briefings -> arming screen.
cd "$(dirname "$0")/../.."
d() { dev/drive.sh "$@" >/dev/null 2>&1; }
click() { d r "$1" "$2" 0; sleep 0.4; d r 0 0 1; sleep 0.3; d r 0 0 0; }
sleep 6;  d k 01            # skip credits
sleep 3;  d k 01            # skip rooms
sleep 5;  click 160 102     # LOAD PREVIOUS GAME
sleep 4;  click -40 -55     # pilot in slot 1
sleep 1;  d k 1c
sleep 5;  click 25 30       # TRAINING
sleep 9;  d k 1c            # commander
sleep 5;  d k 1c
sleep 8;  d k 1c
sleep 5;  d k 1c            # laptop
sleep 6;  d k 1c            # HOP ONE text
sleep 6;  d k 1c
sleep 5
echo "at arming screen (probably)"
