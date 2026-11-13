"""Explain why a code-pointer candidate was rejected by the analyzer."""
import sys
from analyze import *
from recomp import default_bases
exe = sys.argv[1]
p = Program(exe)          # link bases
p.run()
for t in [int(x, 16) for x in sys.argv[2:]]:
    print(f'== {t:#x} in insns: {t in p.insns}')
    j = p._j = dict(insns=[], leaders=[], tables=[], soft=[], n_ind=len(p.indirect), n_dr=len(p.data_ranges))
    p.bad = []
    p._trace([t], set())
    p._j = None
    print('  bad:', [hex(x) for x in p.bad])
    for a in j['insns']:
        i = p.insns[a]
        if p.suspicious(i) or p._overlaps(a, i.size):
            print(f'  {"SUSP" if p.suspicious(i) else "OVLP"} {a:#x} {i.mnemonic} {i.op_str}')
    for a in j['insns']:
        del p.insns[a]
