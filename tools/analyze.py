"""Recursive-descent disassembly of an LE executable's 32-bit code object.

Builds the set of reachable instructions, basic-block leaders, function entry
points and jump tables.  Used both for reporting and by the recompiler.
"""
import sys
from collections import Counter

import capstone
from capstone import x86 as X

from le import LE

CS = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
CS.detail = True

JCC = {X.X86_INS_JAE, X.X86_INS_JA, X.X86_INS_JBE, X.X86_INS_JB, X.X86_INS_JCXZ, X.X86_INS_JECXZ,
       X.X86_INS_JE, X.X86_INS_JGE, X.X86_INS_JG, X.X86_INS_JLE, X.X86_INS_JL, X.X86_INS_JNE,
       X.X86_INS_JNO, X.X86_INS_JNP, X.X86_INS_JNS, X.X86_INS_JO, X.X86_INS_JP, X.X86_INS_JS,
       X.X86_INS_LOOP, X.X86_INS_LOOPE, X.X86_INS_LOOPNE}
ENDS = {X.X86_INS_RET, X.X86_INS_RETF, X.X86_INS_IRET, X.X86_INS_IRETD, X.X86_INS_HLT}

OK_REP = {'rep movsd', 'rep movsb', 'rep movsw', 'rep stosd', 'rep stosb', 'rep stosw',
          'repne scasb', 'repe cmpsb', 'repe cmpsd', 'repe cmpsw', 'repne scasw',
          'repne scasd', 'repe scasb', 'repne movsb', 'repne movsd', 'repne movsw',
          'repne stosb', 'repne stosd', 'rep insb', 'rep outsb', 'rep insw', 'rep outsw'}
BAD_MN = {'sgdt', 'sidt', 'lgdt', 'lidt', 'lldt', 'sldt', 'ltr', 'str', 'arpl', 'femms', 'bound',
          'into', 'salc', 'hlt', 'icebp',
          'int1', 'int3', 'ljmp', 'lcall', 'insb', 'insd', 'insw', 'outsb', 'outsd', 'outsw',
          'clts', 'invd', 'wbinvd', 'rdmsr', 'wrmsr', 'ud2', 'ud0', 'ud1',
          'lar', 'lsl', 'verr', 'verw', 'rsm', 'sysenter', 'sysexit', 'syscall', 'rdtsc', 'cpuid',
          'smsw'}

# x86 system instructions that real (but rarely executed) Watcom runtime code
# uses; accepted when the code is referenced from a pointer in the data object.
SYS_OK = {'sidt', 'sgdt', 'smsw', 'lar', 'lsl', 'verr', 'verw', 'hlt', 'str', 'sldt', 'clts',
          'int3', 'into', 'bound',
          # string port I/O: the rasterizer 0x439190 sets VGA plane masks with `outsb`
          'insb', 'insw', 'insd', 'outsb', 'outsw', 'outsd'}


