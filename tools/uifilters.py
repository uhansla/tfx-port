"""Offline comparison of pixel-art upscalers on a game frame.

usage: py -3 tools/uifilters.py FRAME.bmp OUTDIR X Y W H
  FRAME.bmp: the game's 320x200 frame as dev/drive.sh `shot` saves it (32-bit BMP)
  X Y W H:   the region in game pixels
Writes OUTDIR/<filter>.bmp, each the region upscaled 4x: nearest, epx (Scale2x
twice), mmpx (MMPX twice), xbr (xBR level 2, blended, as the port's ui_filter
shader) and xbr_sharp (xBR level 2 without blending).  Pure Python.
"""
import os
import struct
import sys


def read_bmp(path):
    d = open(path, 'rb').read()
    off, = struct.unpack_from('<I', d, 10)
    w, h, _, bpp = struct.unpack_from('<iiHH', d, 18)
    assert bpp == 32, bpp
    rows = []
    for y in range(abs(h)):
        sy = abs(h) - 1 - y if h > 0 else y
        p = off + sy * w * 4
        rows.append([struct.unpack_from('<I', d, p + 4 * x)[0] & 0xffffff for x in range(w)])
    return rows


def write_bmp(path, img):
    h, w = len(img), len(img[0])
    stride = (w * 3 + 3) & ~3
    out = bytearray()
    for y in range(h - 1, -1, -1):
        row = bytearray()
        for c in img[y]:
            row += bytes((c & 255, (c >> 8) & 255, (c >> 16) & 255))
        out += row + b'\0' * (stride - len(row))
    hdr = struct.pack('<2sIHHI', b'BM', 54 + len(out), 0, 0, 54)
    hdr += struct.pack('<IiiHHIIiiII', 40, w, h, 1, 24, 0, len(out), 2835, 2835, 0, 0)
    open(path, 'wb').write(hdr + out)


def luma(c):
    return 0.299 * ((c >> 16) & 255) + 0.587 * ((c >> 8) & 255) + 0.114 * (c & 255)


def getter(img):
    h, w = len(img), len(img[0])
    return lambda x, y: img[min(max(y, 0), h - 1)][min(max(x, 0), w - 1)]


def nearest(img, k):
    return [[c for c in row for _ in range(k)] for row in img for _ in range(k)]


def epx2(img):
    s = getter(img)
    h, w = len(img), len(img[0])
    out = [[0] * (2 * w) for _ in range(2 * h)]
    for y in range(h):
        for x in range(w):
            B, D, E, F, H = s(x, y - 1), s(x - 1, y), s(x, y), s(x + 1, y), s(x, y + 1)
            e0 = D if (D == B and B != F and D != H) else E
            e1 = F if (B == F and B != D and F != H) else E
            e2 = D if (D == H and D != B and H != F) else E
            e3 = F if (H == F and D != H and B != F) else E
            out[2 * y][2 * x], out[2 * y][2 * x + 1], out[2 * y + 1][2 * x], out[2 * y + 1][2 * x + 1] = e0, e1, e2, e3
    return out


