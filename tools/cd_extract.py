"""Extract the data track of a BIN/CUE CD image (MODE1/2352) to a folder.

usage: cd_extract.py <image.bin> <outdir>
"""
import os
import struct
import sys


def main():
    src, out = sys.argv[1], sys.argv[2]
    f = open(src, 'rb')

    def sector(lba):
        f.seek(lba * 2352 + 16)
        return f.read(2048)

    def read(lba, size):
        data = bytearray()
        n = (size + 2047) // 2048
        for i in range(n):
            data += sector(lba + i)
        return bytes(data[:size])

    pvd = sector(16)
    assert pvd[1:6] == b'CD001', 'not an ISO9660 data track'
    root = pvd[156:156 + 34]

    def walk(lba, size, path):
        blk = read(lba, size)
        i = 0
        while i < len(blk):
            ln = blk[i]
            if ln == 0:
                i = (i // 2048 + 1) * 2048
                continue
            e = blk[i:i + ln]
            elba = struct.unpack_from('<I', e, 2)[0]
            esz = struct.unpack_from('<I', e, 10)[0]
            flags = e[25]
            name = e[33:33 + e[32]]
            i += ln
            if name in (b'\0', b'\1'):
                continue
            nm = name.decode('ascii').split(';')[0].rstrip('.')
            p = os.path.join(path, nm)
            if flags & 2:
                os.makedirs(p, exist_ok=True)
                walk(elba, esz, p)
            else:
                with open(p, 'wb') as o:
                    o.write(read(elba, esz))
                print(f'{esz:10d} {p}')

    os.makedirs(out, exist_ok=True)
    walk(struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0], out)


if __name__ == '__main__':
    main()
