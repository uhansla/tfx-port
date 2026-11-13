#!/bin/bash
# usage: dev/play/bisect2.sh RANGESET...   (game running in the cockpit with the live cmdfile)
# For each range set: interpret it for a few seconds, then report how often the
# scan converter (0x47AB50) and the rasterizer region (0x470000-0x47c000) were
# called, plus rasterizer VGA writes.  "-" = no interpretation (baseline).
cd "$(dirname "$0")/../.."
LOG=build/tfx/runtime.log
for R in "$@"; do
  [ "$R" == "-" ] && R=""
  dev/drive.sh i $R; sleep 2
  dev/drive.sh z; dev/drive.sh g; sleep ${SECS:-6}
  dev/drive.sh h; dev/drive.sh g; sleep 2
  dev/drive.sh i; sleep 1
  py -3.12 - "$LOG" "$R" <<'PYEOF'
import sys
lines = open(sys.argv[1], errors='replace').read().splitlines()
# take everything after the last "cmdfile: z"
last_z = max(i for i, l in enumerate(lines) if l.startswith('cmdfile: z'))
seg = lines[last_z:]
sc = ras = 0; top = []
for l in seg:
    if l.startswith('hist '):
        _, a, n = l.split(); a = int(a, 16); n = int(n)
        if a == 0x47ab50: sc += n
        if 0x470000 <= a < 0x47c000: ras += n
vg = 0
for l in seg:
    if l.startswith('vgaw '):
        _, e, n = l.split()
        if 0x470000 <= int(e, 16) < 0x47c000: vg += int(n)
fatal = [l for l in lines if l.startswith('FATAL')]
print(f'ranges={sys.argv[2] or "(none)":30s} scanconv={sc} raster_calls={ras} raster_vgaw={vg} fatal={fatal[-1] if fatal else "-"}')
PYEOF
done