class Program:
    def __init__(self, path, bases=None, extra_entries=(), data_ranges=(), smc_observed=None):
        # smc_observed: {link addr: byte value} of code bytes seen modified at run time
        # (tools/tfx_smc_observed.txt, collected with the runtime's `c` command)
        self.smc_observed = dict(smc_observed or {})
        self.le = LE(path)
        self.le.load(bases)
        self.code = [o for o in self.le.objs if o.is_code and o.is_32]
        self.insns = {}             # addr -> capstone insn (committed + current trial)
        self.cov = {}               # byte addr -> insn addr (committed only)
        self.leaders = set()
        self.func_entries = set()
        self.jump_tables = {}       # addr of jmp insn -> (table addr, [targets])
        self.indirect = []          # addr of unresolved indirect jmp/call
        self.bad = []
        self.fixup_at = {f.src: f for f in self.le.fixups}
        self.data_ranges = list(data_ranges)
        self.extra_entries = list(extra_entries)
        self.rejected = set()
        self.rejected_soft = set()
        self.rejected_strong = set()
        self.rejected_gap = set()
        self.strong = set()         # code pointers stored in the data object
        self.soft = set()           # post-call fallthrough addresses (validated)
        self.int_stubs = []
        self.noreturn_sites = set()
        self.smc_insns = {}
        self.smc_jcc_targets = {}
        self.synth_bytes = {}       # synthesized (patched-placeholder) instructions
        self._dcache = {}
        self._j = None              # trial journal
        self._prune = None
        self.pruned = set()         # addresses where pruned traces stopped

    def in_code(self, a):
        for o in self.code:
            if o.base <= a < o.base + o.vsize:
                return True
        return False

    def in_data_range(self, a):
        for lo, hi in self.data_ranges:
            if lo <= a < hi:
                return True
        return False

    # ------------------------------------------------------------------
    def import_analysis(self, other):
        """Adopt the analysis of the same executable loaded at other bases."""
        def mv(x):
            o = other.le.obj_at(x)
            if o is None:
                return x
            return x - o.base + self.le.obj(o.idx).base
        synth = getattr(other, 'synth_bytes', {})
        for a in sorted(other.insns):
            if a in synth:
                t = mv(a)
                self.insns[t] = next(CS.disasm(synth[a], t, 1))
                self.synth_bytes[t] = synth[a]
            else:
                self.decode(mv(a))
        self.leaders = {mv(x) for x in other.leaders}
        self.func_entries = {mv(x) for x in other.func_entries}
        self.jump_tables = {mv(k): (mv(t), [mv(x) for x in tg])
                            for k, (t, tg) in other.jump_tables.items()}
        self.noreturn_sites = {mv(x) for x in other.noreturn_sites}
        self.int_stubs = [mv(x) for x in other.int_stubs]
        self.smc_insns = {mv(k): [mv(x) for x in v] for k, v in other.smc_insns.items()}
        self.smc_jcc_targets = {mv(k): [mv(x) for x in v] for k, v in getattr(other, "smc_jcc_targets", {}).items()}
        self.indirect = [mv(x) for x in other.indirect]
        self.data_ranges = [(mv(lo), mv(lo) + (hi - lo)) for lo, hi in other.data_ranges]
        for a, i in self.insns.items():
            for b in range(a, a + i.size):
                self.cov[b] = a

    def find_int_stubs(self):
        """Watcom int386x(): table of `int N; ret` stubs entered via push/ret."""
        out = []
        for o in self.code:
            d = bytes(o.data[:o.vsize])
            i = 0
            while i < len(d) - 24:
                run = 0
                while True:
                    k = i + 3 * run
                    if k + 3 > len(d):
                        break
                    tri = d[k:k + 3]
                    if (tri[0] == 0xcd and tri[2] == 0xc3) or tri == bytes((0xcc, 0x90, 0xc3)):
                        run += 1
                    else:
                        break
                if run >= 8:
                    out += [o.base + i + 3 * k for k in range(run)]
                    i += 3 * run
                else:
                    i += 1
        return out

    def presynthesize_placeholders(self):
        """Before discovery: find raw stores `mov word ptr [abs32], imm16`
        (66 C7 05) whose target is in code and whose value is a short jump,
        where the target bytes do not decode.  Insert the patched jump so the
        surrounding code can be traced (see synthesize_patched_jumps)."""
        for o in self.code:
            d = bytes(o.data[:o.vsize])
            i = d.find(b'\x66\xc7\x05')
            while i >= 0:
                if i + 9 <= len(d):
                    t = int.from_bytes(d[i + 3:i + 7], 'little')
                    v = d[i + 7:i + 9]
                    if self.in_code(t) and (0x70 <= v[0] <= 0x7f or v[0] == 0xeb):
                        to = self.le.obj_at(t)
                        cur = bytes(to.data[t - to.base:t - to.base + 2])
                        if next(CS.disasm(cur + bytes(14), t, 1), None) is None and t not in self.insns:
                            syn = next(CS.disasm(v, t, 1), None)
                            if syn is not None:
                                self.insns[t] = syn
                                self.synth_bytes[t] = v
                                for b in range(t, t + 2):
                                    self.cov[b] = t
                                self.leaders.add(t)
                i = d.find(b'\x66\xc7\x05', i + 1)

    def run(self):
        self.presynthesize_placeholders()
        self.int_stubs = self.find_int_stubs()
        self.extra_entries += self.int_stubs
        self._trace_commit([self.le.entry] + self.extra_entries, force=True)
        # Discovery in order of evidence strength, so weak guesses can never
        # block real code by being committed first:
        #   1. fall-through after calls, pointers stored in the data object
        #   2. unreached regions that start with a Watcom prologue
        #   3. other code-pointer candidates (immediates, tables in code)
        while True:
            if self._tier_strong():
                continue
            if self._scan_gaps_once():
                continue
            if self._tier_weak():
                continue
            break
        # post-call addresses that never validated: the call does not return
        # there (noreturn function or call-over-data trick)
        self.noreturn_sites = {t for t in self.rejected_soft if t not in self.insns}
        self.find_smc()
        # address-taken code locations that are instruction starts inside
        # other functions (threaded-code fragments, shared tails): make them
        # entries too so control can enter there from the runtime
        for t in self._code_ptr_candidates():
            if t in self.insns:
                self.func_entries.add(t)

    def synthesize_patched_jumps(self):
        """Jump placeholders: code bytes that only become an instruction after
        the program patches them (TFX 3D engine: `FF FF` at link 0x5AAC1 is
        overwritten with `7D 0D` / `7E 1D`...).  For immediate stores into
        code that write a short jump over bytes that are not decoded, decode
        the patched bytes, insert that instruction and trace from there."""
        from capstone import CS_AC_WRITE
        added = []
        for a, ins in list(self.insns.items()):
            imm = [o for o in ins.operands if o.type == X.X86_OP_IMM]
            for op in ins.operands:
                if op.type != X.X86_OP_MEM or not (op.access & CS_AC_WRITE) or not imm:
                    continue
                if op.mem.base or op.mem.index:
                    continue
                t = op.mem.disp & 0xffffffff
                if not self.in_code(t) or t in self.insns or t in self.cov:
                    continue
                val = (imm[0].imm & ((1 << (8 * op.size)) - 1)).to_bytes(op.size, 'little')
                if len(val) < 2 or not (0x70 <= val[0] <= 0x7f or val[0] == 0xeb):
                    continue
                syn = next(CS.disasm(bytes(val[:2]), t, 1), None)
                if syn is None:
                    continue
                self.insns[t] = syn
                self.synth_bytes[t] = bytes(val[:2])
                for b in range(t, t + 2):
                    self.cov[b] = t
                self.leaders.add(t)
                added.append(t)
        for t in added:
            self._trace_commit([t + 2], force=True, is_func=False, prune=True)
        self.synthesized = added

    def find_smc(self):
        """Instructions whose bytes are written by stores to fixed addresses
        (self-modifying code: patched immediates / displacements)."""
        from capstone import CS_AC_WRITE
        self.synthesize_patched_jumps()
        self.smc_insns = {}            # insn addr -> list of writer addrs
        for a, ins in self.insns.items():
            for op in ins.operands:
                if op.type != X.X86_OP_MEM or not (op.access & CS_AC_WRITE):
                    continue
                if op.mem.base or op.mem.index:
                    continue
                t = op.mem.disp & 0xffffffff
                if not self.in_code(t):
                    continue
                for k in range(t, t + max(op.size, 1)):
                    owner = self.cov.get(k)
                    if owner is not None:
                        self.smc_insns.setdefault(owner, []).append(a)
        # Bytes observed changing at run time.  These catch patches made through
        # computed addresses, which the scan above can't see (e.g. the 3D clip
        # rectangle immediates in 0x44C904, patched to 0..319 x 0..199).
        for k in self.smc_observed:
            owner = self.cov.get(k)
            if owner is not None:
                self.smc_insns.setdefault(owner, [])
        # Patched short conditional jumps (opcode + disp8 rewritten, e.g. the
        # 3D engine switching jge/jle and its target): work out every target
        # the immediate stores can produce, so they become labels.
        self.smc_jcc_targets = {}
        for j, writers in self.smc_insns.items():
            ins = self.insns[j]
            if ins.size == 5 and ins.bytes[0] == 0xe9:
                # A near jump whose rel32 the program sets with immediate
                # stores (TFX's textured and Gouraud scan converters pick one
                # of three edge blocks this way): each stored value is a target.
                targets = set()
                for w in set(writers):
                    wi = self.insns[w]
                    imm = [o for o in wi.operands if o.type == X.X86_OP_IMM]
                    mem = [o for o in wi.operands if o.type == X.X86_OP_MEM]
                    if not imm or not mem or mem[0].size != 4 or mem[0].mem.base or mem[0].mem.index:
                        continue
                    if (mem[0].mem.disp & 0xffffffff) == j + 1:
                        targets.add((j + 5 + imm[0].imm) & 0xffffffff)
                targets = {t for t in targets if self.in_code(t)}
                if targets:
                    self.smc_jcc_targets[j] = sorted(targets)
                continue
            if ins.size != 2 or not (0x70 <= ins.bytes[0] <= 0x7f or ins.bytes[0] == 0xeb):
                # Any other instruction overwritten with a short branch, e.g. the
                # `mov bx, [..]` at 0x44CCA2 that the renderer temporarily turns
                # into `jmp short +4Ah` (EB 4A) and later restores.
                targets = set()
                for w in set(writers):
                    wi = self.insns[w]
                    imm = [o for o in wi.operands if o.type == X.X86_OP_IMM]
                    mem = [o for o in wi.operands if o.type == X.X86_OP_MEM]
                    if not imm or not mem or mem[0].size < 2:
                        continue
                    t = mem[0].mem.disp & 0xffffffff
                    val = (imm[0].imm & ((1 << (8 * mem[0].size)) - 1)).to_bytes(mem[0].size, 'little')
                    if t == j and (0x70 <= val[0] <= 0x7f or val[0] == 0xeb):
                        targets.add(j + 2 + (val[1] - 256 if val[1] >= 128 else val[1]))
                b0, b1 = self.smc_observed.get(j), self.smc_observed.get(j + 1)
                if b0 is not None and b1 is not None and (0x70 <= b0 <= 0x7f or b0 == 0xeb):
                    targets.add(j + 2 + (b1 - 256 if b1 >= 128 else b1))
                targets = {t for t in targets if self.in_code(t)}
                if targets:
                    self.smc_jcc_targets[j] = sorted(targets)
                continue
            targets = {ins.operands[0].imm}
            for w in set(writers):
                wi = self.insns[w]
                imm = [o for o in wi.operands if o.type == X.X86_OP_IMM]
                mem = [o for o in wi.operands if o.type == X.X86_OP_MEM]
                if not imm or not mem:
                    continue
                t = mem[0].mem.disp & 0xffffffff
                val = (imm[0].imm & ((1 << (8 * mem[0].size)) - 1)).to_bytes(mem[0].size, 'little')
                if t <= j + 1 < t + len(val):
                    disp = val[j + 1 - t]
                    disp = disp - 256 if disp >= 128 else disp
                    targets.add(j + 2 + disp)
            if j in self.smc_observed or j + 1 in self.smc_observed:
                b0 = self.smc_observed.get(j, ins.bytes[0])
                b1 = self.smc_observed.get(j + 1, ins.bytes[1])
                if 0x70 <= b0 <= 0x7f or b0 == 0xeb:
                    targets.add(j + 2 + (b1 - 256 if b1 >= 128 else b1))
            self.smc_jcc_targets[j] = sorted(targets)
        for ts in self.smc_jcc_targets.values():
            self._trace_commit(list(ts), force=True, is_func=False, prune=True)
            for t in ts:
                self.leaders.add(t)

    def _sys_ok(self, ins, strong):
        return strong and (ins.mnemonic in SYS_OK or 'cr0' in ins.op_str)

    def _shallow_ok(self, t, strong):
        """Linear decode from t up to the first unconditional transfer."""
        a = t
        for _ in range(64):
            if not self.in_code(a) or self.in_data_range(a):
                return False
            ins = self._dcache.get(a, 0)
            if ins == 0:
                o = self.le.obj_at(a)
                ins = None
                for ins in CS.disasm(bytes(o.data[a - o.base:a - o.base + 16]), a, 1):
                    break
                self._dcache[a] = ins
            if ins is None:
                return False
            if a in self.insns:          # joined existing code
                return True
            if (self.suspicious(ins) and not self._sys_ok(ins, strong)) or self._overlaps(a, ins.size):
                return False
            if ins.id in (X.X86_INS_JMP, X.X86_INS_RET, X.X86_INS_RETF, X.X86_INS_IRETD,
                          X.X86_INS_CALL):
                return True
            a += ins.size
        return True

    def _tier_strong(self):
        progress = False
        while True:
            self._code_ptr_candidates()          # refreshes self.strong
            soft = {t for t in self.soft if t not in self.insns}
            self.soft = set()
            strong = {t for t in self.strong if t not in self.insns}
            cands = sorted((soft | strong) - self.rejected_soft - self.rejected_strong)
            if not cands:
                return progress
            for t in cands:
                if t in self.insns:
                    continue
                is_soft = t in soft
                if self._shallow_ok(t, strong=not is_soft):
                    self._trace_commit([t], force=True, is_func=not is_soft, strong=not is_soft,
                                       prune=True)
                    progress = True
                elif is_soft:
                    self.rejected_soft.add(t)
                else:
                    self.rejected_strong.add(t)

    def _tier_weak(self):
        cands = sorted(self._code_ptr_candidates() - self.insns.keys() - self.rejected)
        progress = False
        for t in cands:
            if t in self.insns:
                continue
            if self._trace_commit([t]):
                progress = True
                if self.soft:
                    return True        # go back to the strong tier
            else:
                self.rejected.add(t)
        return progress

    PAD = (bytes.fromhex('8d4000'), bytes.fromhex('8bc0'), bytes.fromhex('90'),
           bytes.fromhex('8d4900'), bytes.fromhex('8d642400'), bytes.fromhex('8d742600'))

    def _gaps(self):
        cov = self.cov
        for o in self.code:
            a, end = o.base, o.base + o.vsize
            while a < end:
                if a in cov or self.in_data_range(a):
                    a += 1
                    continue
                b = a
                while b < end and b not in cov and not self.in_data_range(b):
                    b += 1
                yield a, b
                a = b

    def _scan_gaps_once(self):
        """Try to decode unreached regions that start like Watcom functions."""
        progress = False
        for a, b in list(self._gaps()):
            p = a
            while p < b:
                raw = self.le.read(p, 4)
                for pad in self.PAD:
                    if raw.startswith(pad):
                        p += len(pad)
                        break
                else:
                    break
            if p >= b or p in self.insns or p in self.rejected_gap or self.le.read(p, 1)[0] not in (
                    0x50, 0x51, 0x52, 0x53, 0x55, 0x56, 0x57, 0xc8, 0x60, 0x06, 0x1e):
                continue
            if self._trace_commit([p]):
                progress = True
            else:
                self.rejected_gap.add(p)
        return progress

    def _code_ptr_candidates(self):
        """Fixup targets in code that are used as code pointers.

        A fixup inside a decoded instruction counts only if it is an
        immediate operand (push offset / mov reg, offset); memory
        displacements are data references.  Fixups outside decoded code
        (data object, or tables embedded in the code object) always count.
        """
        cov = self.cov
        out = set()
        for f in self.le.fixups:
            if not self.in_code(f.target) or self.in_data_range(f.target):
                continue
            if f.src in cov:
                ins = self.insns[cov[f.src]]
                if ins.id in (X.X86_INS_CALL, X.X86_INS_JMP) or ins.id in JCC:
                    continue
                if any(op.type == X.X86_OP_IMM and (op.imm & 0xffffffff) == f.target
                       for op in ins.operands):
                    out.add(f.target)
            else:
                out.add(f.target)
                o = self.le.obj_at(f.src)
                if o is not None and not o.is_code:
                    self.strong.add(f.target)
        return out

    def _port_set_before(self, a):
        """A `mov dx, 3xxh` / `mov edx, 3xxh` within the 4 KB before a
        (string port output such as the rasterizer 0x439190's VGA `outsb`)."""
        o = self.le.obj_at(a)
        if o is None:
            return False
        lo = max(o.base, a - 4096)
        b = bytes(o.data[lo - o.base:a - o.base])
        for i in range(len(b) - 3):
            if b[i] == 0xba and b[i + 2] == 0x03 and (i + 4 >= len(b) or b[i + 3:i + 5] == bytes(2)):
                return True
            if b[i] == 0x66 and b[i + 1] == 0xba and i + 3 < len(b) and b[i + 3] == 0x03:
                return True
        return False

    def suspicious(self, ins):
        m = ins.mnemonic
        if m in ('outsb', 'outsw', 'outsd') and self._port_set_before(ins.address):
            return False
        if m in BAD_MN:
            return True
        if ins.bytes[:2] == bytes(2):      # add [eax], al: zero padding / data
            return True
        if ins.op_str.startswith('mm') or 'xmm' in ins.op_str or ', mm' in ins.op_str or \
                'cr0' in ins.op_str or ins.op_str.startswith('dr'):
            return True
        if m.startswith('lock'):
            return True
        if (m.startswith('rep ') or m.startswith('repe') or m.startswith('repne')) and m not in OK_REP:
            return True
        return False

    def _overlaps(self, a, size):
        cov = self.cov
        for b in range(a, a + size):
            if b in cov:
                return True
        return False

    def _trace_commit(self, starts, force=False, is_func=True, strong=False, prune=False):
        """Trace from starts as a trial; commit if it looks like code.

        prune: stop individual paths at bad/overlapping instructions instead
        of rejecting the whole trace (used for strong evidence)."""
        j = self._j = dict(insns=[], leaders=[], tables=[], soft=[],
                           n_ind=len(self.indirect), n_dr=len(self.data_ranges))
        self.bad = []
        funcs = set(starts) if is_func else set()
        self._prune = (strong,) if prune else None
        self._trace(list(starts), funcs)
        self._prune = None
        if prune:
            self.bad = []
        ok = True
        if not force:
            if self.bad:
                ok = False
            else:
                for a in j['insns']:
                    i = self.insns[a]
                    susp = self.suspicious(i) and not self._sys_ok(i, strong)
                    if susp or self._overlaps(a, i.size):
                        ok = False
                        break
        self._j = None
        if ok:
            for a in j['insns']:
                for b in range(a, a + self.insns[a].size):
                    self.cov[b] = a
            self.func_entries |= funcs
            return True
        for a in j['insns']:
            del self.insns[a]
        for a in j['leaders']:
            self.leaders.discard(a)
        for a in j['tables']:
            self.jump_tables.pop(a, None)
        for a in j['soft']:
            self.soft.discard(a)
        del self.indirect[j['n_ind']:]
        del self.data_ranges[j['n_dr']:]
        return False

    def decode(self, a):
        if a in self.insns:
            return self.insns[a]
        ins = self._dcache.get(a, 0)
        if ins == 0:
            ins = None
            o = self.le.obj_at(a)
            if o is not None:
                i = a - o.base
                for ins in CS.disasm(bytes(o.data[i:i + 16]), a, 1):
                    break
            self._dcache[a] = ins
        if ins is not None:
            self.insns[a] = ins
            if self._j is not None:
                self._j['insns'].append(a)
        return ins

    def _lead(self, a):
        if a not in self.leaders:
            self.leaders.add(a)
            if self._j is not None:
                self._j['leaders'].append(a)

    def _trace(self, work, funcs):
        while work:
            a = work.pop()
            if a in self.leaders and a in self.insns:
                continue
            self._lead(a)
            while True:
                if not self.in_code(a) or self.in_data_range(a):
                    self.bad.append(a)
                    break
                fresh = a not in self.insns
                ins = self.decode(a)
                if ins is None:
                    self.bad.append(a)
                    break
                if self._prune is not None and fresh:
                    if (self.suspicious(ins) and not self._sys_ok(ins, self._prune[0])) \
                            or self._overlaps(a, ins.size):
                        del self.insns[a]
                        self._j['insns'].remove(a)
                        self.pruned.add(a)
                        break
                nxt = a + ins.size
                iid = ins.id
                if iid == X.X86_INS_CALL:
                    op = ins.operands[0]
                    if op.type == X.X86_OP_IMM:
                        funcs.add(op.imm)
                        work.append(op.imm)
                    else:
                        self.indirect.append(a)
                    if nxt not in self.insns and nxt not in self.soft:
                        self.soft.add(nxt)
                        if self._j is not None:
                            self._j['soft'].append(nxt)
                    break
                elif iid == X.X86_INS_JMP:
                    op = ins.operands[0]
                    if op.type == X.X86_OP_IMM:
                        work.append(op.imm)
                    else:
                        tbl = self._jump_table(ins) or self._block_jump(ins)
                        if tbl:
                            self.jump_tables[a] = tbl
                            if self._j is not None:
                                self._j['tables'].append(a)
                            work.extend(tbl[1])
                        else:
                            self.indirect.append(a)
                    break
                elif iid in JCC:
                    work.append(ins.operands[0].imm)
                    self._lead(nxt)
                elif iid in ENDS:
                    break
                if nxt in self.leaders and nxt in self.insns:
                    break
                a = nxt

    def _table_bound(self, ins, index_reg):
        """Find `cmp idx, N` + `ja` guarding a jump table -> N+1 entries."""
        a = ins.address
        prev = {}
        for b in range(a - 40, a):
            i = self.insns.get(b)
            if i is not None and b + i.size <= a:
                prev[b + i.size] = i
        cur = a
        for _ in range(8):
            i = prev.get(cur)
            if i is None:
                return None
            if i.id == X.X86_INS_CMP and len(i.operands) == 2 and \
                    i.operands[1].type == X.X86_OP_IMM and i.operands[0].type == X.X86_OP_REG:
                return (i.operands[1].imm & 0xffffffff) + 1
            cur = i.address
        return None

    def _block_jump(self, ins):
        """A computed jump into an unrolled block:
               imul eax, ecx, -N / add eax, END / [mov cl, imm8] / jmp eax
           followed directly by the block of N-byte units that ends at END
           (TFX's polygon scan converter 0x46AB50: 200 edge steps of 11
           bytes; ECX steps are run by entering 11 * ECX bytes before the
           end).  Every unit start, and END, is a target."""
        op = ins.operands[0]
        if op.type != X.X86_OP_REG or op.reg != X.X86_REG_EAX:
            return None
        a = p = ins.address
        mv = self.decode(a - 2)
        if mv is not None and mv.size == 2 and mv.mnemonic == 'mov' and mv.operands[0].type == X.X86_OP_REG                 and mv.operands[0].reg == X.X86_REG_CL and mv.operands[1].type == X.X86_OP_IMM:
            p = a - 2
        add, mul = self.decode(p - 5), self.decode(p - 8)
        if add is None or mul is None or add.size != 5 or mul.size != 3 or add.mnemonic != 'add' or mul.mnemonic != 'imul':
            return None
        if len(mul.operands) != 3 or mul.operands[0].reg != X.X86_REG_EAX or mul.operands[1].type != X.X86_OP_REG                 or mul.operands[1].reg != X.X86_REG_ECX or mul.operands[2].type != X.X86_OP_IMM:
            return None
        if add.operands[0].type != X.X86_OP_REG or add.operands[0].reg != X.X86_REG_EAX or add.operands[1].type != X.X86_OP_IMM:
            return None
        n = mul.operands[2].imm & 0xffffffff
        n = 0x100000000 - n if n & 0x80000000 else -n
        end, start = add.operands[1].imm & 0xffffffff, a + ins.size
        if n <= 0 or end <= start or (end - start) % n or not self.in_code(end):
            return None
        return (a, [end - n * k for k in range((end - start) // n + 1)])

    def _jump_table(self, ins):
        """jmp dword ptr [reg*4 + table]  where table entries are fixups."""
        op = ins.operands[0]
        if op.type != X.X86_OP_MEM or op.mem.scale != 4 or op.mem.base != 0:
            return None
        tbl = op.mem.disp & 0xffffffff
        limit = self._table_bound(ins, op.mem.index)
        out = []
        p = tbl
        while p in self.fixup_at and self.in_code(self.fixup_at[p].target):
            out.append(self.fixup_at[p].target)
            p += 4
            if limit is not None and len(out) >= limit:
                break
            if out and p in self.insns:
                break
        if not out:
            return None
        self.data_ranges.append((tbl, p))
        return (tbl, out)


def report(prog):
    insns = prog.insns
    mn = Counter(i.mnemonic for i in insns.values())
    total_code = sum(o.vsize for o in prog.code)
    covered = sum(i.size for i in insns.values())
    print(f'instructions: {len(insns)}  bytes covered: {covered:#x} / {total_code:#x} '
          f'({100*covered/total_code:.1f}%)')
    print(f'function entries: {len(prog.func_entries)}  leaders: {len(prog.leaders)}  '
          f'jump tables: {len(prog.jump_tables)}  unresolved indirect: {len(prog.indirect)}  '
          f'decode failures: {len(prog.bad)}  rejected ptr targets: {len(prog.rejected)}  '
          f'int stubs: {len(prog.int_stubs)}  noreturn call sites: {len(prog.noreturn_sites)}')
    print('\nmnemonics:')
    for k, v in sorted(mn.items(), key=lambda x: -x[1]):
        print(f'  {k:12} {v}')

    addrs = sorted(insns)
    idx = {a: n for n, a in enumerate(addrs)}
    print('\nint instructions:')
    ints = Counter()
    for a in addrs:
        ins = insns[a]
        if ins.mnemonic == 'int':
            ah = '?'
            for back in range(1, 8):
                n = idx[a] - back
                if n < 0:
                    break
                p = insns[addrs[n]]
                if p.mnemonic == 'mov' and p.op_str.split(',')[0].strip() in ('ah', 'ax', 'eax'):
                    ah = p.op_str.split(',')[1].strip()
                    break
            ints[(ins.op_str, ah)] += 1
    for (n, ah), c in sorted(ints.items()):
        print(f'  int {n:5} ax/ah={ah:12} x{c}')

    sysm = {'in', 'out', 'insb', 'insw', 'insd', 'outsb', 'outsw', 'outsd', 'cli', 'sti',
            'lgdt', 'lidt', 'sgdt', 'sidt', 'lldt', 'ltr', 'hlt', 'iretd', 'iret', 'lar', 'lsl',
            'verr', 'verw', 'arpl', 'retf', 'ljmp', 'lcall', 'pushfd', 'popfd', 'pushf', 'popf'}
    print('\nsystem instructions (counts):')
    c = Counter(insns[a].mnemonic + ' ' + insns[a].op_str for a in addrs if insns[a].mnemonic in sysm)
    for k, v in sorted(c.items()):
        print(f'  {k:30} {v}')
    print('\nsegment register writes:')
    for a in addrs:
        ins = insns[a]
        if (ins.mnemonic in ('mov', 'pop') and ins.op_str.split(',')[0].strip() in
                ('ds', 'es', 'fs', 'gs', 'ss')) or ins.mnemonic in ('lds', 'les', 'lfs', 'lgs', 'lss'):
            print(f'  {a:#010x}  {ins.mnemonic} {ins.op_str}')
    print('\nsegment overrides:')
    seg = Counter()
    for a in addrs:
        ins = insns[a]
        for s in ('es:', 'fs:', 'gs:', 'cs:', 'ss:', 'ds:'):
            if s in ins.op_str:
                seg[(s, ins.mnemonic)] += 1
    for k, v in sorted(seg.items()):
        print(f'  {k} {v}')
    print('\nunresolved indirect jumps:')
    for a in prog.indirect:
        ins = insns[a]
        if ins.mnemonic.endswith('jmp'):
            print(f'  {a:#010x}  {ins.mnemonic} {ins.op_str}')
    print('\nsuspicious instructions in reached code:')
    for a in addrs:
        if prog.suspicious(insns[a]):
            print(f'  {a:#010x}  {insns[a].mnemonic} {insns[a].op_str}')
    if prog.bad:
        print('\ndecode failures:', [hex(a) for a in prog.bad[:40]])


if __name__ == '__main__':
    p = Program(sys.argv[1])
    p.run()
    report(p)
