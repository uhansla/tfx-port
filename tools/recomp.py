"""Static recompiler: Watcom/DOS4GW LE executable -> C source.

Every guest function becomes a C function operating on the global guest CPU
state declared in runtime/cpu.h.  Guest memory is a flat byte array (MEM);
guest addresses are 32-bit linear addresses inside it.

usage: recomp.py <exe> <outdir> [--prefix name]
"""
import argparse
import os
import struct
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))    # the other tools, also for a Python that ignores the script's folder

from capstone import x86 as X

from analyze import Program, JCC

# ----------------------------------------------------------------------------
# register helpers
R32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
R16 = {'ax': 'eax', 'cx': 'ecx', 'dx': 'edx', 'bx': 'ebx', 'sp': 'esp', 'bp': 'ebp', 'si': 'esi',
       'di': 'edi'}
R8L = {'al': 'eax', 'cl': 'ecx', 'dl': 'edx', 'bl': 'ebx'}
R8H = {'ah': 'eax', 'ch': 'ecx', 'dh': 'edx', 'bh': 'ebx'}
SEGS = ['es', 'cs', 'ss', 'ds', 'fs', 'gs']

CT = {1: 'uint8_t', 2: 'uint16_t', 4: 'uint32_t', 8: 'uint64_t'}
ST = {1: 'int8_t', 2: 'int16_t', 4: 'int32_t', 8: 'int64_t'}
MASK = {1: '0xffu', 2: '0xffffu', 4: '0xffffffffu'}
SIGN = {1: '0x80u', 2: '0x8000u', 4: '0x80000000u'}
SZI = {1: 0, 2: 1, 4: 2}

# eflags bits
CF, PF, AF, ZF, SF, OF = 1, 4, 0x10, 0x40, 0x80, 0x800
ALLF = CF | PF | AF | ZF | SF | OF

LF = {'ADD': 0, 'ADC': 1, 'SUB': 2, 'SBB': 3, 'LOGIC': 4, 'INC': 5, 'DEC': 6, 'NEG': 7,
      'SHL': 8, 'SHR': 9, 'SAR': 10, 'MUL': 11, 'FLAGS': 12}


def hx(v):
    return f'0x{v & 0xffffffff:x}u'


class Unsupported(Exception):
    pass


# flag read/write sets by mnemonic (base mnemonic, no prefixes)
def flag_rw(ins):
    m = base_mn(ins)
    if m in ('add', 'sub', 'cmp', 'neg', 'xadd', 'cmpsb', 'cmpsw', 'cmpsd', 'scasb', 'scasw',
             'scasd'):
        return 0, ALLF
    if m in ('adc', 'sbb'):
        return CF, ALLF
    if m in ('and', 'or', 'xor', 'test'):
        return 0, ALLF
    if m in ('inc', 'dec'):
        return 0, ALLF & ~CF
    if m in ('shl', 'sal', 'shr', 'sar', 'shld', 'shrd'):
        cnt = ins.operands[-1]
        if cnt.type == X.X86_OP_IMM and (cnt.imm & 31) == 0:
            return 0, 0
        if cnt.type == X.X86_OP_IMM:
            return 0, ALLF
        return ALLF, ALLF          # count may be 0 -> flags preserved
    if m in ('rol', 'ror'):
        return ALLF, CF | OF
    if m in ('rcl', 'rcr'):
        return ALLF, CF | OF
    if m in ('mul', 'imul'):
        return 0, ALLF
    if m in ('div', 'idiv'):
        return 0, ALLF
    if m in ('bt', 'bts', 'btr', 'btc'):
        return 0, CF
    if m in ('bsr', 'bsf'):
        return 0, ALLF
    if m in ('stc', 'clc', 'cmc'):
        return (CF if m == 'cmc' else 0), CF
    if m == 'sahf':
        return 0, CF | PF | AF | ZF | SF
    if m in ('lahf', 'pushfd', 'pushf'):
        return ALLF, 0
    if m in ('popfd', 'popf', 'iretd', 'iret'):
        return 0, ALLF
    if m in ('daa', 'das', 'aaa', 'aas'):
        return CF | AF, ALLF
    if m in ('aam', 'aad'):
        return 0, ALLF
    if m.startswith('set') or m.startswith('cmov'):
        return cond_reads(m[3:] if m.startswith('set') else m[4:]), 0
    if m in ('loope', 'loopne', 'loopz', 'loopnz'):
        return ZF, 0
    if m.startswith('j') and ins.id in JCC:
        if m in ('jcxz', 'jecxz'):
            return 0, 0
        return cond_reads(m[1:]), 0
    if m in ('int',):
        return ALLF, ALLF
    if m in ('call',):
        return 0, ALLF
    if m in ('fcomi', 'fucomi', 'fcomip', 'fucomip'):
        return 0, ALLF
    return 0, 0


def cond_reads(cc):
    return {'o': OF, 'no': OF, 'b': CF, 'ae': CF, 'e': ZF, 'ne': ZF, 'be': CF | ZF, 'a': CF | ZF,
            's': SF, 'ns': SF, 'p': PF, 'np': PF, 'l': SF | OF, 'ge': SF | OF,
            'le': ZF | SF | OF, 'g': ZF | SF | OF}.get(cc, ALLF)


def base_mn(ins):
    m = ins.mnemonic
    for p in ('rep ', 'repe ', 'repne ', 'notrack ', 'lock '):
        if m.startswith(p):
            m = m[len(p):]
    return m


def rep_prefix(ins):
    """REP/REPE/REPNE from the raw prefix bytes (capstone hides F2 on movs/stos)."""
    p = 0
    for b in ins.bytes:
        if b in (0xf2, 0xf3):
            p = b
        elif b not in (0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0x66, 0x67, 0xf0):
            break
    m = base_mn(ins)
    if p == 0xf3:
        return 'repe' if m.startswith(('cmps', 'scas')) else 'rep'
    if p == 0xf2:
        return 'repne' if m.startswith(('cmps', 'scas')) else 'rep'
    return None


