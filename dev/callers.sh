#!/bin/bash
# usage: callers.sh FUNC  -> functions whose generated code calls FUNC, with live counts
cd "$(dirname "$0")/.."
f=$1
grep -l "f_$f(" build/tfx_gen/code_*.c | while read file; do
  awk -v t="f_$f(" '/^void f_/{name=$2} index($0, t) && $0 !~ /^void/ {print name}' "$file"
done | sort -u | sed 's/(uint32_t//' | while read fn; do
  a=${fn:2}
  n=$(grep -i "^00$a " work/hist.txt | cut -d' ' -f2)
  echo "$a:${n:-0}"
done | tr '\n' ' '
echo
