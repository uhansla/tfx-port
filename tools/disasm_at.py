import sys
from analyze import *
from recomp import default_bases
p=Program(sys.argv[1], bases=default_bases(sys.argv[1]))
a=int(sys.argv[2],16); n=int(sys.argv[3])
for _ in range(n):
    i=p.decode(a)
    if i is None: print(hex(a),'??'); break
    print(f'{a:#x} {i.bytes.hex():18} {i.mnemonic} {i.op_str}'); a+=i.size
