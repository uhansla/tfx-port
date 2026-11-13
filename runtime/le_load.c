/* Load the original DOS/4GW LE executable (TFX.DAT / CONFIG.EXE) from the
 * user's game folder and relocate it in guest memory, exactly as the
 * recompiler's tools/le.py did when the code was translated.
 *
 * Objects are placed contiguously from 0x400000, 64 KB aligned (recomp.py
 * default_bases); the file's FNV-1a hash must match the one recorded at build
 * time, because the recompiled code only fits that exact executable. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

#define IMAGE_BASE 0x00400000u

struct le_obj { uint32_t vsize, link, flags, pmi, npages, base, size; };

static uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

uint64_t le_hash(const uint8_t *d, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= d[i]; h *= 0x100000001b3ull; }
    return h;
}

/* selector numbering shared with tools/le.py selector_for() */
static uint16_t selector_for(const struct le_obj *o, int objnum) {
    if (o->flags & 0x2000) return (o->flags & 4) ? SEL_FLAT_CS : SEL_FLAT_DS;
    return (uint16_t)(0x18 + 8 * (objnum - 1));
}

/* returns 0 on success, else an error message is written to err */
int le_load(const char *path, uint64_t expect_hash, uint32_t *entry, uint32_t *stack,
            void (*placed)(int idx, uint32_t base, uint32_t size, uint32_t flags),
            char *err, int errlen) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errlen, "cannot open %s", path); return -1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); snprintf(err, errlen, "cannot read %s", path); return -1; }
    fclose(f);
    if (expect_hash && le_hash(d, (size_t)n) != expect_hash) {
        free(d);
        snprintf(err, errlen, "%s is not the file this tfx.exe was built from.\n\n"
                 "This program is built from one exact copy of the game (the CD release patched to v2.01) "
                 "and only runs with that file. Put this tfx.exe in the folder of the game it was built from, "
                 "or build it again from this copy with build.bat.\n\n(expected hash %016llx)",
                 path, (unsigned long long)expect_hash);
        return -1;
    }
    /* find the LE header behind an MZ stub (DOS/4GW bound executables) */
    long stub = -1, h = -1;
    for (long o = 0; o + 0x40 <= n; o++) {
        if (d[o] != 'M' || d[o + 1] != 'Z') continue;
        /* 64-bit arithmetic: lf comes from arbitrary bytes ("MZ" can occur in
         * data), and (long)lf is negative for large values (crashed the x64 build) */
        long long lf = u32(d + o + 0x3c);
        if ((long long)o + lf + 2 <= (long long)n && d[o + lf] == 'L' && d[o + lf + 1] == 'E') { stub = o; h = (long)(o + lf); break; }
    }
    if (h < 0) { free(d); snprintf(err, errlen, "%s: no LE header", path); return -1; }
    uint32_t npages = u32(d + h + 0x14);
    uint32_t eip_obj = u32(d + h + 0x18), eip = u32(d + h + 0x1c);
    uint32_t esp_obj = u32(d + h + 0x20), esp_ = u32(d + h + 0x24);
    uint32_t ps = u32(d + h + 0x28), last_page = u32(d + h + 0x2c);
    long fpt = h + (long)u32(d + h + 0x68), frt = h + (long)u32(d + h + 0x6c);
    long pages = stub + (long)u32(d + h + 0x80);
    uint32_t nobj = u32(d + h + 0x44);
    if (nobj > 16 || ps == 0) { free(d); snprintf(err, errlen, "%s: bad LE header", path); return -1; }
    struct le_obj obj[16];
    uint32_t next = IMAGE_BASE;
    for (uint32_t i = 0; i < nobj; i++) {
        const uint8_t *e = d + h + u32(d + h + 0x40) + i * 24;
        struct le_obj *o = &obj[i];
        o->vsize = u32(e); o->link = u32(e + 4); o->flags = u32(e + 8);
        o->pmi = u32(e + 12); o->npages = u32(e + 16);
        uint32_t sz = o->vsize > o->npages * ps ? o->vsize : o->npages * ps;
        o->base = next;
        o->size = (sz + ps - 1) / ps * ps;
        next = (next + sz + 0xffff) & ~0xffffu;
        if (o->base + o->size > MEM_SIZE) { free(d); snprintf(err, errlen, "object %u out of range", i + 1); return -1; }
        memset(MEM + o->base, 0, o->size);
        for (uint32_t p = 0; p < o->npages; p++) {
            uint32_t pnum = o->pmi + p;
            long off = pages + (long)(pnum - 1) * ps;
            uint32_t len = pnum == npages ? last_page : ps;
            if (off + (long)len > n) len = off < n ? (uint32_t)(n - off) : 0;
            memcpy(MEM + o->base + p * ps, d + off, len);
        }
    }
    /* fixups (same rules as tools/le.py _apply_fixups) */
    for (uint32_t pidx = 0; pidx < npages; pidx++) {
        uint32_t pnum = pidx + 1;
        struct le_obj *o = NULL; uint32_t page_off = 0;
        for (uint32_t i = 0; i < nobj; i++)
            if (obj[i].pmi <= pnum && pnum < obj[i].pmi + obj[i].npages) { o = &obj[i]; page_off = (pnum - obj[i].pmi) * ps; }
        if (!o) continue;
        long p = frt + (long)u32(d + fpt + 4 * pidx), end = frt + (long)u32(d + fpt + 4 * (pidx + 1));
        while (p < end) {
            uint8_t src_t = d[p], fl = d[p + 1]; p += 2;
            if ((fl & 3) || (fl & 4)) { free(d); snprintf(err, errlen, "unsupported fixup type %02x/%02x", src_t, fl); return -1; }
            int srclist = (src_t & 0x20) != 0, cnt = 1;
            int16_t srcoff = 0;
            if (srclist) cnt = d[p++]; else { srcoff = (int16_t)u16(d + p); p += 2; }
            uint32_t tobj;
            if (fl & 0x40) { tobj = u16(d + p); p += 2; } else tobj = d[p++];
            int stype = src_t & 0x0f;
            uint32_t toff = 0;
            if (stype != 2) { if (fl & 0x10) { toff = u32(d + p); p += 4; } else { toff = u16(d + p); p += 2; } }
            if (tobj < 1 || tobj > nobj) { free(d); snprintf(err, errlen, "bad fixup target object %u", tobj); return -1; }
            const struct le_obj *t = &obj[tobj - 1];
            uint32_t target = t->base + toff;
            uint16_t sel = selector_for(t, (int)tobj);
            for (int k = 0; k < cnt; k++) {
                int32_t so = srclist ? (int16_t)u16(d + p + 2 * k) : srcoff;
                int32_t at = (int32_t)page_off + so;
                uint8_t *q = MEM + o->base + at;
                int room4 = at >= 0 && (uint32_t)at + 4 <= o->size, room2 = at >= 0 && (uint32_t)at + 2 <= o->size;
                int room6 = at >= 0 && (uint32_t)at + 6 <= o->size;
                switch (stype) {
                case 7: if (room4) memcpy(q, &target, 4); break;                    /* off32 */
                case 8: if (room4) { uint32_t r = target - (o->base + (uint32_t)at + 4); memcpy(q, &r, 4); } break;
                case 5: if (room2) { uint16_t v = (uint16_t)target; memcpy(q, &v, 2); } break;
                case 2: if (room2) memcpy(q, &sel, 2); break;                       /* sel16 */
                case 6: if (room6) { memcpy(q, &target, 4); memcpy(q + 4, &sel, 2); } break;
                case 3: if (room4) { uint16_t v = (uint16_t)toff; memcpy(q, &v, 2); memcpy(q + 2, &sel, 2); } break;
                default: free(d); snprintf(err, errlen, "unsupported fixup source type %d", stype); return -1;
                }
            }
            if (srclist) p += 2 * cnt;
        }
    }
    *entry = obj[eip_obj - 1].base + eip;
    *stack = obj[esp_obj - 1].base + esp_;
    for (uint32_t i = 0; i < nobj; i++) placed((int)i, obj[i].base, obj[i].size, obj[i].flags);
    free(d);
    return 0;
}
