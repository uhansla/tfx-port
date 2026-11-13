"""Dump the structure of a DOS/4GW-bound Watcom LE executable (e.g. TFX.DAT)."""
import struct, sys, re

path = sys.argv[1]
d = open(path, 'rb').read()

# find the MZ stub whose e_lfanew points to an LE header
le_off = None
for m in re.finditer(b'MZ', d):
    o = m.start()
    if o + 0x40 > len(d):
        continue
    lf = struct.unpack_from('<I', d, o + 0x3c)[0]
    if o + lf + 2 <= len(d) and d[o + lf:o + lf + 2] == b'LE':
        le_off, stub_off = o + lf, o
        break
if le_off is None:
    sys.exit('no LE header found')

def u32(o): return struct.unpack_from('<I', d, le_off + o)[0]

hdr = dict(
    cpu=struct.unpack_from('<H', d, le_off + 8)[0], os=struct.unpack_from('<H', d, le_off + 10)[0],
    flags=u32(0x10), pages=u32(0x14), eip_obj=u32(0x18), eip=u32(0x1c), esp_obj=u32(0x20), esp=u32(0x24),
    page_size=u32(0x28), last_page=u32(0x2c), fixup_size=u32(0x30), loader_size=u32(0x38),
    obj_tbl=u32(0x40), n_obj=u32(0x44), page_map=u32(0x48), fixup_page_tbl=u32(0x68),
    fixup_rec_tbl=u32(0x6c), import_mod=u32(0x70), n_import_mod=u32(0x74),
    data_pages=u32(0x80), autodata=u32(0x94),
)
print(f'stub MZ at {stub_off:#x}, LE header at {le_off:#x}')
for k, v in hdr.items():
    print(f'  {k:16} {v:#x}')

objs = []
for i in range(hdr['n_obj']):
    vsize, base, fl, pmi, npages, _ = struct.unpack_from('<IIIIII', d, le_off + hdr['obj_tbl'] + i * 24)
    kind = ('code' if fl & 4 else 'data') + (' 32bit' if fl & 0x2000 else ' 16bit')
    print(f'  obj{i+1}: base={base:#010x} vsize={vsize:#x} flags={fl:#x} ({kind}) pages {pmi}..{pmi+npages-1}')
    objs.append((vsize, base, fl, pmi, npages))

# count fixups
fpt = le_off + hdr['fixup_page_tbl']
offs = struct.unpack_from(f'<{hdr["pages"]+1}I', d, fpt)
print(f'  fixup records total bytes: {offs[-1]:#x}')
