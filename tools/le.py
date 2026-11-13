"""Loader for DOS/4GW-bound Watcom LE executables.

Parses the LE header, object table and fixup records, and produces a relocated
flat memory image with each object placed at a caller-chosen base address.
"""
import re
import struct


class Obj:
    def __init__(self, idx, vsize, link_base, flags, pmi, npages):
        self.idx = idx            # 1-based object number
        self.vsize = vsize
        self.link_base = link_base
        self.flags = flags
        self.pmi = pmi
        self.npages = npages
        self.base = link_base     # load address (set by load())
        self.data = None          # bytearray, len = aligned vsize

    @property
    def is_code(self): return bool(self.flags & 4)

    @property
    def is_32(self): return bool(self.flags & 0x2000)

    def __repr__(self):
        return (f'obj{self.idx}(base={self.base:#x} vsize={self.vsize:#x} '
                f'{"code" if self.is_code else "data"}{"32" if self.is_32 else "16"})')


class Fixup:
    """One relocated location.

    kind: 'off32'  32-bit absolute offset
          'rel32'  32-bit self-relative (call/jmp target)
          'off16'  16-bit offset
          'sel16'  16-bit selector of target object
          'ptr48'  16:32 far pointer
          'ptr32'  16:16 far pointer
    src: linear address (after load) of the patched bytes
    tobj: target object number, toff: offset in target object
    """
    __slots__ = ('kind', 'src', 'tobj', 'toff', 'target')

    def __init__(self, kind, src, tobj, toff, target):
        self.kind, self.src, self.tobj, self.toff, self.target = kind, src, tobj, toff, target

    def __repr__(self):
        return f'Fixup({self.kind} @{self.src:#x} -> obj{self.tobj}+{self.toff:#x} = {self.target:#x})'


