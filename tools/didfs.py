"""Reader for TFX's packed data archive DID.DAT.

Layout (reverse engineered from TFX.DAT, see TFX_CODE.md):
  u32  offset of the directory block
  ...  file data
  directory block:
    u16 nfiles, u16 ndirs, u16 nexts, u16 (unknown, 493)
    ndirs  x (u8 len, chars)   directory prefixes, e.g. "tunes\\airsuper\\"
    nexts  x (u8 len, chars)   extensions, e.g. ".dir"
    nfiles x 12-byte entries   (u32 hash, u32 a, u32 b), sorted by signed hash
A path is looked up by replacing a matching directory prefix with
chr('0' + index), the extension with chr('A' + index), then hashing.

usage: didfs.py <DID.DAT> [name ...]
"""
import struct
import sys


def did_hash(s):
    """TFX name hash (TFX.DAT 0x759E0)."""
    b = s.encode('latin-1')
    n = len(b)
    b4 = bc = b10 = b8 = 0
    cl = b[0] if n else 0
    for i in range(n):
        if i != 0 and cl != 0x5f:
            b4 = (b4 << 1) & 0xff
            b4 |= 1 if cl < b[i] else 0
        cl = b[i]
        if cl == 0x5f:
            bc = (bc - i) & 0xff
        else:
            b8 = (b8 + cl) & 0xff
            bc = ((bc << 1) & 0xff) ^ cl
            b10 = (b10 + b8) & 0xff
    return ((n & 15) << 28) | ((b8 & 15) << 24) | (b10 << 16) | (bc << 8) | b4


class DidFS:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        d = self.data
        off = struct.unpack_from('<I', d, 0)[0]
        self.nfiles, self.ndirs, self.nexts, self.unk = struct.unpack_from('<4H', d, off)
        p = off + 8
        self.dirs = []
        for _ in range(self.ndirs):
            n = d[p]; self.dirs.append(d[p + 1:p + 1 + n].decode('latin-1')); p += 1 + n
        self.exts = []
        for _ in range(self.nexts):
            n = d[p]; self.exts.append(d[p + 1:p + 1 + n].decode('latin-1')); p += 1 + n
        self.entries_off = p
        self.entries = [struct.unpack_from('<iII', d, p + 12 * i) for i in range(self.nfiles)]
        self.by_hash = {}
        for h, a, b in self.entries:
            self.by_hash.setdefault(h, []).append((a, b))

    def encode(self, name):
        s = name.lower()
        for i, dname in enumerate(self.dirs):
            if s.startswith(dname.lower()):
                s = chr(0x30 + i) + s[len(dname):]
                break
        dot = s.find('.')
        if dot >= 0:
            ext = s[dot:]
            for i, e in enumerate(self.exts):
                if ext == e.lower():
                    s = s[:dot] + chr(0x41 + i)
                    break
        return s

    def lookup(self, name):
        enc = self.encode(name)
        h = did_hash(enc)
        if h & 0x80000000:
            h -= 1 << 32
        return enc, h, self.by_hash.get(h)


if __name__ == '__main__':
    fs = DidFS(sys.argv[1])
    print(f'files {fs.nfiles} dirs {fs.ndirs} exts {fs.nexts} unk {fs.unk}')
    print('dirs:', fs.dirs)
    print('exts:', fs.exts)
    print('first entries:', [(hex(h & 0xffffffff), hex(a), hex(b)) for h, a, b in fs.entries[:6]])
    hs = [h for h, _, _ in fs.entries]
    print('sorted (signed):', hs == sorted(hs))
    for n in sys.argv[2:]:
        print(n, fs.lookup(n))