# ----------------------------------------------------------------------------
class FuncGen:
    def __init__(self, gen, entry):
        self.g = gen
        self.p = gen.p
        self.entry = entry
        self.out = []
        self.tmp = 0

    # -- operand access ---------------------------------------------------
    def reg(self, name):
        if name in R32:
            return name
        if name in R16:
            return f'((uint16_t){R16[name]})'
        if name in R8L:
            return f'((uint8_t){R8L[name]})'
        if name in R8H:
            return f'((uint8_t)({R8H[name]} >> 8))'
        if name in SEGS:
            return f'((uint32_t){name})'
        raise Unsupported('reg ' + name)

    def set_reg(self, name, val):
        if name in R32:
            return f'{name} = (uint32_t)({val});'
        if name in R16:
            r = R16[name]
            return f'{r} = ({r} & 0xffff0000u) | (uint16_t)({val});'
        if name in R8L:
            r = R8L[name]
            return f'{r} = ({r} & 0xffffff00u) | (uint8_t)({val});'
        if name in R8H:
            r = R8H[name]
            return f'{r} = ({r} & 0xffff00ffu) | ((uint32_t)(uint8_t)({val}) << 8);'
        if name in SEGS:
            return f'rt_load_seg({SEGS.index(name)}, (uint16_t)({val}));'
        raise Unsupported('setreg ' + name)

    def rn(self, regid):
        return self.ins.reg_name(regid)

    def ea(self, op, with_seg=True):
        m = op.mem
        parts = []
        if m.base:
            b = self.rn(m.base)
            if b not in R32:
                raise Unsupported('16-bit addressing')
            parts.append(b)
        if m.index:
            i = self.rn(m.index)
            parts.append(f'{i}*{m.scale}' if m.scale != 1 else i)
        if m.disp or not parts:
            parts.append(hx(m.disp))
        e = ' + '.join(parts)
        if len(parts) > 1:
            e = f'(uint32_t)({e})'
        if not with_seg:
            return e
        seg = self.segof(op)
        return f'{seg}_base + {e}' if seg != 'nobase' else e

    def segof(self, op):
        m = op.mem
        if m.segment:
            return self.rn(m.segment)
        if m.base and self.rn(m.base) in ('esp', 'ebp'):
            return 'ss'
        return 'ds'

    def rd(self, op, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.reg(self.rn(op.reg))
        if op.type == X.X86_OP_IMM:
            return hx(op.imm & int(MASK[size][:-1], 16))
        if op.type == X.X86_OP_MEM:
            fn = {1: 'RB', 2: 'RW', 4: 'RD', 8: 'RQ'}[size]
            return f'{fn}({self.ea(op)})'
        raise Unsupported('op type')

    def wr(self, op, val, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.set_reg(self.rn(op.reg), val)
        if op.type == X.X86_OP_MEM:
            fn = {1: 'WB', 2: 'WW', 4: 'WD', 8: 'WQ'}[size]
            return f'{fn}({self.ea(op)}, {val});'
        raise Unsupported('write op type')

    # -- emission helpers ---------------------------------------------------
    def emit(self, s):
        self.out.append('    ' + s)

    def t(self):
        self.tmp += 1
        return f't{self.tmp}'

    def setf(self, kind, size, a, b, res, cf='0'):
        """Store lazy flag state (only if some written flag is live)."""
        if not self.flags_live:
            return ''
        op = LF[kind] * 4 + SZI[size]
        return f'LF_SET({op}, {a}, {b}, {res}, {cf});'

    # -- function generation ----------------------------------------------
    def traverse(self):
        """Collect instruction addresses belonging to this function."""
        p = self.p
        seen = set()
        work = [self.entry]
        targets = set([self.entry])
        while work:
            a = work.pop()
            while a not in seen:
                ins = p.insns.get(a)
                if ins is None:
                    break
                seen.add(a)
                nxt = a + ins.size
                iid = ins.id
                if iid == X.X86_INS_CALL:
                    if nxt in p.noreturn_sites or nxt not in p.insns:
                        break
                    a = nxt
                    continue
                if iid == X.X86_INS_JMP:
                    op = ins.operands[0]
                    if op.type == X.X86_OP_IMM:
                        for t2 in p.smc_jcc_targets.get(a, ()):     # a near jump with a patched rel32
                            if not (t2 in p.func_entries and t2 != self.entry):
                                targets.add(t2)
                                work.append(t2)
                        t = op.imm
                        if t in p.func_entries and t != self.entry:
                            break
                        targets.add(t)
                        work.append(t)
                    elif a in p.jump_tables:
                        for t in p.jump_tables[a][1]:
                            targets.add(t)
                            work.append(t)
                    break
                if iid in JCC:
                    t = ins.operands[0].imm
                    targets.add(t)
                    work.append(t)
                    for t2 in p.smc_jcc_targets.get(a, ()):
                        targets.add(t2)
                        work.append(t2)
                    a = nxt
                    continue
                if iid in (X.X86_INS_RET, X.X86_INS_RETF, X.X86_INS_IRETD, X.X86_INS_IRET,
                           X.X86_INS_HLT):
                    break
                for t2 in p.smc_jcc_targets.get(a, ()):     # branch patched over this insn
                    targets.add(t2)
                    work.append(t2)
                a = nxt
        self.addrs = sorted(seen)
        self.labels = targets

    def successors(self, a):
        p = self.p
        ins = p.insns[a]
        nxt = a + ins.size
        iid = ins.id
        if iid == X.X86_INS_CALL:
            return [nxt] if nxt in self.addr_set else []
        if iid == X.X86_INS_JMP:
            op = ins.operands[0]
            if op.type == X.X86_OP_IMM:
                return [t for t in [op.imm] + list(p.smc_jcc_targets.get(a, ())) if t in self.addr_set and not (
                    t in p.func_entries and t != self.entry)]
            if a in p.jump_tables:
                return [t for t in p.jump_tables[a][1] if t in self.addr_set]
            return []
        if iid in JCC:
            extra = list(p.smc_jcc_targets.get(a, ()))
            return [x for x in [ins.operands[0].imm, nxt] + extra if x in self.addr_set]
        if iid in (X.X86_INS_RET, X.X86_INS_RETF, X.X86_INS_IRETD, X.X86_INS_IRET, X.X86_INS_HLT):
            return []
        extra = [t for t in p.smc_jcc_targets.get(a, ()) if t in self.addr_set]
        return ([nxt] if nxt in self.addr_set else []) + extra

    def is_exit(self, a):
        """Instruction leaves the function with flags possibly observed."""
        ins = self.p.insns[a]
        iid = ins.id
        if iid in (X.X86_INS_RET, X.X86_INS_RETF, X.X86_INS_IRETD, X.X86_INS_IRET):
            return True
        if iid == X.X86_INS_JMP:
            op = ins.operands[0]
            if op.type == X.X86_OP_IMM:
                return op.imm in self.p.func_entries and op.imm != self.entry
            return a not in self.p.jump_tables
        if iid == X.X86_INS_CALL:
            nxt = a + ins.size
            return nxt not in self.addr_set
        return False

    def liveness(self):
        """Backward dataflow: flags live after each instruction."""
        rw = {a: flag_rw(self.p.insns[a]) for a in self.addrs}
        succ = {a: self.successors(a) for a in self.addrs}
        live_in = {a: 0 for a in self.addrs}
        live_out = {a: 0 for a in self.addrs}
        changed = True
        order = list(reversed(self.addrs))
        while changed:
            changed = False
            for a in order:
                out = 0
                if self.is_exit(a):
                    out = ALLF
                for s in succ[a]:
                    out |= live_in[s]
                r, w = rw[a]
                inn = r | (out & ~w)
                if out != live_out[a] or inn != live_in[a]:
                    live_out[a] = out
                    live_in[a] = inn
                    changed = True
        self.live_out = live_out
        self.rw = rw

    def generate(self):
        self.traverse()
        self.addr_set = set(self.addrs)
        self.liveness()
        name = self.g.fname(self.entry)
        # ret_: the return address our C caller expects this guest code to
        # `ret` to (the pushed address for calls; inherited for tail jumps)
        self.out.append(f'void {name}(uint32_t ret_)\n{{')
        if self.entry in self.g.interp_funcs:
            # debug: run this function in the interpreter (differential testing)
            self.out.append(f'    rt_interpret_func(0x{self.entry:x}u, ret_);\n}}\n')
            return '\n'.join(self.out)
        self.emit(f'INTERP_CHECK(0x{self.entry:x}u, ret_);')
        self.emit(f'FTRACE(0x{self.entry:x}u);')
        if self.entry in self.g.entry_hooks:
            self.emit(self.g.entry_hooks[self.entry])
        self.emit('ESP_ENTRY();')
        if self.addrs and self.addrs[0] != self.entry:
            self.emit(f'goto L_{self.entry:x};')
            self.labels.add(self.entry)
        self.last_setter = None    # (kind, size) of last flag setter in current block
        prev_end = None
        for idx, a in enumerate(self.addrs):
            ins = self.p.insns[a]
            self.ins = ins
            if a in self.labels or (prev_end is not None and prev_end != a):
                self.out.append(f'L_{a:x}:;')
                self.last_setter = None
            if a in self.g.trace_addrs:
                self.emit(f'rt_trace(0x{a:x}u);')
            w = self.rw[a][1]
            self.flags_live = bool(w & self.live_out[a])
            self.flags_out = self.live_out[a]
            if self.needs_eip(ins):
                self.emit(f'rt_eip = 0x{a:x}u;')
            if a in self.p.smc_insns and ins.id not in JCC and ins.id not in (
                    X.X86_INS_JMP, X.X86_INS_CALL, X.X86_INS_RET):
                # patched at run time by the program: execute the live bytes
                if a in self.p.smc_jcc_targets:
                    # may currently hold a short branch written over it
                    cases = ' '.join(f'case 0x{t:x}u: goto L_{t:x};' for t in self.p.smc_jcc_targets[a]
                                     if t in self.addr_set)
                    for t in self.p.smc_jcc_targets[a]:
                        if t in self.addr_set:
                            self.labels.add(t)
                    self.emit(f'{{ uint8_t op_ = RB(0x{a:x}u); if (op_ == 0xeb || (op_ >= 0x70 && op_ <= 0x7f)) {{ '
                              f'if (rt_jcc_live(op_, 0x{a:x}u)) {{ uint32_t t_ = 0x{a + 2:x}u + (uint32_t)(int8_t)RB(0x{a + 1:x}u); '
                              f'IRQ_CHECK(); switch (t_) {{ {cases} default: rt_call(t_, ret_); return; }} }} '
                              f'rt_call(0x{a + 2:x}u, ret_); return; }} }}')
                self.emit(f'rt_step(0x{a:x}u);  /* self-modified: {ins.mnemonic} {ins.op_str} */')
                self.last_setter = None
                nxt = a + ins.size
                nxt_emitted = self.addrs[idx + 1] if idx + 1 < len(self.addrs) else None
                if nxt in self.addr_set:
                    if nxt_emitted != nxt:
                        self.emit(f'goto L_{nxt:x};')
                        self.labels.add(nxt)
                else:
                    self.emit(f'rt_fallthrough(0x{nxt:x}u); return;')
                prev_end = nxt
                continue
            try:
                self.translate(ins)
            except Unsupported as e:
                self.emit(f'rt_unimpl(0x{a:x}u, "{ins.mnemonic} {ins.op_str}");  /* {e} */')
                self.g.unsupported[ins.mnemonic] += 1
            nxt = a + ins.size
            falls = self.falls_through(ins)
            nxt_emitted = self.addrs[idx + 1] if idx + 1 < len(self.addrs) else None
            if falls:
                if nxt in self.addr_set:
                    if nxt_emitted != nxt:
                        self.emit(f'goto L_{nxt:x};')
                        self.labels.add(nxt)
                else:
                    # fell off into unknown code
                    self.emit(f'rt_fallthrough(0x{nxt:x}u); return;')
                prev_end = nxt
            else:
                prev_end = None
        self.out.append('}\n')
        return '\n'.join(self.out)

    def needs_eip(self, ins):
        m = base_mn(ins)
        if m in ('int', 'in', 'out', 'les', 'lds', 'lss', 'lfs', 'lgs', 'retf', 'iretd', 'hlt',
                 'div', 'idiv', 'insb', 'outsb', 'into', 'int3'):
            return True
        if m in ('mov', 'pop') and ins.operands and ins.operands[0].type == X.X86_OP_REG and                 ins.reg_name(ins.operands[0].reg) in SEGS:
            return True
        if m in ('call', 'jmp') and ins.operands and ins.operands[0].type != X.X86_OP_IMM:
            return True
        return False

    def falls_through(self, ins):
        iid = ins.id
        if iid in (X.X86_INS_JMP, X.X86_INS_RET, X.X86_INS_RETF, X.X86_INS_IRETD, X.X86_INS_IRET,
                   X.X86_INS_HLT):
            return False
        if iid == X.X86_INS_CALL:
            nxt = ins.address + ins.size
            return nxt in self.addr_set
        return True

    # -- control flow --------------------------------------------------------
    def goto(self, t):
        """Emit a jump to t (label in this function or tail call)."""
        p = self.p
        back = '' if t > self.ins.address else 'IRQ_CHECK(); '
        if t in self.addr_set and not (t in p.func_entries and t != self.entry):
            return f'{back}goto L_{t:x};'
        if t in p.func_entries:
            return f'{self.g.fname(t)}(ret_); return;'
        return f'rt_call(0x{t:x}u, ret_); return;'

    def cond(self, cc):
        """C expression for condition code cc, using known flag setter if any."""
        ls = self.last_setter
        if ls:
            kind, size = ls
            sm = MASK[size]
            sg = ST[size]
            if kind == 'SUB':
                fast = {
                    'e': 'lf_res == 0', 'ne': 'lf_res != 0',
                    'b': 'lf_a < lf_b', 'ae': 'lf_a >= lf_b',
                    'be': 'lf_a <= lf_b', 'a': 'lf_a > lf_b',
                    'l': f'({sg})lf_a < ({sg})lf_b', 'ge': f'({sg})lf_a >= ({sg})lf_b',
                    'le': f'({sg})lf_a <= ({sg})lf_b', 'g': f'({sg})lf_a > ({sg})lf_b',
                    's': f'(lf_res & {SIGN[size]}) != 0', 'ns': f'(lf_res & {SIGN[size]}) == 0',
                }
                if cc in fast:
                    return fast[cc]
            elif kind == 'LOGIC':
                fast = {
                    'e': 'lf_res == 0', 'ne': 'lf_res != 0', 'be': 'lf_res == 0', 'a': 'lf_res != 0',
                    'b': '0', 'ae': '1', 'o': '0', 'no': '1',
                    's': f'(lf_res & {SIGN[size]}) != 0', 'ns': f'(lf_res & {SIGN[size]}) == 0',
                    'l': f'({sg})lf_res < 0', 'ge': f'({sg})lf_res >= 0',
                    'le': f'({sg})lf_res <= 0', 'g': f'({sg})lf_res > 0',
                }
                if cc in fast:
                    return fast[cc]
            elif kind in ('ADD', 'INC', 'DEC'):
                fast = {'e': 'lf_res == 0', 'ne': 'lf_res != 0',
                        's': f'(lf_res & {SIGN[size]}) != 0', 'ns': f'(lf_res & {SIGN[size]}) == 0'}
                if cc in fast:
                    return fast[cc]
        return f'CC_{cc.upper()}()'

    # -- main translator -----------------------------------------------------
    def translate(self, ins):
        m = base_mn(ins)
        ops = ins.operands
        a = ins.address
        nxt = a + ins.size
        e = self.emit
        h = getattr(self, 'i_' + m, None)
        if h is not None:
            return h(ins, ops)
        if ins.bytes[0] in range(0xd8, 0xe0) or (ins.bytes[0] in (0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65,
                                                                     0x66, 0x9b)
                                                  and m.startswith('f')):
            return self.x87(ins)
        if ins.id in JCC:
            return self.i_jcc(ins, ops)
        if m.startswith('set'):
            cc = m[3:]
            cc = {'z': 'e', 'nz': 'ne', 'c': 'b', 'nc': 'ae'}.get(cc, cc)
            e(self.wr(ops[0], f'({self.cond(cc)}) ? 1 : 0'))
            return
        raise Unsupported(m)

    # data movement
    def i_mov(self, ins, ops):
        d, s = ops
        if d.type == X.X86_OP_REG and self.rn(d.reg) in SEGS:
            self.emit(self.set_reg(self.rn(d.reg), self.rd(s, 2)))
            return
        if s.type == X.X86_OP_REG and self.rn(s.reg) in SEGS:
            if d.type == X.X86_OP_MEM:
                self.emit(self.wr(d, self.reg(self.rn(s.reg)), 2))
            else:
                # 32-bit dest: zero-extended on 386+ for register targets
                self.emit(self.wr(d, self.reg(self.rn(s.reg))))
            return
        self.emit(self.wr(d, self.rd(s, d.size)))

    def i_movzx(self, ins, ops):
        self.emit(self.wr(ops[0], self.rd(ops[1])))

    def i_movsx(self, ins, ops):
        d, s = ops
        self.emit(self.wr(d, f'({CT[d.size]})({ST[s.size]})({self.rd(s)})'))

    def i_lea(self, ins, ops):
        self.emit(self.wr(ops[0], self.ea(ops[1], with_seg=False)))

    def i_xchg(self, ins, ops):
        d, s = ops
        t = self.t()
        self.emit(f'{{ {CT[d.size]} {t} = {self.rd(d)}; {self.wr(d, self.rd(s, d.size))} '
                  f'{self.wr(s, t, d.size)} }}')

    def i_nop(self, ins, ops):
        pass

    i_fnop = i_nop

    def i_push(self, ins, ops):
        s = ops[0]
        if s.type == X.X86_OP_REG and self.rn(s.reg) in SEGS:
            self.emit(f'PUSH32({self.reg(self.rn(s.reg))});')
            return
        if s.size == 2:
            self.emit(f'PUSH16({self.rd(s)});')
            return
        if s.type == X.X86_OP_IMM:
            self.emit(f'PUSH32({hx(s.imm)});')
            return
        self.emit(f'PUSH32({self.rd(s)});')

    def i_pop(self, ins, ops):
        d = ops[0]
        t = self.t()
        if d.type == X.X86_OP_REG and self.rn(d.reg) in SEGS:
            self.emit(f'{{ uint32_t {t} = POP32(); {self.set_reg(self.rn(d.reg), t)} }}')
            return
        if d.size == 2:
            self.emit(f'{{ uint16_t {t} = POP16(); {self.wr(d, t)} }}')
            return
        self.emit(f'{{ uint32_t {t} = POP32(); {self.wr(d, t)} }}')

    def i_pushal(self, ins, ops):
        self.emit('{ uint32_t t = esp; PUSH32(eax); PUSH32(ecx); PUSH32(edx); PUSH32(ebx); '
                  'PUSH32(t); PUSH32(ebp); PUSH32(esi); PUSH32(edi); }')

    def i_popal(self, ins, ops):
        self.emit('{ edi = POP32(); esi = POP32(); ebp = POP32(); esp += 4; ebx = POP32(); '
                  'edx = POP32(); ecx = POP32(); eax = POP32(); }')

    def i_pushfd(self, ins, ops):
        self.emit('PUSH32(rt_get_eflags());')

    def i_pushf(self, ins, ops):
        self.emit('PUSH16((uint16_t)rt_get_eflags());')

    # Instructions that load the flags explicitly must clear last_setter, or a
    # following jcc would use the fast path of an earlier cmp/test.
    def i_popfd(self, ins, ops):
        self.emit('rt_set_eflags(POP32());')
        self.last_setter = None

    def i_popf(self, ins, ops):
        self.emit('rt_set_eflags((rt_get_eflags() & 0xffff0000u) | POP16());')
        self.last_setter = None

    def i_lahf(self, ins, ops):
        self.emit(self.set_reg('ah', '(rt_get_eflags() & 0xd5u) | 2u'))

    def i_sahf(self, ins, ops):
        self.emit('rt_set_arith_flags((rt_get_eflags() & ~0xd5u) | (((eax >> 8)) & 0xd5u));')
        self.last_setter = None

    def i_cwde(self, ins, ops):
        self.emit('eax = (uint32_t)(int32_t)(int16_t)eax;')

    def i_cbw(self, ins, ops):
        self.emit(self.set_reg('ax', '(uint16_t)(int16_t)(int8_t)eax'))

    def i_cdq(self, ins, ops):
        self.emit('edx = ((int32_t)eax < 0) ? 0xffffffffu : 0u;')

    def i_cwd(self, ins, ops):
        self.emit(self.set_reg('dx', '((int16_t)eax < 0) ? 0xffffu : 0u'))

    def ptr_load(self, ins, ops, seg):
        d, s = ops
        t = self.t()
        ea = self.ea(s)
        self.emit(f'{{ uint32_t {t} = {ea}; uint32_t o_ = RD({t}); uint16_t s_ = RW({t} + 4); '
                  f'rt_load_seg({SEGS.index(seg)}, s_); {self.set_reg(self.rn(d.reg), "o_")} }}')

    def i_les(self, ins, ops): self.ptr_load(ins, ops, 'es')
    def i_lds(self, ins, ops): self.ptr_load(ins, ops, 'ds')
    def i_lss(self, ins, ops): self.ptr_load(ins, ops, 'ss')
    def i_lfs(self, ins, ops): self.ptr_load(ins, ops, 'fs')
    def i_lgs(self, ins, ops): self.ptr_load(ins, ops, 'gs')

    def i_xlatb(self, ins, ops):
        seg = 'ds'
        if ins.prefix[1]:
            seg = {0x26: 'es', 0x2e: 'cs', 0x36: 'ss', 0x3e: 'ds', 0x64: 'fs', 0x65: 'gs'}[ins.prefix[1]]
        self.emit(self.set_reg('al', f'RB({seg}_base + (uint32_t)(ebx + (uint8_t)eax))'))

    # arithmetic
    def arith(self, ins, ops, kind, store=True):
        d, s = ops
        sz = d.size
        t = self.t()
        a = self.rd(d)
        b = self.rd(s, sz)
        cty = CT[sz]
        if d.type == X.X86_OP_MEM:
            ea = self.ea(d)
            fn = {1: 'B', 2: 'W', 4: 'D'}[sz]
            pre = f'uint32_t ea_ = {ea}; {cty} a_ = R{fn}(ea_);'
            wr = f'W{fn}(ea_, r_);'
        else:
            pre = f'{cty} a_ = {a};'
            wr = self.wr(d, 'r_')
        body = ''
        if kind == 'ADD':
            body = f'{cty} b_ = {b}; {cty} r_ = a_ + b_;'
            fl = self.setf('ADD', sz, 'a_', 'b_', 'r_')
        elif kind == 'SUB':
            body = f'{cty} b_ = {b}; {cty} r_ = a_ - b_;'
            fl = self.setf('SUB', sz, 'a_', 'b_', 'r_')
        elif kind == 'ADC':
            body = f'{cty} b_ = {b}; uint32_t c_ = CC_B(); {cty} r_ = a_ + b_ + c_;'
            fl = self.setf('ADC', sz, 'a_', 'b_', 'r_', 'c_')
        elif kind == 'SBB':
            body = f'{cty} b_ = {b}; uint32_t c_ = CC_B(); {cty} r_ = a_ - b_ - c_;'
            fl = self.setf('SBB', sz, 'a_', 'b_', 'r_', 'c_')
        elif kind in ('AND', 'OR', 'XOR'):
            opc = {'AND': '&', 'OR': '|', 'XOR': '^'}[kind]
            if kind == 'XOR' and d.type == X.X86_OP_REG and s.type == X.X86_OP_REG and d.reg == s.reg:
                body = f'{cty} r_ = 0;'
                pre = ''
            else:
                body = f'{cty} r_ = a_ {opc} ({cty})({b});'
            fl = self.setf('LOGIC', sz, '0', '0', 'r_')
        else:
            raise Unsupported(kind)
        if store:
            self.emit(f'{{ {pre} {body} {fl} {wr} }}')
        else:
            self.emit(f'{{ {pre} {body} {fl} }}')
        self.last_setter = ('LOGIC' if kind in ('AND', 'OR', 'XOR') else kind, sz)

    def i_add(self, ins, ops): self.arith(ins, ops, 'ADD')
    def i_sub(self, ins, ops): self.arith(ins, ops, 'SUB')
    def i_adc(self, ins, ops): self.arith(ins, ops, 'ADC')
    def i_sbb(self, ins, ops): self.arith(ins, ops, 'SBB')
    def i_and(self, ins, ops): self.arith(ins, ops, 'AND')
    def i_or(self, ins, ops): self.arith(ins, ops, 'OR')
    def i_xor(self, ins, ops): self.arith(ins, ops, 'XOR')

    def i_cmp(self, ins, ops):
        if not self.flags_live:
            self.last_setter = ('SUB', ops[0].size)
            return
        self.arith(ins, ops, 'SUB', store=False)

    def i_test(self, ins, ops):
        d, s = ops
        sz = d.size
        if self.flags_live:
            self.emit(f'{{ {CT[sz]} r_ = {self.rd(d)} & {self.rd(s, sz)}; '
                      f'{self.setf("LOGIC", sz, "0", "0", "r_")} }}')
        self.last_setter = ('LOGIC', sz)

    def incdec(self, ins, ops, kind):
        d = ops[0]
        sz = d.size
        cty = CT[sz]
        delta = '+ 1' if kind == 'INC' else '- 1'
        cf = 'CC_B()' if (self.flags_out & CF) else '0'
        fl = self.setf(kind, sz, 'a_', '1', 'r_', cf)
        if d.type == X.X86_OP_MEM:
            fn = {1: 'B', 2: 'W', 4: 'D'}[sz]
            self.emit(f'{{ uint32_t ea_ = {self.ea(d)}; {cty} a_ = R{fn}(ea_); {cty} r_ = a_ {delta}; '
                      f'{fl} W{fn}(ea_, r_); }}')
        else:
            self.emit(f'{{ {cty} a_ = {self.rd(d)}; {cty} r_ = a_ {delta}; {fl} {self.wr(d, "r_")} }}')
        self.last_setter = (kind, sz)

    def i_inc(self, ins, ops): self.incdec(ins, ops, 'INC')
    def i_dec(self, ins, ops): self.incdec(ins, ops, 'DEC')

    def i_neg(self, ins, ops):
        d = ops[0]
        sz = d.size
        cty = CT[sz]
        fl = self.setf('NEG', sz, '0', 'a_', 'r_')
        self.emit(f'{{ {cty} a_ = {self.rd(d)}; {cty} r_ = ({cty})(0 - a_); {fl} {self.wr(d, "r_")} }}')
        self.last_setter = ('NEG', sz)

    def i_not(self, ins, ops):
        d = ops[0]
        self.emit(self.wr(d, f'~{self.rd(d)}'))

    def shift(self, ins, ops, kind):
        d = ops[0]
        sz = d.size
        cty = CT[sz]
        bits = sz * 8
        if len(ops) == 1:
            cnt = '1'
        else:
            cnt = self.rd(ops[1], 1) if ops[1].type != X.X86_OP_IMM else str(ops[1].imm & 31)
        if cnt == '0':
            return
        if kind == 'SHL':
            expr = f'({cty})(a_ << n_)'
        elif kind == 'SHR':
            expr = f'({cty})(a_ >> n_)'
        else:
            expr = f'({cty})(({ST[sz]})a_ >> (n_ >= {bits} ? {bits - 1} : n_))'
            if sz == 4:
                expr = f'({cty})(({ST[sz]})a_ >> n_)'
        # for 8/16-bit SHL/SHR with n >= width the result is 0 (C shift is on int)
        fl = self.setf(kind, sz, 'a_', 'n_', 'r_')
        if d.type == X.X86_OP_MEM:
            fn = {1: 'B', 2: 'W', 4: 'D'}[sz]
            ld = f'uint32_t ea_ = {self.ea(d)}; {cty} a_ = R{fn}(ea_);'
            st = f'W{fn}(ea_, r_);'
        else:
            ld = f'{cty} a_ = {self.rd(d)};'
            st = self.wr(d, 'r_')
        if cnt.isdigit():
            self.emit(f'{{ {ld} uint32_t n_ = {cnt}; {cty} r_ = {expr}; {fl} {st} }}')
        else:
            self.emit(f'{{ uint32_t n_ = ({cnt}) & 31; if (n_) {{ {ld} {cty} r_ = {expr}; {fl} {st} }} }}')
        self.last_setter = None

    def i_shl(self, ins, ops): self.shift(ins, ops, 'SHL')
    i_sal = i_shl
    def i_shr(self, ins, ops): self.shift(ins, ops, 'SHR')
    def i_sar(self, ins, ops): self.shift(ins, ops, 'SAR')

    def rot(self, ins, ops, kind):
        d = ops[0]
        sz = d.size
        cnt = '1' if len(ops) == 1 else (self.rd(ops[1], 1) if ops[1].type != X.X86_OP_IMM
                                         else str(ops[1].imm & 31))
        fn = {1: 'B', 2: 'W', 4: 'D'}[sz]
        if d.type == X.X86_OP_MEM:
            self.emit(f'{{ uint32_t ea_ = {self.ea(d)}; W{fn}(ea_, rt_{kind}{sz * 8}(R{fn}(ea_), {cnt})); }}')
        else:
            self.emit(self.wr(d, f'rt_{kind}{sz * 8}({self.rd(d)}, {cnt})'))
        self.last_setter = None

    def i_rol(self, ins, ops): self.rot(ins, ops, 'rol')
    def i_ror(self, ins, ops): self.rot(ins, ops, 'ror')
    def i_rcl(self, ins, ops): self.rot(ins, ops, 'rcl')
    def i_rcr(self, ins, ops): self.rot(ins, ops, 'rcr')

    def shxd(self, ins, ops, kind):
        d, s, c = ops
        sz = d.size
        cnt = self.rd(c, 1) if c.type != X.X86_OP_IMM else str(c.imm & 31)
        fn = {2: 'W', 4: 'D'}[sz]
        if d.type == X.X86_OP_MEM:
            self.emit(f'{{ uint32_t ea_ = {self.ea(d)}; W{fn}(ea_, rt_{kind}{sz * 8}(R{fn}(ea_), '
                      f'{self.rd(s)}, {cnt})); }}')
        else:
            self.emit(self.wr(d, f'rt_{kind}{sz * 8}({self.rd(d)}, {self.rd(s)}, {cnt})'))
        self.last_setter = None

    def i_shld(self, ins, ops): self.shxd(ins, ops, 'shld')
    def i_shrd(self, ins, ops): self.shxd(ins, ops, 'shrd')

    def i_bsr(self, ins, ops):
        d, s = ops
        self.emit(f'{{ uint32_t v_ = {self.rd(s)}; if (v_) {{ {self.wr(d, "31 - __builtin_clz(v_)")} '
                  f'LF_SET({LF["LOGIC"] * 4 + 2}, 0, 0, 1, 0); }} else {{ LF_SET({LF["LOGIC"] * 4 + 2}, 0, 0, 0, 0); }} }}')
        self.last_setter = None

    def i_bsf(self, ins, ops):
        d, s = ops
        self.emit(f'{{ uint32_t v_ = {self.rd(s)}; if (v_) {{ {self.wr(d, "__builtin_ctz(v_)")} '
                  f'LF_SET({LF["LOGIC"] * 4 + 2}, 0, 0, 1, 0); }} else {{ LF_SET({LF["LOGIC"] * 4 + 2}, 0, 0, 0, 0); }} }}')
        self.last_setter = None

    def i_bt(self, ins, ops):
        d, s = ops
        if d.type == X.X86_OP_MEM and s.type == X.X86_OP_REG:
            raise Unsupported('bt mem,reg')
        self.emit(f'rt_set_cf(({self.rd(d)} >> ({self.rd(s)} & {d.size * 8 - 1})) & 1);')
        self.last_setter = None

    def i_imul(self, ins, ops):
        if len(ops) == 1:
            s = ops[0]
            sz = s.size
            if sz == 4:
                self.emit(f'{{ int64_t r_ = (int64_t)(int32_t)eax * (int32_t){self.rd(s)}; eax = (uint32_t)r_; '
                          f'edx = (uint32_t)(r_ >> 32); {self.setf("MUL", 4, "0", "0", "eax", "(r_ != (int32_t)r_)")} }}')
            elif sz == 2:
                self.emit(f'{{ int32_t r_ = (int16_t)eax * (int16_t){self.rd(s)}; '
                          f'{self.set_reg("ax", "r_")} {self.set_reg("dx", "r_ >> 16")} '
                          f'{self.setf("MUL", 2, "0", "0", "(uint16_t)r_", "(r_ != (int16_t)r_)")} }}')
            else:
                self.emit(f'{{ int16_t r_ = (int8_t)eax * (int8_t){self.rd(s)}; '
                          f'{self.set_reg("ax", "r_")} {self.setf("MUL", 1, "0", "0", "(uint8_t)r_", "(r_ != (int8_t)r_)")} }}')
        else:
            d = ops[0]
            if len(ops) == 2:
                a, b = self.rd(d), self.rd(ops[1], d.size)
            else:
                a, b = self.rd(ops[1]), self.rd(ops[2], d.size)
            sz = d.size
            if sz == 4:
                self.emit(f'{{ int64_t r_ = (int64_t)(int32_t)({a}) * (int32_t)({b}); {self.wr(d, "(uint32_t)r_")} '
                          f'{self.setf("MUL", 4, "0", "0", "(uint32_t)r_", "(r_ != (int32_t)r_)")} }}')
            else:
                self.emit(f'{{ int32_t r_ = (int32_t)(int16_t)({a}) * (int16_t)({b}); {self.wr(d, "(uint16_t)r_")} '
                          f'{self.setf("MUL", 2, "0", "0", "(uint16_t)r_", "(r_ != (int16_t)r_)")} }}')
        self.last_setter = None

    def i_mul(self, ins, ops):
        s = ops[0]
        sz = s.size
        if sz == 4:
            self.emit(f'{{ uint64_t r_ = (uint64_t)eax * {self.rd(s)}; eax = (uint32_t)r_; edx = (uint32_t)(r_ >> 32); '
                      f'{self.setf("MUL", 4, "0", "0", "eax", "(edx != 0)")} }}')
        elif sz == 2:
            self.emit(f'{{ uint32_t r_ = (uint32_t)(uint16_t)eax * {self.rd(s)}; {self.set_reg("ax", "r_")} '
                      f'{self.set_reg("dx", "r_ >> 16")} {self.setf("MUL", 2, "0", "0", "(uint16_t)r_", "((r_ >> 16) != 0)")} }}')
        else:
            self.emit(f'{{ uint32_t r_ = (uint32_t)(uint8_t)eax * {self.rd(s)}; {self.set_reg("ax", "r_")} '
                      f'{self.setf("MUL", 1, "0", "0", "(uint8_t)r_", "((r_ >> 8) != 0)")} }}')
        self.last_setter = None

    def i_div(self, ins, ops):
        s = ops[0]
        sz = s.size
        a = hex(ins.address)
        if sz == 4:
            self.emit(f'{{ uint32_t d_ = {self.rd(s)}; uint64_t n_ = ((uint64_t)edx << 32) | eax; '
                      f'if (!d_ || n_ / d_ > 0xffffffffu) rt_div0({a}u); eax = (uint32_t)(n_ / d_); edx = (uint32_t)(n_ % d_); }}')
        elif sz == 2:
            self.emit(f'{{ uint32_t d_ = {self.rd(s)}; uint32_t n_ = ((edx & 0xffff) << 16) | (eax & 0xffff); '
                      f'if (!d_ || n_ / d_ > 0xffffu) rt_div0({a}u); {self.set_reg("ax", "n_ / d_")} {self.set_reg("dx", "n_ % d_")} }}')
        else:
            self.emit(f'{{ uint32_t d_ = {self.rd(s)}; uint32_t n_ = eax & 0xffff; '
                      f'if (!d_ || n_ / d_ > 0xffu) rt_div0({a}u); {self.set_reg("al", "n_ / d_")} {self.set_reg("ah", "n_ % d_")} }}')
        self.last_setter = None

    def i_idiv(self, ins, ops):
        s = ops[0]
        sz = s.size
        a = hex(ins.address)
        if sz == 4:
            self.emit(f'{{ int32_t d_ = (int32_t){self.rd(s)}; int64_t n_ = (int64_t)(((uint64_t)edx << 32) | eax); '
                      f'if (!d_) rt_div0({a}u); int64_t q_ = n_ / d_; if (q_ != (int32_t)q_) rt_div0({a}u); '
                      f'eax = (uint32_t)q_; edx = (uint32_t)(n_ % d_); }}')
        elif sz == 2:
            # a divide the program's own exception handler recovers from: continue where it does
            t = DIV_RECOVER_NOW.get(ins.address)
            fail = f'{{ {self.g.fname(t)}(ret_); return; }}' if t else f'rt_div0({a}u);'
            self.emit(f'{{ int32_t d_ = (int16_t){self.rd(s)}; int32_t n_ = (int32_t)(((edx & 0xffff) << 16) | (eax & 0xffff)); '
                      f'if (!d_ || (d_ == -1 && n_ == INT32_MIN)) {fail} int32_t q_ = n_ / d_; if (q_ != (int16_t)q_) {fail} '
                      f'{self.set_reg("ax", "q_")} {self.set_reg("dx", "n_ % d_")} }}')
        else:
            self.emit(f'{{ int32_t d_ = (int8_t){self.rd(s)}; int32_t n_ = (int16_t)eax; '
                      f'if (!d_) rt_div0({a}u); int32_t q_ = n_ / d_; if (q_ != (int8_t)q_) rt_div0({a}u); '
                      f'{self.set_reg("al", "q_")} {self.set_reg("ah", "n_ % d_")} }}')
        self.last_setter = None

    def i_aam(self, ins, ops):
        base = ops[0].imm if ops else 10
        self.emit(f'{{ uint8_t a_ = (uint8_t)eax; {self.set_reg("ah", f"a_ / {base}")} {self.set_reg("al", f"a_ % {base}")} '
                  f'LF_SET({LF["LOGIC"] * 4}, 0, 0, (uint8_t)eax, 0); }}')
        self.last_setter = None

    def i_aad(self, ins, ops):
        base = ops[0].imm if ops else 10
        self.emit(f'{{ uint8_t r_ = (uint8_t)(((eax >> 8) & 0xff) * {base} + (eax & 0xff)); '
                  f'{self.set_reg("ax", "r_")} LF_SET({LF["LOGIC"] * 4}, 0, 0, r_, 0); }}')
        self.last_setter = None

    def i_daa(self, ins, ops): self.emit('rt_daa();'); self.last_setter = None
    def i_das(self, ins, ops): self.emit('rt_das();'); self.last_setter = None
    def i_aaa(self, ins, ops): self.emit('rt_aaa();'); self.last_setter = None
    def i_aas(self, ins, ops): self.emit('rt_aas();'); self.last_setter = None

    def i_clc(self, ins, ops): self.emit('rt_set_cf(0);'); self.last_setter = None
    def i_stc(self, ins, ops): self.emit('rt_set_cf(1);'); self.last_setter = None
    def i_cmc(self, ins, ops): self.emit('rt_set_cf(!CC_B());'); self.last_setter = None
    def i_cld(self, ins, ops): self.emit('df = 0;')
    def i_std(self, ins, ops): self.emit('df = 1;')
    def i_cli(self, ins, ops): self.emit('rt_cli();')
    def i_sti(self, ins, ops): self.emit('rt_sti();')
    def i_wait(self, ins, ops): pass
    def i_hlt(self, ins, ops): self.emit(f'rt_hlt(0x{ins.address:x}u); return;')

    def i_enter(self, ins, ops):
        n = ops[0].imm
        lvl = ops[1].imm & 31
        if lvl:
            raise Unsupported('enter nesting')
        self.emit(f'PUSH32(ebp); ebp = esp; esp -= {n}u;')

    def i_leave(self, ins, ops):
        self.emit('esp = ebp; ebp = POP32();')

    # control flow
    def i_jcc(self, ins, ops):
        m = base_mn(ins)
        t = ops[0].imm
        a = ins.address
        if a in self.p.smc_jcc_targets:
            # opcode and displacement are rewritten at run time: evaluate the
            # live bytes and dispatch to the resulting target
            cands = sorted(set(self.p.smc_jcc_targets[a]) | {t})
            cases = ' '.join(f'case 0x{x:x}u: goto L_{x:x};' for x in cands if x in self.addr_set)
            for x in cands:
                if x in self.addr_set:
                    self.labels.add(x)
            self.emit(f'{{ uint8_t op_ = RB(0x{a:x}u); int8_t d_ = (int8_t)RB(0x{a + 1:x}u); '
                      f'if (rt_jcc_live(op_, 0x{a:x}u)) {{ uint32_t t_ = 0x{a + 2:x}u + d_; IRQ_CHECK(); '
                      f'switch (t_) {{ {cases} default: rt_call(t_, ret_); return; }} }} }}')
            return
        if m == 'jecxz':
            c = 'ecx == 0'
        elif m == 'jcxz':
            c = '(ecx & 0xffff) == 0'
        elif m == 'loop':
            self.emit(f'if (--ecx) {{ {self.goto(t)} }}')
            return
        elif m in ('loope', 'loopz'):
            self.emit(f'if (--ecx && ({self.cond("e")})) {{ {self.goto(t)} }}')
            return
        elif m in ('loopne', 'loopnz'):
            self.emit(f'if (--ecx && ({self.cond("ne")})) {{ {self.goto(t)} }}')
            return
        else:
            cc = m[1:]
            cc = {'z': 'e', 'nz': 'ne', 'c': 'b', 'nc': 'ae', 'nae': 'b', 'nb': 'ae', 'na': 'be',
                  'nbe': 'a', 'nge': 'l', 'nl': 'ge', 'ng': 'le', 'nle': 'g', 'pe': 'p',
                  'po': 'np'}.get(cc, cc)
            c = self.cond(cc)
        self.emit(f'if ({c}) {{ {self.goto(t)} }}')

    def i_jmp(self, ins, ops):
        op = ops[0]
        a = ins.address
        if op.type == X.X86_OP_IMM and a in self.p.smc_insns and ins.bytes[0] == 0xe9:
            # jump whose rel32 is patched at run time: dispatch on the live value
            nxt = a + ins.size
            cases = ' '.join(f'case 0x{t:x}u: goto L_{t:x};' for t in sorted(self.labels | {op.imm})
                             if t in self.addr_set and not (t in self.p.func_entries and t != self.entry))
            self.labels.add(op.imm) if op.imm in self.addr_set else None
            self.emit(f'{{ uint32_t t_ = 0x{nxt:x}u + RD(0x{a + 1:x}u); IRQ_CHECK(); '
                      f'switch (t_) {{ {cases} default: rt_call(t_, ret_); return; }} }}')
            return
        if op.type == X.X86_OP_IMM:
            self.emit(self.goto(op.imm))
            return
        tgt = self.rd(op, 4)
        if a in self.p.jump_tables:
            cases =' '.join(f'case 0x{t:x}u: goto L_{t:x};' for t in sorted(set(self.p.jump_tables[a][1])))
            self.emit(f'switch ({tgt}) {{ {cases} default: rt_call({tgt}, ret_); return; }}')
            return
        self.emit(f'{{ uint32_t t_ = {tgt}; rt_call(t_, ret_); return; }}')

    def i_call(self, ins, ops):
        op = ops[0]
        nxt = ins.address + ins.size
        if op.type == X.X86_OP_IMM:
            t = op.imm
            call = f'{self.g.fname(t)}(0x{nxt:x}u);' if t in self.p.func_entries else f'rt_call(0x{t:x}u, 0x{nxt:x}u);'
            self.emit(f'PUSH32(0x{nxt:x}u); {call}')
        else:
            self.emit(f'{{ uint32_t t_ = {self.rd(op, 4)}; PUSH32(0x{nxt:x}u); rt_call(t_, 0x{nxt:x}u); }}')
        if nxt not in self.addr_set:
            self.emit('return;')
        else:
            self.emit(f'if (rt_unwind) {{ if (rt_unwind == 0x{nxt:x}u) rt_unwind = 0; else return; }}')
        self.last_setter = None

    def i_ret(self, ins, ops):
        n = ops[0].imm if ops else 0
        self.emit(f'{{ ESP_CHECK(0x{self.entry:x}u); uint32_t r_ = POP32(); esp += {n}u; if (r_ != ret_) rt_ret_mismatch(r_, ret_); return; }}')

    def i_retf(self, ins, ops):
        n = ops[0].imm if ops else 0
        self.emit(f'rt_retf({n}u, ret_); return;')

    def i_iretd(self, ins, ops):
        self.emit('rt_iretd(); return;')

    i_iret = i_iretd

    def i_int(self, ins, ops):
        self.emit(f'rt_int(0x{ops[0].imm:x}u);')
        self.last_setter = None

    def i_int3(self, ins, ops):
        self.emit('rt_int(3);')

    def i_into(self, ins, ops):
        self.emit('if (CC_O()) rt_int(4);')

    # port I/O
    def i_in(self, ins, ops):
        d, s = ops
        port = self.rd(s, 2) if s.type == X.X86_OP_REG else hx(s.imm)
        fn = {1: 'rt_inb', 2: 'rt_inw', 4: 'rt_ind'}[d.size]
        self.emit(self.wr(d, f'{fn}({port})'))

    def i_out(self, ins, ops):
        d, s = ops
        port = self.rd(d, 2) if d.type == X.X86_OP_REG else hx(d.imm)
        fn = {1: 'rt_outb', 2: 'rt_outw', 4: 'rt_outd'}[s.size]
        self.emit(f'{fn}({port}, {self.rd(s)});')

    # string ops
    def strop(self, ins, kind, sz):
        rep = rep_prefix(ins)
        src_seg = 'ds'
        if ins.prefix[1] and kind in ('movs', 'lods', 'cmps', 'outs'):
            src_seg = {0x26: 'es', 0x2e: 'cs', 0x36: 'ss', 0x3e: 'ds', 0x64: 'fs', 0x65: 'gs'}[ins.prefix[1]]
        fn = {1: 'B', 2: 'W', 4: 'D'}[sz]
        cty = CT[sz]
        step = f'(df ? -{sz} : {sz})'
        acc = {1: 'al', 2: 'ax', 4: 'eax'}[sz]
        if kind == 'movs':
            if rep and sz in (1, 4):
                self.emit(f'rt_rep_movs{sz}({src_seg}_base);')
                return
            body = f'W{fn}(es_base + edi, R{fn}({src_seg}_base + esi)); esi += {step}; edi += {step};'
        elif kind == 'stos':
            if rep and sz in (1, 4):
                self.emit(f'rt_rep_stos{sz}();')
                return
            body = f'W{fn}(es_base + edi, ({cty}){self.reg(acc)}); edi += {step};'
        elif kind == 'lods':
            body = f'{self.set_reg(acc, f"R{fn}({src_seg}_base + esi)")} esi += {step};'
        elif kind == 'scas':
            body = (f'{{ {cty} a_ = {self.reg(acc)}; {cty} b_ = R{fn}(es_base + edi); {cty} r_ = a_ - b_; '
                    f'LF_SET({LF["SUB"] * 4 + SZI[sz]}, a_, b_, r_, 0); }} edi += {step};')
        elif kind == 'cmps':
            body = (f'{{ {cty} a_ = R{fn}({src_seg}_base + esi); {cty} b_ = R{fn}(es_base + edi); {cty} r_ = a_ - b_; '
                    f'LF_SET({LF["SUB"] * 4 + SZI[sz]}, a_, b_, r_, 0); }} esi += {step}; edi += {step};')
        elif kind == 'outs':
            body = f'rt_out{fn.lower()}((uint16_t)edx, R{fn}({src_seg}_base + esi)); esi += {step};'
        elif kind == 'ins':
            body = f'W{fn}(es_base + edi, rt_in{fn.lower()}((uint16_t)edx)); edi += {step};'
        else:
            raise Unsupported(kind)
        if not rep:
            self.emit(body)
        elif kind in ('scas', 'cmps'):
            cc = 'lf_res == 0' if rep == 'repe' else 'lf_res != 0'
            self.emit(f'while (ecx) {{ {body} ecx--; if (!({cc})) break; }}')
        else:
            self.emit(f'while (ecx) {{ {body} ecx--; }}')
        self.last_setter = ('SUB', sz) if kind in ('scas', 'cmps') else self.last_setter

    def i_movsb(self, ins, ops): self.strop(ins, 'movs', 1)
    def i_movsw(self, ins, ops): self.strop(ins, 'movs', 2)
    def i_stosb(self, ins, ops): self.strop(ins, 'stos', 1)
    def i_stosw(self, ins, ops): self.strop(ins, 'stos', 2)
    def i_stosd(self, ins, ops): self.strop(ins, 'stos', 4)
    def i_lodsb(self, ins, ops): self.strop(ins, 'lods', 1)
    def i_lodsw(self, ins, ops): self.strop(ins, 'lods', 2)
    def i_lodsd(self, ins, ops): self.strop(ins, 'lods', 4)
    def i_scasb(self, ins, ops): self.strop(ins, 'scas', 1)
    def i_scasw(self, ins, ops): self.strop(ins, 'scas', 2)
    def i_scasd(self, ins, ops): self.strop(ins, 'scas', 4)
    def i_cmpsb(self, ins, ops): self.strop(ins, 'cmps', 1)
    def i_cmpsw(self, ins, ops): self.strop(ins, 'cmps', 2)
    def i_outsb(self, ins, ops): self.strop(ins, 'outs', 1)
    def i_insb(self, ins, ops): self.strop(ins, 'ins', 1)
    def i_outsw(self, ins, ops): self.strop(ins, 'outs', 2)
    def i_outsd(self, ins, ops): self.strop(ins, 'outs', 4)
    def i_insw(self, ins, ops): self.strop(ins, 'ins', 2)
    def i_insd(self, ins, ops): self.strop(ins, 'ins', 4)

    def i_movsd(self, ins, ops):
        self.strop(ins, 'movs', 4)

    def i_cmpsd(self, ins, ops):
        self.strop(ins, 'cmps', 4)

    # ---------------------------------------------------------------- x87
    def x87(self, ins):
        b = ins.bytes
        i = 0
        while b[i] in (0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0x66, 0x67, 0x9b, 0xf2, 0xf3):
            i += 1
        opc = b[i]
        modrm = b[i + 1]
        reg = (modrm >> 3) & 7
        rm = modrm & 7
        e = self.emit
        memop = None
        for op in ins.operands:
            if op.type == X.X86_OP_MEM:
                memop = op
        if modrm < 0xc0:
            ea = self.ea(memop)
            arith = ['+', '*', 'com', 'comp', '-', 'r-', '/', 'r/']
            if opc in (0xd8, 0xdc, 0xda, 0xde):
                src = {0xd8: f'(long double)RF32({ea})', 0xdc: f'(long double)RF64({ea})',
                       0xda: f'(long double)(int32_t)RD({ea})', 0xde: f'(long double)(int16_t)RW({ea})'}[opc]
                k = arith[reg]
                if k == 'com':
                    e(f'fpu_compare(ST(0), {src});')
                elif k == 'comp':
                    e(f'fpu_compare(ST(0), {src}); FPOP();')
                elif k.startswith('r'):
                    e(f'ST(0) = {src} {k[1]} ST(0);')
                else:
                    e(f'ST(0) = ST(0) {k} {src};')
                return
            if opc == 0xd9:
                if reg == 0:
                    e(f'{{ long double v_ = RF32({ea}); FPUSH(v_); }}')
                elif reg == 2:
                    e(f'WF32({ea}, (float)ST(0));')
                elif reg == 3:
                    e(f'WF32({ea}, (float)ST(0)); FPOP();')
                elif reg == 4:
                    e(f'fpu_ldenv({ea});')
                elif reg == 5:
                    e(f'fpu_cw = RW({ea});')
                elif reg == 6:
                    e(f'fpu_stenv({ea});')
                elif reg == 7:
                    e(f'WW({ea}, fpu_cw);')
                else:
                    raise Unsupported('d9 mem')
                return
            if opc == 0xdb:
                if reg == 0:
                    e(f'{{ long double v_ = (int32_t)RD({ea}); FPUSH(v_); }}')
                elif reg == 2:
                    e(f'WD({ea}, (uint32_t)fpu_to_i32(ST(0)));')
                elif reg == 3:
                    e(f'WD({ea}, (uint32_t)fpu_to_i32(ST(0))); FPOP();')
                elif reg == 5:
                    e(f'{{ long double v_ = RF80({ea}); FPUSH(v_); }}')
                elif reg == 7:
                    e(f'WF80({ea}, ST(0)); FPOP();')
                else:
                    raise Unsupported('db mem')
                return
            if opc == 0xdd:
                if reg == 0:
                    e(f'{{ long double v_ = RF64({ea}); FPUSH(v_); }}')
                elif reg == 2:
                    e(f'WF64({ea}, (double)ST(0));')
                elif reg == 3:
                    e(f'WF64({ea}, (double)ST(0)); FPOP();')
                elif reg == 4:
                    e(f'fpu_rstor({ea});')
                elif reg == 6:
                    e(f'fpu_save({ea});')
                elif reg == 7:
                    e(f'WW({ea}, fpu_get_sw());')
                else:
                    raise Unsupported('dd mem')
                return
            if opc == 0xdf:
                if reg == 0:
                    e(f'{{ long double v_ = (int16_t)RW({ea}); FPUSH(v_); }}')
                elif reg == 2:
                    e(f'WW({ea}, (uint16_t)fpu_to_i16(ST(0)));')
                elif reg == 3:
                    e(f'WW({ea}, (uint16_t)fpu_to_i16(ST(0))); FPOP();')
                elif reg == 5:
                    e(f'{{ long double v_ = (int64_t)RQ({ea}); FPUSH(v_); }}')
                elif reg == 7:
                    e(f'WQ({ea}, (uint64_t)fpu_to_i64(ST(0))); FPOP();')
                else:
                    raise Unsupported('df mem')
                return
            raise Unsupported('x87 mem opc')
        # register forms
        si = f'ST({rm})'
        if opc == 0xd8:
            k = ['+', '*', 'com', 'comp', '-', 'r-', '/', 'r/'][reg]
            if k == 'com':
                e(f'fpu_compare(ST(0), {si});')
            elif k == 'comp':
                e(f'fpu_compare(ST(0), {si}); FPOP();')
            elif k.startswith('r'):
                e(f'ST(0) = {si} {k[1]} ST(0);')
            else:
                e(f'ST(0) = ST(0) {k} {si};')
            return
        if opc in (0xdc, 0xde):
            pop = ' FPOP();' if opc == 0xde else ''
            if opc == 0xde and modrm == 0xd9:
                e('fpu_compare(ST(0), ST(1)); FPOP(); FPOP();')
                return
            if reg == 0:
                e(f'{si} = {si} + ST(0);{pop}')
            elif reg == 1:
                e(f'{si} = {si} * ST(0);{pop}')
            elif reg == 4:
                e(f'{si} = ST(0) - {si};{pop}')
            elif reg == 5:
                e(f'{si} = {si} - ST(0);{pop}')
            elif reg == 6:
                e(f'{si} = ST(0) / {si};{pop}')
            elif reg == 7:
                e(f'{si} = {si} / ST(0);{pop}')
            elif reg in (2, 3) and opc == 0xdc:
                e(f'fpu_compare(ST(0), {si});' + (' FPOP();' if reg == 3 else ''))
            else:
                raise Unsupported('dc/de reg')
            return
        if opc == 0xd9:
            if reg == 0:
                e(f'{{ long double v_ = {si}; FPUSH(v_); }}')
                return
            if reg == 1:
                e(f'{{ long double v_ = ST(0); ST(0) = {si}; {si} = v_; }}')
                return
            f = {0xd0: '', 0xe0: 'ST(0) = -ST(0);', 0xe1: 'ST(0) = fabsl(ST(0));',
                 0xe4: 'fpu_compare(ST(0), 0.0L);', 0xe5: 'fpu_xam();',
                 0xe8: 'FPUSH(1.0L);', 0xe9: 'FPUSH(3.321928094887362347870L);',
                 0xea: 'FPUSH(1.442695040888963407360L);', 0xeb: 'FPUSH(3.141592653589793238463L);',
                 0xec: 'FPUSH(0.301029995663981195214L);', 0xed: 'FPUSH(0.693147180559945309417L);',
                 0xee: 'FPUSH(0.0L);',
                 0xf0: 'ST(0) = exp2l(ST(0)) - 1.0L;',
                 0xf1: 'ST(1) = ST(1) * log2l(ST(0)); FPOP();',
                 0xf2: 'ST(0) = tanl(ST(0)); FPUSH(1.0L); fpu_sw &= ~0x400;',
                 0xf3: 'ST(1) = atan2l(ST(1), ST(0)); FPOP();',
                 0xf4: 'fpu_xtract();',
                 0xf5: 'fpu_prem1();',
                 0xf6: 'ftop = (ftop - 1) & 7;', 0xf7: 'ftop = (ftop + 1) & 7;',
                 0xf8: 'fpu_prem();', 0xf9: 'ST(1) = ST(1) * log2l(ST(0) + 1.0L); FPOP();',
                 0xfa: 'ST(0) = sqrtl(ST(0));',
                 0xfb: '{ long double v_ = ST(0); ST(0) = sinl(v_); FPUSH(cosl(v_)); fpu_sw &= ~0x400; }',
                 0xfc: 'ST(0) = fpu_round(ST(0));', 0xfd: 'ST(0) = ldexpl(ST(0), (int)truncl(ST(1)));',
                 0xfe: 'ST(0) = sinl(ST(0)); fpu_sw &= ~0x400;', 0xff: 'ST(0) = cosl(ST(0)); fpu_sw &= ~0x400;'}
            if modrm in f:
                e(f[modrm])
                return
            raise Unsupported(f'd9 {modrm:x}')
        if opc == 0xda and modrm == 0xe9:
            e('fpu_compare(ST(0), ST(1)); FPOP(); FPOP();')
            return
        if opc == 0xdb:
            if modrm == 0xe2:
                e('fpu_sw &= 0x7f00;')
                return
            if modrm == 0xe3:
                e('fpu_init();')
                return
            if modrm in (0xe0, 0xe1, 0xe4):
                return
            raise Unsupported(f'db {modrm:x}')
        if opc == 0xdd:
            if reg == 0:
                return  # ffree: tag only
            if reg == 2:
                e(f'{si} = ST(0);')
                return
            if reg == 3:
                e(f'{si} = ST(0); FPOP();')
                return
            if reg == 4:
                e(f'fpu_compare(ST(0), {si});')
                return
            if reg == 5:
                e(f'fpu_compare(ST(0), {si}); FPOP();')
                return
            raise Unsupported('dd reg')
        if opc == 0xdf and modrm == 0xe0:
            e(self.set_reg('ax', 'fpu_get_sw()'))
            return
        raise Unsupported(f'x87 {opc:x} {modrm:x}')


# ----------------------------------------------------------------------------
class Gen:
    def __init__(self, prog, prefix, trace_addrs=(), interp_funcs=(), entry_hooks=None):
        self.p = prog
        self.interp_funcs = set(interp_funcs)
        self.entry_hooks = dict(entry_hooks or {})
        self.prefix = prefix
        self.unsupported = defaultdict(int)
        self.trace_addrs = set(trace_addrs)

    def fname(self, a):
        return f'f_{a:x}'

    def run(self, outdir, chunk=200):
        os.makedirs(outdir, exist_ok=True)
        # entries that never decoded have no C function: reach them via rt_call
        self.p.func_entries = {f for f in self.p.func_entries if f in self.p.insns}
        funcs = sorted(self.p.func_entries)
        protos = '\n'.join(f'void {self.fname(f)}(uint32_t ret_);' for f in funcs)
        with open(os.path.join(outdir, 'funcs.h'), 'w') as fh:
            fh.write('#pragma once\n#include "cpu.h"\n' + protos + '\n')
        n = 0
        for i in range(0, len(funcs), chunk):
            parts = ['#include "funcs.h"\n']
            for f in funcs[i:i + chunk]:
                parts.append(FuncGen(self, f).generate())
            with open(os.path.join(outdir, f'code_{n:03d}.c'), 'w') as fh:
                fh.write('\n'.join(parts))
            n += 1
        # call continuation addresses (for unwinding mismatched returns)
        conts = sorted(i.address + i.size for i in self.p.insns.values() if i.id == X.X86_INS_CALL)
        with open(os.path.join(outdir, 'conttab.c'), 'w') as fh:
            fh.write('#include <stdint.h>\nconst uint32_t guest_conts[] = {\n')
            for c in conts:
                fh.write(f'    0x{c:x}u,\n')
            fh.write(f'}};\nconst unsigned guest_nconts = {len(conts)};\n')
        # function table
        with open(os.path.join(outdir, 'functab.c'), 'w') as fh:
            fh.write('#include "funcs.h"\n')
            fh.write('const struct func_entry guest_funcs[] = {\n')
            for f in funcs:
                fh.write(f'    {{0x{f:x}u, {self.fname(f)}}},\n')
            fh.write('};\n')
            fh.write(f'const unsigned guest_nfuncs = {len(funcs)};\n')
        # memory image
        self.write_image(os.path.join(outdir, 'image.bin'))
        print(f'{len(funcs)} functions in {n} files')
        if self.unsupported:
            print('unsupported:', dict(self.unsupported))

    def write_image(self, path):
        """image file: header + objects.

        header: 'LEIM', entry, esp, nobj, then per object: base, size, flags
        followed by data.
        """
        le = self.p.le
        with open(path, 'wb') as fh:
            fh.write(b'LEIM')
            fh.write(struct.pack('<III', le.entry, le.stack_top, len(le.objs)))
            for o in le.objs:
                fh.write(struct.pack('<III', o.base, len(o.data), o.flags))
            for o in le.objs:
                fh.write(bytes(o.data))


IMAGE_BASE = 0x00400000

# Entry hooks: a C statement emitted at the entry of a guest function (the
# guest registers hold its inputs there).
#   TFX.DAT 0x435690: reads the tick count for the frame limiter 0x41B998 /
#     0x403BB0, which spins until 2 ticks have passed: rt_idle_hint() lets the
#     host sleep when the polled value stops changing.
#   The 3D polygon primitives (ESI = list of vertex-record offsets, ECX =
#     count; TFX_CODE.md 9.11): scene_prim() captures each polygon with its
#     view-space vertices for the native renderer (runtime/scene.c).
_SCENE_PRIMS = (0x44C904, 0x44CEB8, 0x44D194, 0x44CBE8, 0x466EF4, 0x4675D0, 0x467B84,
                0x44F26A,   # filled disc (sprites such as the pilot's helmet)
                0x44EA99,   # 2D line clipper/rasterizer (3D model lines when called from 0x44B058)
                0x4759A8, 0x4759D4,   # 1x1 / 2x2 points (lights)
                0x454212, 0x454336,   # 2D line rasterisers (HUD; 0x454336 model lines from VM handler 0x44B09C)
                0x45CFA4,             # 1-bit bitmap blit (HUD/MFD text and symbols)
                0x450454,             # 8-bit sprite blit (cockpit art; one-colour sprites: HUD brackets, MFD text)
                0x45658C)             # single-pixel plot (HUD dots)
# Code that is only reached through pointers the analysis can't follow (load
# addresses).  Without an entry here it still runs, in the interpreter, which
# is some 30 times slower.  TFX: the textured span fill (a pixel loop: looking
# down in the virtual cockpit it took over half of the frame time), the
# stippled fills of clouds and effects, and two small pieces of the model
# code.  The cmdfile command P (a sampling profile) and the log's
# "interpreter: entering at" lines show what is interpreted.
EXTRA_ENTRIES = {'TFX.DAT': [0x475334, 0x4726CE, 0x4727FF, 0x47292F, 0x47415A, 0x47424C, 0x4725C0, 0x44C0A0, 0x44AA15]}

# Divide errors the program handles itself.  TFX installs a handler for
# exception 0 (0x448708, set at 0x44863A with int 31h fn 0203h) that looks at
# the faulting address: the two perspective divides of the vertex projection
# (0x4489D3, 0x4489E8: a vertex far off to the side of a near plane overflows
# the 16-bit quotient) continue at 0x4488DF, which drops the stack back to
# the model routine's entry and returns from it (the object isn't drawn); the
# two divides of the visibility test (0x4491A9, 0x4491D1) continue at
# 0x4491DC ("not visible").  Any other address is its fatal error.  The
# generated code calls the continuation as a tail jump.
DIV_RECOVER = {'TFX.DAT': {0x4489D3: 0x4488DF, 0x4489E8: 0x4488DF, 0x4491A9: 0x4491DC, 0x4491D1: 0x4491DC}}
DIV_RECOVER_NOW = {}

ENTRY_HOOKS = {'TFX.DAT': dict([(0x435690, 'rt_idle_hint();  /* polled while waiting: lets the host sleep */'),
                               (0x4203C4, 'rt_fix_chase_ease();  /* chase camera with 1-tick frames (gamefix.c) */'),
                               (0x415DF4, 'rt_joy_frame();  /* controller as the game port joystick (joy.c) */'),
                               (0x420530, 'rt_head_track();  /* virtual cockpit: head tracking (track.c) */'),
                               (0x4488DF, 'rt_div_recovered();  /* a projection overflow: the object is dropped (gamefix.c) */'),
                               (0x41F41C, 'if (rt_fix_pan_rate()) { (void)POP32(); return; }  /* look keys with 1-tick frames (gamefix.c) */'),
                               (0x41F4B4, 'if (rt_fix_pan_rate()) { (void)POP32(); return; }  /* look keys with 1-tick frames (gamefix.c) */'),
                               (0x44915C, 'if (rt_wide_cull()) { (void)POP32(); return; }  /* wide views: visibility test replaced (gamefix.c) */')] +
                               [(a, f'scene_prim(0x{a:x}u);  /* native renderer capture */') for a in _SCENE_PRIMS])}


def load_smc_observed(exe):
    """{link addr: byte} of code bytes seen modified at run time, from
    tools/smc/<exe name>.txt (lines: 'LINKADDR HEXBYTES', see tools/smc_merge.py)."""
    d = {}
    smc_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'smc')
    name = os.path.basename(exe).upper()
    # <EXE>.txt: observed at run time (tools/smc_merge.py); <EXE>.live.txt:
    # kept by hand, bytes the port itself patches (tfx.ini options)
    for path in (os.path.join(smc_dir, name + '.txt'), os.path.join(smc_dir, name + '.live.txt')):
        if not os.path.exists(path):
            continue
        for line in open(path):
            line = line.split('#')[0].split()
            if len(line) == 2:
                a = int(line[0], 16)
                for i, b in enumerate(bytes.fromhex(line[1])):
                    d[a + i] = b
    return d


def cached_analysis(exe, extra):
    """Run (or load from cache) the link-base analysis.

    The cache key covers the executable, the analyzer source and the extra
    entries; only what import_analysis() needs is stored."""
    import hashlib
    import pickle
    from types import SimpleNamespace
    from le import LE
    here = os.path.dirname(os.path.abspath(__file__))
    h = hashlib.sha1(open(exe, 'rb').read())
    h.update(open(os.path.join(here, 'analyze.py'), 'rb').read())
    h.update(repr(sorted(extra)).encode())
    smc = load_smc_observed(exe)
    h.update(repr(sorted(smc.items())).encode())
    cache_dir = os.path.join(here, '..', 'work', 'analysis_cache')
    os.makedirs(cache_dir, exist_ok=True)
    path = os.path.join(cache_dir, h.hexdigest() + '.pkl')
    if os.path.exists(path):
        d = pickle.load(open(path, 'rb'))
        print('using cached analysis', os.path.basename(path))
    else:
        p = Program(exe, extra_entries=extra, smc_observed=smc)
        p.run()
        d = dict(insns=sorted(p.insns), leaders=p.leaders, func_entries=p.func_entries,
                 jump_tables=p.jump_tables, noreturn_sites=p.noreturn_sites,
                 int_stubs=p.int_stubs, indirect=p.indirect, data_ranges=p.data_ranges,
                 smc_insns=p.smc_insns, smc_jcc_targets=p.smc_jcc_targets,
                 synth_bytes=p.synth_bytes)
        pickle.dump(d, open(path, 'wb'))
    le = LE(exe)
    le.load()
    ns = SimpleNamespace(le=le, **d)
    ns.insns = dict.fromkeys(d['insns'])
    return ns


def default_bases(exe):
    """Place LE objects contiguously above 1MB (64K aligned), like DOS/4GW."""
    from le import LE
    le = LE(exe)
    bases = {}
    a = IMAGE_BASE
    for o in le.objs:
        bases[o.idx] = a
        size = max(o.vsize, o.npages * le.page_size)
        a = (a + size + 0xffff) & ~0xffff
    return bases


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('exe')
    ap.add_argument('outdir')
    ap.add_argument('--trace', default='')
    ap.add_argument('--interp', default='', help='comma-separated function entries to interpret')
    ap.add_argument('--base', action='append', default=[],
                    help='objnum=hexbase to relocate an object')
    ap.add_argument('--entry', action='append', default=[], help='extra function entry (hex)')
    a = ap.parse_args()
    bases = default_bases(a.exe)
    for b in a.base:
        k, v = b.split('=')
        bases[int(k)] = int(v, 16)
    # analyse at the link-time addresses (deterministic: the zero bytes of the
    # original addresses help tell data from code), then map onto load bases
    prog = Program(a.exe, bases=bases)

    def to_link(x):
        o = prog.le.obj_at(x)
        return x - o.base + o.link_base if o is not None else x
    extra = [int(x, 16) for x in a.entry] + [to_link(x) for x in EXTRA_ENTRIES.get(os.path.basename(a.exe).upper(), ())]
    link = cached_analysis(a.exe, extra)
    prog.import_analysis(link)
    trace = [int(x, 16) for x in a.trace.split(',') if x]
    interp = [int(x, 16) for x in a.interp.split(',') if x]
    hooks = ENTRY_HOOKS.get(os.path.basename(a.exe).upper(), {})
    DIV_RECOVER_NOW.update(DIV_RECOVER.get(os.path.basename(a.exe).upper(), {}))
    Gen(prog, 'g', trace, interp, hooks).run(a.outdir)


if __name__ == '__main__':
    main()