def mmpx2(img):
    """MMPX (McGuire and Gagiu 2021): rule-based 2x magnification for pixel art."""
    s = getter(img)
    h, w = len(img), len(img[0])
    out = [[0] * (2 * w) for _ in range(2 * h)]

    def all3(b, a0, a1, a2): return b == a0 and b == a1 and b == a2
    def all4(b, a0, a1, a2, a3): return b == a0 and b == a1 and b == a2 and b == a3
    def any3(b, a0, a1, a2): return b == a0 or b == a1 or b == a2
    def none2(b, a0, a1): return b != a0 and b != a1
    def none4(b, a0, a1, a2, a3): return b != a0 and b != a1 and b != a2 and b != a3

    for y in range(h):
        for x in range(w):
            A, B, C = s(x - 1, y - 1), s(x, y - 1), s(x + 1, y - 1)
            D, E, F = s(x - 1, y), s(x, y), s(x + 1, y)
            G, H, I = s(x - 1, y + 1), s(x, y + 1), s(x + 1, y + 1)
            J = K = L = M = E
            if not (A == E and B == E and C == E and D == E and F == E and G == E and H == E and I == E):
                P, S, Q, R = s(x, y - 2), s(x, y + 2), s(x - 2, y), s(x + 2, y)
                Bl, Dl, El, Fl, Hl = luma(B), luma(D), luma(E), luma(F), luma(H)
                # 1:1 slopes
                if D == B and D != H and D != F and (El >= Dl or E == A) and any3(E, A, C, G) and (El < Dl or A != D or E != P or E != Q):
                    J = D
                if B == F and B != D and B != H and (El >= Bl or E == C) and any3(E, A, C, I) and (El < Bl or C != B or E != P or E != R):
                    K = B
                if H == D and H != F and H != B and (El >= Hl or E == G) and any3(E, A, G, I) and (El < Hl or G != H or E != S or E != Q):
                    L = H
                if F == H and F != B and F != D and (El >= Fl or E == I) and any3(E, C, G, I) and (El < Fl or I != H or E != R or E != S):
                    M = F
                # intersections
                if E != F and all4(E, C, I, D, Q) and F == B and F == H and F != s(x + 3, y):
                    K = M = F
                if E != D and all4(E, A, G, F, R) and D == B and D == H and D != s(x - 3, y):
                    J = L = D
                if E != H and all4(E, G, I, B, P) and H == D and H == F and H != s(x, y + 3):
                    L = M = H
                if E != B and all4(E, A, C, H, S) and B == D and B == F and B != s(x, y - 3):
                    J = K = B
                # triangle tips
                if Bl < El and all4(E, G, H, I, S) and none4(E, A, D, C, F):
                    J = K = B
                if Hl < El and all4(E, A, B, C, P) and none4(E, G, D, I, F):
                    L = M = H
                if Fl < El and all4(E, A, D, G, Q) and none4(E, B, C, H, I):
                    K = M = F
                if Dl < El and all4(E, C, F, I, R) and none4(E, B, A, H, G):
                    J = L = D
                # 2:1 slopes
                if H != B:
                    if H != A and H != E and H != C:
                        if all3(H, G, F, R) and none2(H, D, s(x + 2, y - 1)):
                            L = M
                        if all3(H, I, D, Q) and none2(H, F, s(x - 2, y - 1)):
                            M = L
                    if B != I and B != G and B != E:
                        if all3(B, A, F, R) and none2(B, D, s(x + 2, y + 1)):
                            J = K
                        if all3(B, C, D, Q) and none2(B, F, s(x - 2, y + 1)):
                            K = J
                if F != D:
                    if D != I and D != E and D != C:
                        if all3(D, A, H, S) and none2(D, B, s(x + 1, y + 2)):
                            J = L
                        if all3(D, G, B, P) and none2(D, H, s(x + 1, y - 2)):
                            L = J
                    if F != E and F != A and F != G:
                        if all3(F, C, H, S) and none2(F, B, s(x - 1, y + 2)):
                            K = M
                        if all3(F, I, B, P) and none2(F, H, s(x - 1, y - 2)):
                            M = K
            out[2 * y][2 * x], out[2 * y][2 * x + 1], out[2 * y + 1][2 * x], out[2 * y + 1][2 * x + 1] = J, K, L, M
    return out


