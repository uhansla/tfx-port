#!/bin/bash
# Boot (live cmdfile) -> Enrol -> UN Commander -> pick the Somalia theatre,
# then screenshots un_1..un_N with Enter between them.
cd "$(dirname "$0")/../.."
d() { dev/drive.sh "$@" >/dev/null 2>&1; }
click() { d r "$1" "$2" 0; sleep 0.4; d r 0 0 1; sleep 0.3; d r 0 0 0; }
sleep 6;  d k 01
sleep 3;  d k 01
sleep 5;  click 160 102
sleep 4;  click -40 -55
sleep 1;  d k 1c
sleep 5;  click 25 85       # UN COMMANDER
sleep 6;  d k 1c            # "UN Central Command" title
sleep 6;  click 39 -27      # Somalia box on the theatre map
for i in $(seq 1 "${1:-5}"); do sleep 8; d shot "un_$i"; d k 1c; done
grep -E "HOST EXC|FATAL" build/tfx/runtime.log | tail -1
