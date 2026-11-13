#!/bin/bash
# usage: calltree.sh FUNC  -> calls made by FUNC with live counts
f=$1
file=$(grep -l "^void f_$f(" build/tfx_gen/*.c)
awk "/^void f_$f\(/,/^}/" $file | grep -o "f_4[0-9a-f]*(0x[0-9a-f]*u)\|rt_call([^;]*" | while read c; do
  a=$(echo $c | grep -o "f_4[0-9a-f]*" | head -1); a=${a:2}
  n=$(grep -i "^00$a " work/hist.txt | cut -d' ' -f2)
  if [ -n "$a" ]; then printf "%s:%s " $a ${n:-0}; else printf "[%s] " "$c"; fi
done; echo
