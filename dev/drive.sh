#!/bin/bash
# Live-drive a running game started with TFX_CMDFILE=work/cmd.txt (see dev/run_tfx.sh).
#   dev/drive.sh k 1c          press and release a key (XT scancode, hex)
#   dev/drive.sh d 48 / u 48   key down / key up only
#   dev/drive.sh r 25 40 1     relative mouse move (mickeys) with buttons (1 = left)
#   dev/drive.sh shot NAME     screenshot -> work/NAME.png
#   dev/drive.sh q             quit the game
cd "$(dirname "$0")/.."
CMD=work/cmd.txt
WORK=$(cygpath -m "$(pwd)/work")
if [ "$1" == "wshot" ]; then          # the window as presented (scaled)
  rm -f "work/$2.bmp" "work/$2.png"
  echo "W $WORK/$2.bmp" >> $CMD
  for i in 1 2 3 4 5 6 7 8 9 10; do [ -f "work/$2.bmp" ] && break; sleep 0.3; done
  sleep 0.3
  powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w tools/bmp2png.ps1)" \
    -In "$WORK/$2.bmp" -Out "$WORK/$2.png" -Scale 1
elif [ "$1" == "shot" ]; then
  rm -f "work/$2.bmp" "work/$2.png"
  echo "s $WORK/$2.bmp" >> $CMD
  for i in 1 2 3 4 5 6 7 8 9 10; do [ -f "work/$2.bmp" ] && break; sleep 0.3; done
  sleep 0.3
  powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w tools/bmp2png.ps1)" \
    -In "$WORK/$2.bmp" -Out "$WORK/$2.png"
else
  echo "$*" >> $CMD
fi