def xbr(img, k, blend=True):
    """xBR level 2 (as the port's ui_filter shader), at k x; blend=False: hard edges."""
    s = getter(img)
    h, w = len(img), len(img[0])
    Y = lambda c: luma(c) * 48.0 / 255.0
    Ao, Bo, Co = (1, -1, -1, 1), (1, 1, -1, -1), (1.5, 0.5, -0.5, 0.5)
    Ax, Bx, Cx = (1, -1, -1, 1), (0.5, 2, -0.5, -2), (1, 1, -0.5, 0)
    Ay, By, Cy = (1, -1, -1, 1), (2, 0.5, -2, -0.5), (2, 0, -1, 0.5)
    delta = 1.0 / k
    dL = (0.5 / k, 1.0 / k, 0.5 / k, 1.0 / k)
    dU = (dL[1], dL[0], dL[3], dL[2])
    out = [[0] * (k * w) for _ in range(k * h)]

    def mix(c0, c1, t):
        if t <= 0: return c0
        if t >= 1: return c1
        r = [int(((c0 >> sh) & 255) * (1 - t) + ((c1 >> sh) & 255) * t + 0.5) for sh in (16, 8, 0)]
        return (r[0] << 16) | (r[1] << 8) | r[2]

    def cdist(a, b):
        return sum(abs(((a >> sh) & 255) - ((b >> sh) & 255)) for sh in (16, 8, 0))

    for y in range(h):
        for x in range(w):
            P = {}
            for dy in range(-2, 3):
                for dx in range(-2, 3):
                    if abs(dx) == 2 and abs(dy) == 2:
                        continue
                    P[(dx, dy)] = s(x + dx, y + dy)
            Ec = P[(0, 0)]
            if all(c == Ec for c in P.values()):
                for sy in range(k):
                    for sx in range(k):
                        out[k * y + sy][k * x + sx] = Ec
                continue
            L = {kk: Y(v) for kk, v in P.items()}
            B, D, Hh, F = L[(0, -1)], L[(-1, 0)], L[(0, 1)], L[(1, 0)]
            C, A, G, I = L[(1, -1)], L[(-1, -1)], L[(-1, 1)], L[(1, 1)]
            b = (B, D, Hh, F); c = (C, A, G, I); e = (L[(0, 0)],) * 4
            sw = lambda v, o: tuple(v[(i + o) % 4] for i in range(4))
            d, f, g, hh, i_ = sw(b, 1), sw(b, 3), sw(c, 2), sw(b, 2), sw(c, 3)
            i4 = (L[(2, 1)], L[(1, -2)], L[(-2, -1)], L[(-1, 2)])
            i5 = (L[(1, 2)], L[(2, -1)], L[(-1, -2)], L[(-2, 1)])
            h5 = (L[(0, 2)], L[(2, 0)], L[(0, -2)], L[(-2, 0)])
            f4 = sw(h5, 1)
            ne = lambda a, bb: 1.0 if a != bb else 0.0
            neq = lambda a, bb: 1.0 if abs(a - bb) >= 15.0 else 0.0
            eq = lambda a, bb: 1.0 if abs(a - bb) < 15.0 else 0.0
            wd = lambda a, bb, cc, dd, ee, ff, gg, h_: abs(a - bb) + abs(a - cc) + abs(dd - ee) + abs(dd - ff) + 4 * abs(gg - h_)
            dec = []
            for j in range(4):
                lv0 = ne(e[j], f[j]) * ne(e[j], hh[j])
                lv1 = lv0 * min(1.0, neq(f[j], b[j]) * neq(hh[j], d[j]) + eq(e[j], i_[j]) * neq(f[j], i4[j]) * neq(hh[j], i5[j]) + eq(e[j], g[j]) + eq(e[j], c[j]))
                lv2l = ne(e[j], g[j]) * ne(d[j], g[j]); lv2u = ne(e[j], c[j]) * ne(b[j], c[j])
                wd1 = wd(e[j], c[j], g[j], i_[j], h5[j], f4[j], hh[j], f[j]); wd2 = wd(hh[j], d[j], i5[j], f[j], i4[j], b[j], e[j], i_[j])
                edri = (1.0 if wd1 <= wd2 else 0.0) * lv0
                edr = (1.0 if wd1 < wd2 else 0.0) * lv1
                dfg, dhc = abs(f[j] - g[j]), abs(hh[j] - c[j])
                edrl = (1.0 if 2 * dfg <= dhc else 0.0) * lv2l * edr
                edru = (1.0 if dfg >= 2 * dhc else 0.0) * lv2u * edr
                px = abs(e[j] - f[j]) <= abs(e[j] - hh[j])
                dec.append((edri, edr, edrl, edru, px))
            Bc, Dc, Hc, Fc = P[(0, -1)], P[(-1, 0)], P[(0, 1)], P[(1, 0)]
            for sy in range(k):
                for sx in range(k):
                    fpx, fpy = (sx + 0.5) / k, (sy + 0.5) / k
                    mx = []
                    for j in range(4):
                        edri, edr, edrl, edru, px = dec[j]
                        fx = Ao[j] * fpy + Bo[j] * fpx; fl = Ax[j] * fpy + Bx[j] * fpx; fu = Ay[j] * fpy + By[j] * fpx
                        if blend:
                            sat = lambda v: min(1.0, max(0.0, v))
                            v45i = sat((fx + delta - Co[j] - 0.25) / (2 * delta)) * edri
                            v45 = sat((fx + delta - Co[j]) / (2 * delta)) * edr
                            v30 = sat((fl + dL[j] - Cx[j]) / (2 * dL[j])) * edrl
                            v60 = sat((fu + dU[j] - Cy[j]) / (2 * dU[j])) * edru
                        else:
                            v45i = (1.0 if fx > Co[j] + 0.25 else 0.0) * edri
                            v45 = (1.0 if fx > Co[j] else 0.0) * edr
                            v30 = (1.0 if fl > Cx[j] else 0.0) * edrl
                            v60 = (1.0 if fu > Cy[j] else 0.0) * edru
                        mx.append(max(v45i, v45, v30, v60))
                    r1 = mix(Ec, Fc if dec[0][4] else Hc, mx[0])
                    r1 = mix(r1, Dc if dec[2][4] else Bc, mx[2])
                    r2 = mix(Ec, Bc if dec[1][4] else Fc, mx[1])
                    r2 = mix(r2, Hc if dec[3][4] else Dc, mx[3])
                    out[k * y + sy][k * x + sx] = r2 if cdist(Ec, r2) >= cdist(Ec, r1) else r1
    return out


def main():
    src, outdir = sys.argv[1], sys.argv[2]
    X, Yy, W, H = (int(v) for v in sys.argv[3:7])
    img = read_bmp(src)
    m = 3                                           # margin so the filters see the neighbours
    x0, y0 = max(0, X - m), max(0, Yy - m)
    x1, y1 = min(len(img[0]), X + W + m), min(len(img), Yy + H + m)
    crop = [row[x0:x1] for row in img[y0:y1]]
    ox, oy = X - x0, Yy - y0

    def cut(big, k):
        return [row[ox * k:(ox + W) * k] for row in big[oy * k:(oy + H) * k]]

    os.makedirs(outdir, exist_ok=True)
    results = {
        'nearest': cut(nearest(crop, 4), 4),
        'epx': cut(epx2(epx2(crop)), 4),
        'mmpx': cut(mmpx2(mmpx2(crop)), 4),
        'xbr': cut(xbr(crop, 4, True), 4),
        'xbr_sharp': cut(xbr(crop, 4, False), 4),
    }
    for name, im in results.items():
        write_bmp(os.path.join(outdir, name + '.bmp'), im)
        print('wrote', name)


if __name__ == '__main__':
    main()
