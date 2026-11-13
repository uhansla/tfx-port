"""16-bit disassembly of a code blob inside a DOS MZ file (START.EXE's music
drivers are position-independent blobs that run with CS at their own base).

usage: py tools/dis16.py FILE CSBASE_FILEOFF OFF [COUNT]
       py tools/dis16.py FILE CSBASE_FILEOFF table OFF N   (word table of near pointers)
"""
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_16

f, base = sys.argv[1], int(sys.argv[2], 16)
d = open(f, 'rb').read()
md = Cs(CS_ARCH_X86, CS_MODE_16)
if sys.argv[3] == 'table':
    off, n = int(sys.argv[4], 16), int(sys.argv[5])
    for i in range(n):
        w = d[base + off + 2 * i] | d[base + off + 2 * i + 1] << 8
        print(f'{i:02x}: {w:04x}')
    sys.exit()
off = int(sys.argv[3], 16)
count = int(sys.argv[4]) if len(sys.argv) > 4 else 60
for ins in md.disasm(d[base + off:base + off + 16 * count], off):
    print(f'{ins.address:04x}  {ins.bytes.hex():16} {ins.mnemonic} {ins.op_str}')
    count -= 1
    if count <= 0:
        break