class LE:
    def __init__(self, path):
        self.path = path
        self.raw = open(path, 'rb').read()
        d = self.raw
        self.le_off = None
        for m in re.finditer(b'MZ', d):
            o = m.start()
            if o + 0x40 > len(d):
                continue
            lf = struct.unpack_from('<I', d, o + 0x3c)[0]
            if o + lf + 2 <= len(d) and d[o + lf:o + lf + 2] == b'LE':
                self.le_off = o + lf
                self.stub_off = o
                break
        if self.le_off is None:
            raise ValueError('no LE header')
        h = self.le_off
        u32 = lambda o: struct.unpack_from('<I', d, h + o)[0]
        self.npages = u32(0x14)
        self.eip_obj, self.eip = u32(0x18), u32(0x1c)
        self.esp_obj, self.esp = u32(0x20), u32(0x24)
        self.page_size = u32(0x28)
        self.last_page = u32(0x2c)
        self.fixup_page_tbl = h + u32(0x68)
        self.fixup_rec_tbl = h + u32(0x6c)
        # data pages offset is relative to the start of the *file* stub (DOS/4GW
        # bound exes: relative to the MZ header of the LE stub)
        self.data_pages = self.stub_off + u32(0x80)
        n_obj = u32(0x44)
        self.objs = []
        for i in range(n_obj):
            vs, base, fl, pmi, np_, _ = struct.unpack_from('<IIIIII', d, h + u32(0x40) + i * 24)
            self.objs.append(Obj(i + 1, vs, base, fl, pmi, np_))
        self.fixups = []

    def obj(self, n):
        return self.objs[n - 1]

    def load(self, bases=None):
        """Load all objects. bases: optional {objnum: base}."""
        d = self.raw
        ps = self.page_size
        for o in self.objs:
            if bases and o.idx in bases:
                o.base = bases[o.idx]
            size = (max(o.vsize, o.npages * ps) + ps - 1) // ps * ps
            o.data = bytearray(size)
            for p in range(o.npages):
                pnum = o.pmi + p                     # 1-based page number
                off = self.data_pages + (pnum - 1) * ps
                n = self.last_page if pnum == self.npages else ps
                o.data[p * ps:p * ps + n] = d[off:off + n]
        self._apply_fixups()

    def _page_owner(self, pnum):
        for o in self.objs:
            if o.pmi <= pnum < o.pmi + o.npages:
                return o, (pnum - o.pmi) * self.page_size
        raise ValueError(pnum)

    def _apply_fixups(self):
        d = self.raw
        fpt = struct.unpack_from(f'<{self.npages + 1}I', d, self.fixup_page_tbl)
        self.fixups = []
        for pidx in range(self.npages):
            pnum = pidx + 1
            o, page_off = self._page_owner(pnum)
            p = self.fixup_rec_tbl + fpt[pidx]
            end = self.fixup_rec_tbl + fpt[pidx + 1]
            while p < end:
                src_t = d[p]; flags = d[p + 1]; p += 2
                if flags & 3 != 0:
                    raise NotImplementedError(f'fixup target type {flags & 3} at {p:#x}')
                srclist = bool(src_t & 0x20)
                if srclist:
                    cnt = d[p]; p += 1
                else:
                    srcoff = struct.unpack_from('<h', d, p)[0]; p += 2
                if flags & 0x40:
                    tobj = struct.unpack_from('<H', d, p)[0]; p += 2
                else:
                    tobj = d[p]; p += 1
                stype = src_t & 0x0f
                if stype == 2:      # selector only, no offset
                    toff = 0
                elif flags & 0x10:
                    toff = struct.unpack_from('<I', d, p)[0]; p += 4
                else:
                    toff = struct.unpack_from('<H', d, p)[0]; p += 2
                if flags & 0x04:
                    raise NotImplementedError('additive fixups')
                offs = []
                if srclist:
                    offs = list(struct.unpack_from(f'<{cnt}h', d, p)); p += 2 * cnt
                else:
                    offs = [srcoff]
                for so in offs:
                    self._apply_one(o, page_off + so, stype, tobj, toff)

    def _apply_one(self, o, off, stype, tobj, toff):
        t = self.obj(tobj)
        target = t.base + toff
        src = o.base + off
        kinds = {7: 'off32', 8: 'rel32', 5: 'off16', 2: 'sel16', 6: 'ptr48', 3: 'ptr32'}
        kind = kinds.get(stype)
        if kind is None:
            raise NotImplementedError(f'fixup source type {stype}')
        self.fixups.append(Fixup(kind, src, tobj, toff, target))

        def put(fmt, at, val):
            # fixups may straddle the page / object end
            size = struct.calcsize(fmt)
            if 0 <= at and at + size <= len(o.data):
                struct.pack_into(fmt, o.data, at, val)

        if kind == 'off32':
            put('<I', off, target & 0xffffffff)
        elif kind == 'rel32':
            put('<I', off, (target - (src + 4)) & 0xffffffff)
        elif kind == 'off16':
            put('<H', off, target & 0xffff)
        elif kind == 'sel16':
            put('<H', off, self.selector_for(tobj))
        elif kind == 'ptr48':
            put('<I', off, target & 0xffffffff)
            put('<H', off + 4, self.selector_for(tobj))
        elif kind == 'ptr32':
            put('<H', off, toff & 0xffff)
            put('<H', off + 2, self.selector_for(tobj))

    # Selector numbering used by our runtime for LE objects.  Flat code/data
    # objects share the flat selectors; 16-bit objects get their own.
    FLAT_CS, FLAT_DS = 0x08, 0x10

    def selector_for(self, objnum):
        o = self.obj(objnum)
        if o.is_32:
            return self.FLAT_CS if o.is_code else self.FLAT_DS
        return 0x18 + 8 * (objnum - 1)

    # helpers
    def read(self, addr, n):
        for o in self.objs:
            if o.base <= addr < o.base + len(o.data):
                i = addr - o.base
                return bytes(o.data[i:i + n])
        raise KeyError(hex(addr))

    def obj_at(self, addr):
        for o in self.objs:
            if o.base <= addr < o.base + len(o.data):
                return o
        return None

    @property
    def entry(self):
        return self.obj(self.eip_obj).base + self.eip

    @property
    def stack_top(self):
        return self.obj(self.esp_obj).base + self.esp


if __name__ == '__main__':
    import sys
    le = LE(sys.argv[1])
    le.load()
    print(le.objs)
    print(f'entry {le.entry:#x} stack {le.stack_top:#x} fixups {len(le.fixups)}')
    from collections import Counter
    print(Counter(f.kind for f in le.fixups))
    print(Counter((le.obj_at(f.src).idx if le.obj_at(f.src) else None, f.tobj) for f in le.fixups))
