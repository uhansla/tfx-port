/* A small 8086/80286 real-mode interpreter, used to run the original
 * real-mode music code from START.EXE (OSL's track driver and device
 * drivers) inside the guest's first megabyte.  It is separate from the
 * protected-mode interpreter (interp.c) on purpose: nothing here can affect
 * the recompiled game.
 *
 * Memory is the guest's linear MEM (seg * 16 + off, wrapped at 1 MB).
 * Ports go to rt_inb/rt_outb.  Interrupts dispatch through the real-mode
 * vector table at linear 0; vectors that still point into segment F000h are
 * "BIOS" defaults handled here (timer tick, DOS get/set vector, ...).
 * Execution started from the host returns when it reaches F000:FFF0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

enum { AX, CX, DX, BX, SP, BP, SI, DI };
enum { ES, CS, SS, DS };
static uint16_t R[8], S[4], IP, FL;
#define F_CF 0x001
#define F_PF 0x004
#define F_AF 0x010
#define F_ZF 0x040
#define F_SF 0x080
#define F_TF 0x100
#define F_IF 0x200
#define F_DF 0x400
#define F_OF 0x800
#define BIOS_SEG 0xF000u
#define HOST_RET 0xFFF0u

static int trace_rm;
int rm86_active;                         /* inside rm86_run */

static inline uint32_t L(uint16_t seg, uint16_t off) { return (((uint32_t)seg << 4) + off) & 0xFFFFF; }
static inline uint8_t rb(uint16_t s, uint16_t o) { return MEM[L(s, o)]; }
static inline uint16_t rw(uint16_t s, uint16_t o) { return (uint16_t)(rb(s, o) | rb(s, (uint16_t)(o + 1)) << 8); }
static inline void wb(uint16_t s, uint16_t o, uint8_t v) { MEM[L(s, o)] = v; }
static inline void ww(uint16_t s, uint16_t o, uint16_t v) { wb(s, o, (uint8_t)v); wb(s, (uint16_t)(o + 1), (uint8_t)(v >> 8)); }

static inline uint8_t fetch8(void) { return rb(S[CS], IP++); }
static inline uint16_t fetch16(void) { uint16_t v = rw(S[CS], IP); IP += 2; return v; }
static inline void push(uint16_t v) { R[SP] -= 2; ww(S[SS], R[SP], v); }
static inline uint16_t pop(void) { uint16_t v = rw(S[SS], R[SP]); R[SP] += 2; return v; }

static inline uint8_t *r8p(int i) { return (uint8_t *)&R[i & 3] + (i >> 2); }   /* AL CL DL BL AH CH DH BH */

/* ---- flags ---------------------------------------------------------------- */
static int parity(uint8_t v) { v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return !(v & 1); }
static void szp(uint32_t r, int w) {
    FL &= (uint16_t)~(F_ZF | F_SF | F_PF);
    uint32_t m = w ? 0xffff : 0xff;
    if (!(r & m)) FL |= F_ZF;
    if (r & (w ? 0x8000 : 0x80)) FL |= F_SF;
    if (parity((uint8_t)r)) FL |= F_PF;
}
static void setf(uint16_t f, int on) { if (on) FL |= f; else FL &= (uint16_t)~f; }

static uint32_t alu(int op, uint32_t a, uint32_t b, int w) {
    uint32_t m = w ? 0xffff : 0xff, sb = w ? 0x8000 : 0x80, r;
    int c = FL & F_CF;
    switch (op) {
    case 0: r = a + b; setf(F_CF, r > m); setf(F_OF, ((a ^ r) & (b ^ r) & sb) != 0); setf(F_AF, ((a ^ b ^ r) & 0x10) != 0); break;
    case 1: r = a | b; FL &= (uint16_t)~(F_CF | F_OF); break;
    case 2: r = a + b + c; setf(F_CF, r > m); setf(F_OF, ((a ^ r) & (b ^ r) & sb) != 0); setf(F_AF, ((a ^ b ^ r) & 0x10) != 0); break;
    case 3: r = a - b - c; setf(F_CF, b + c > a); setf(F_OF, ((a ^ b) & (a ^ r) & sb) != 0); setf(F_AF, ((a ^ b ^ r) & 0x10) != 0); break;
    case 4: r = a & b; FL &= (uint16_t)~(F_CF | F_OF); break;
    case 5: case 7: r = a - b; setf(F_CF, b > a); setf(F_OF, ((a ^ b) & (a ^ r) & sb) != 0); setf(F_AF, ((a ^ b ^ r) & 0x10) != 0); break;
    default: r = a ^ b; FL &= (uint16_t)~(F_CF | F_OF); break;
    }
    r &= m;
    szp(r, w);
    return r;
}

static int cond(int cc) {
    int c;
    switch ((cc >> 1) & 7) {
    case 0: c = (FL & F_OF) != 0; break;
    case 1: c = (FL & F_CF) != 0; break;
    case 2: c = (FL & F_ZF) != 0; break;
    case 3: c = (FL & (F_CF | F_ZF)) != 0; break;
    case 4: c = (FL & F_SF) != 0; break;
    case 5: c = (FL & F_PF) != 0; break;
    case 6: c = ((FL & F_SF) != 0) != ((FL & F_OF) != 0); break;
    default: c = (FL & F_ZF) || (((FL & F_SF) != 0) != ((FL & F_OF) != 0)); break;
    }
    return (cc & 1) ? !c : c;
}

/* ---- ModRM (16-bit addressing) -------------------------------------------- */
static int seg_ovr;
static int m_mod, m_reg, m_rm;
static uint16_t m_seg, m_off;
static void modrm(void) {
    uint8_t b = fetch8();
    m_mod = b >> 6; m_reg = (b >> 3) & 7; m_rm = b & 7;
    if (m_mod == 3) return;
    uint16_t off = 0; int seg = DS;
    switch (m_rm) {
    case 0: off = R[BX] + R[SI]; break;
    case 1: off = R[BX] + R[DI]; break;
    case 2: off = R[BP] + R[SI]; seg = SS; break;
    case 3: off = R[BP] + R[DI]; seg = SS; break;
    case 4: off = R[SI]; break;
    case 5: off = R[DI]; break;
    case 6: if (m_mod == 0) off = fetch16(); else { off = R[BP]; seg = SS; } break;
    case 7: off = R[BX]; break;
    }
    if (m_mod == 1) off += (uint16_t)(int8_t)fetch8();
    else if (m_mod == 2) off += fetch16();
    m_off = off;
    m_seg = S[seg_ovr >= 0 ? seg_ovr : seg];
}
static uint32_t rm_read(int w) {
    if (m_mod == 3) return w ? R[m_rm] : *r8p(m_rm);
    return w ? rw(m_seg, m_off) : rb(m_seg, m_off);
}
static void rm_write(int w, uint32_t v) {
    if (m_mod == 3) { if (w) R[m_rm] = (uint16_t)v; else *r8p(m_rm) = (uint8_t)v; return; }
    if (w) ww(m_seg, m_off, (uint16_t)v); else wb(m_seg, m_off, (uint8_t)v);
}
static uint32_t reg_read(int w) { return w ? R[m_reg] : *r8p(m_reg); }
static void reg_write(int w, uint32_t v) { if (w) R[m_reg] = (uint16_t)v; else *r8p(m_reg) = (uint8_t)v; }

/* ---- shifts / rotates ----------------------------------------------------------- */
static uint32_t shift(int op, uint32_t v, int n, int w) {
    int bits = w ? 16 : 8;
    uint32_t m = w ? 0xffff : 0xff, sb = w ? 0x8000 : 0x80;
    n &= 0x1f;
    if (!n) return v;
    for (int i = 0; i < n; i++) {
        int c;
        switch (op) {
        case 0: c = (v & sb) != 0; v = ((v << 1) | c) & m; setf(F_CF, c); break;              /* rol */
        case 1: c = v & 1; v = (v >> 1) | (c ? sb : 0); setf(F_CF, c); break;                 /* ror */
        case 2: c = (v & sb) != 0; v = ((v << 1) | (FL & F_CF ? 1 : 0)) & m; setf(F_CF, c); break;  /* rcl */
        case 3: c = v & 1; v = (v >> 1) | ((FL & F_CF) ? sb : 0); setf(F_CF, c); break;       /* rcr */
        case 4: case 6: c = (v & sb) != 0; v = (v << 1) & m; setf(F_CF, c); break;           /* shl */
        case 5: c = v & 1; v >>= 1; setf(F_CF, c); break;                                     /* shr */
        case 7: c = v & 1; v = (v >> 1) | (v & sb); setf(F_CF, c); break;                     /* sar */
        }
    }
    if (op >= 4) szp(v, w);
    setf(F_OF, op == 7 ? 0 : (((v >> (bits - 1)) & 1) ^ ((FL & F_CF) ? 1 : 0)) != 0);
    return v;
}

/* ---- BIOS / DOS defaults for vectors still pointing at F000:n*4 --------------- */
void rt_bios_tick(void);
static void bios_default(int n) {
    switch (n) {
    case 0x08: rt_bios_tick(); break;
    case 0x21: {
        uint8_t ah = (uint8_t)(R[AX] >> 8), al = (uint8_t)R[AX];
        if (ah == 0x25) { ww(0, (uint16_t)(al * 4), R[DX]); ww(0, (uint16_t)(al * 4 + 2), S[DS]); }
        else if (ah == 0x35) { R[BX] = rw(0, (uint16_t)(al * 4)); S[ES] = rw(0, (uint16_t)(al * 4 + 2)); }
        else if (ah == 0x30) R[AX] = 0x0005;
        else rt_log("rm86: unhandled int 21h AH=%02x", ah);
        break;
    }
    default: break;                                   /* other vectors: no-op */
    }
}

static void do_int(int n) {
    push(FL); push(S[CS]); push(IP);
    FL &= (uint16_t)~(F_IF | F_TF);
    IP = rw(0, (uint16_t)(n * 4)); S[CS] = rw(0, (uint16_t)(n * 4 + 2));
}

/* ---- the interpreter loop ---------------------------------------------------- */
static void unsupported(uint8_t op, uint16_t at_cs, uint16_t at_ip) {
    rt_log("rm86: unsupported opcode %02x at %04x:%04x", op, at_cs, at_ip);
    IP = HOST_RET; S[CS] = BIOS_SEG;                  /* abandon this call */
}

static void run(void) {
    long budget = 50000000;                           /* runaway guard */
    while (budget-- > 0) {
        if (S[CS] == BIOS_SEG) {
            if (IP == HOST_RET) return;
            if (IP < 0x400 && !(IP & 3)) {            /* default vector n: do it, then iret */
                bios_default(IP / 4);
                IP = pop(); S[CS] = pop(); FL = pop();
                continue;
            }
        }
        uint16_t at_cs = S[CS], at_ip = IP;
        seg_ovr = -1;
        int rep = 0, o32 = 0;
        uint8_t op;
        for (;;) {
            op = fetch8();
            if (op == 0x26) seg_ovr = ES; else if (op == 0x2e) seg_ovr = CS;
            else if (op == 0x36) seg_ovr = SS; else if (op == 0x3e) seg_ovr = DS;
            else if (op == 0xf2) rep = 2; else if (op == 0xf3) rep = 3;
            else if (op == 0x66) o32 = 1;              /* only for inc/dec dword [mem] */
            else if (op == 0xf0) { } else break;
        }
        if (trace_rm) rt_log("rm86 %04x:%04x %02x ax=%04x bx=%04x cx=%04x dx=%04x si=%04x di=%04x ds=%04x es=%04x",
                             at_cs, at_ip, op, R[AX], R[BX], R[CX], R[DX], R[SI], R[DI], S[DS], S[ES]);
        int w = op & 1;
        uint32_t a, b, r;
        if (op < 0x40 && (op & 7) < 6) {             /* ALU r/m,r  r,r/m  acc,imm */
            int alu_op = op >> 3, form = op & 7;
            if (form < 4) {
                modrm();
                if (form < 2) { a = rm_read(w); b = reg_read(w); r = alu(alu_op, a, b, w); if (alu_op != 7) rm_write(w, r); }
                else { w = form & 1; a = reg_read(w); b = rm_read(w); r = alu(alu_op, a, b, w); if (alu_op != 7) reg_write(w, r); }
            } else {
                w = form & 1;
                b = w ? fetch16() : fetch8();
                a = w ? R[AX] : (R[AX] & 0xff);
                r = alu(alu_op, a, b, w);
                if (alu_op != 7) { if (w) R[AX] = (uint16_t)r; else R[AX] = (uint16_t)((R[AX] & 0xff00) | r); }
            }
            continue;
        }
        switch (op) {
        case 0x06: push(S[ES]); break; case 0x07: S[ES] = pop(); break;
        case 0x0e: push(S[CS]); break;
        case 0x16: push(S[SS]); break; case 0x17: S[SS] = pop(); break;
        case 0x1e: push(S[DS]); break; case 0x1f: S[DS] = pop(); break;
        case 0x27: case 0x2f: case 0x37: case 0x3f: unsupported(op, at_cs, at_ip); break;  /* BCD adjust */
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: {
            int c = FL & F_CF; R[op & 7] = (uint16_t)alu(0, R[op & 7], 1, 1); setf(F_CF, c); break;
        }
        case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f: {
            int c = FL & F_CF; R[op & 7] = (uint16_t)alu(5, R[op & 7], 1, 1); setf(F_CF, c); break;
        }
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: {
            uint16_t v = R[op & 7]; push(v); break;
        }
        case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f: R[op & 7] = pop(); break;
        case 0x60: { uint16_t t = R[SP]; push(R[AX]); push(R[CX]); push(R[DX]); push(R[BX]); push(t); push(R[BP]); push(R[SI]); push(R[DI]); break; }
        case 0x61: R[DI] = pop(); R[SI] = pop(); R[BP] = pop(); pop(); R[BX] = pop(); R[DX] = pop(); R[CX] = pop(); R[AX] = pop(); break;
        case 0x68: push(fetch16()); break;
        case 0x6a: push((uint16_t)(int8_t)fetch8()); break;
        case 0x69: case 0x6b: {
            modrm();
            int32_t x = (int16_t)rm_read(1), y = op == 0x69 ? (int16_t)fetch16() : (int8_t)fetch8();
            int32_t p = x * y;
            R[m_reg] = (uint16_t)p;
            setf(F_CF | F_OF, p != (int16_t)p);
            break;
        }
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: case 0x7e: case 0x7f: {
            int8_t d = (int8_t)fetch8();
            if (cond(op)) IP += (uint16_t)d;
            break;
        }
        case 0x80: case 0x81: case 0x82: case 0x83: {
            modrm();
            a = rm_read(w);
            b = op == 0x81 ? fetch16() : op == 0x83 ? (uint16_t)(int8_t)fetch8() : fetch8();
            r = alu(m_reg, a, b, w);
            if (m_reg != 7) rm_write(w, r);
            break;
        }
        case 0x84: case 0x85: modrm(); alu(4, rm_read(w), reg_read(w), w); break;
        case 0x86: case 0x87: modrm(); a = rm_read(w); b = reg_read(w); rm_write(w, b); reg_write(w, a); break;
        case 0x88: case 0x89: modrm(); rm_write(w, reg_read(w)); break;
        case 0x8a: case 0x8b: modrm(); reg_write(w, rm_read(w)); break;
        case 0x8c: modrm(); rm_write(1, S[m_reg & 3]); break;
        case 0x8d: modrm(); R[m_reg] = m_off; break;
        case 0x8e: modrm(); S[m_reg & 3] = (uint16_t)rm_read(1); break;
        case 0x8f: modrm(); rm_write(1, pop()); break;
        case 0x90: break;
        case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
            uint16_t t = R[AX]; R[AX] = R[op & 7]; R[op & 7] = t; break;
        }
        case 0x98: R[AX] = (uint16_t)(int8_t)R[AX]; break;
        case 0x99: R[DX] = (R[AX] & 0x8000) ? 0xffff : 0; break;
        case 0x9a: { uint16_t o = fetch16(), s = fetch16(); push(S[CS]); push(IP); S[CS] = s; IP = o; break; }
        case 0x9c: push(FL | 0xf002); break;
        case 0x9d: FL = (uint16_t)(pop() & 0x0fd5); break;
        case 0x9e: FL = (uint16_t)((FL & 0xff00) | ((R[AX] >> 8) & 0xd5)); break;
        case 0x9f: R[AX] = (uint16_t)((R[AX] & 0xff) | ((FL & 0xd5) | 2) << 8); break;
        case 0xa0: case 0xa1: { uint16_t o = fetch16(); uint16_t s = S[seg_ovr >= 0 ? seg_ovr : DS];
            if (w) R[AX] = rw(s, o); else R[AX] = (uint16_t)((R[AX] & 0xff00) | rb(s, o)); break; }
        case 0xa2: case 0xa3: { uint16_t o = fetch16(); uint16_t s = S[seg_ovr >= 0 ? seg_ovr : DS];
            if (w) ww(s, o, R[AX]); else wb(s, o, (uint8_t)R[AX]); break; }
        case 0xa4: case 0xa5: case 0xa6: case 0xa7: case 0xaa: case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf: {
            int step = (FL & F_DF) ? -(1 + w) : (1 + w);
            uint16_t src = S[seg_ovr >= 0 ? seg_ovr : DS];
            for (;;) {
                if (rep && !R[CX]) break;
                switch (op & 0xfe) {
                case 0xa4: if (w) ww(S[ES], R[DI], rw(src, R[SI])); else wb(S[ES], R[DI], rb(src, R[SI])); R[SI] += (uint16_t)step; R[DI] += (uint16_t)step; break;
                case 0xa6: a = w ? rw(src, R[SI]) : rb(src, R[SI]); b = w ? rw(S[ES], R[DI]) : rb(S[ES], R[DI]); alu(5, a, b, w); R[SI] += (uint16_t)step; R[DI] += (uint16_t)step; break;
                case 0xaa: if (w) ww(S[ES], R[DI], R[AX]); else wb(S[ES], R[DI], (uint8_t)R[AX]); R[DI] += (uint16_t)step; break;
                case 0xac: if (w) R[AX] = rw(src, R[SI]); else R[AX] = (uint16_t)((R[AX] & 0xff00) | rb(src, R[SI])); R[SI] += (uint16_t)step; break;
                case 0xae: a = w ? R[AX] : (R[AX] & 0xff); b = w ? rw(S[ES], R[DI]) : rb(S[ES], R[DI]); alu(5, a, b, w); R[DI] += (uint16_t)step; break;
                }
                if (!rep) break;
                R[CX]--;
                if ((op & 0xfe) == 0xa6 || (op & 0xfe) == 0xae) {
                    if (rep == 3 && !(FL & F_ZF)) break;
                    if (rep == 2 && (FL & F_ZF)) break;
                }
            }
            break;
        }
        case 0xa8: alu(4, R[AX] & 0xff, fetch8(), 0); break;
        case 0xa9: alu(4, R[AX], fetch16(), 1); break;
        case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7: *r8p(op & 7) = fetch8(); break;
        case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf: R[op & 7] = fetch16(); break;
        case 0xc0: case 0xc1: case 0xd0: case 0xd1: case 0xd2: case 0xd3: {
            w = op & 1;
            modrm();
            int n = (op <= 0xc1) ? fetch8() : (op <= 0xd1) ? 1 : (R[CX] & 0xff);
            rm_write(w, shift(m_reg, rm_read(w), n, w));
            break;
        }
        case 0xc2: { uint16_t n = fetch16(); IP = pop(); R[SP] += n; break; }
        case 0xc3: IP = pop(); break;
        case 0xc4: case 0xc5: modrm(); R[m_reg] = rw(m_seg, m_off); S[op == 0xc4 ? ES : DS] = rw(m_seg, (uint16_t)(m_off + 2)); break;
        case 0xc6: case 0xc7: modrm(); rm_write(w, w ? fetch16() : fetch8()); break;
        case 0xc8: { uint16_t n = fetch16(); uint8_t lv = fetch8(); push(R[BP]); uint16_t fp = R[SP];
            for (int i = 1; i < lv; i++) { R[BP] -= 2; push(rw(S[SS], R[BP])); }
            if (lv) push(fp);
            R[BP] = fp; R[SP] -= n; break; }
        case 0xc9: R[SP] = R[BP]; R[BP] = pop(); break;
        case 0xca: { uint16_t n = fetch16(); IP = pop(); S[CS] = pop(); R[SP] += n; break; }
        case 0xcb: IP = pop(); S[CS] = pop(); break;
        case 0xcc: do_int(3); break;
        case 0xcd: do_int(fetch8()); break;
        case 0xce: if (FL & F_OF) do_int(4); break;
        case 0xcf: IP = pop(); S[CS] = pop(); FL = (uint16_t)(pop() & 0x0fd5); break;
        case 0xd7: { uint16_t s = S[seg_ovr >= 0 ? seg_ovr : DS]; R[AX] = (uint16_t)((R[AX] & 0xff00) | rb(s, (uint16_t)(R[BX] + (R[AX] & 0xff)))); break; }
        case 0xe0: case 0xe1: case 0xe2: {
            int8_t d = (int8_t)fetch8();
            R[CX]--;
            int c = R[CX] != 0;
            if (op == 0xe0) c = c && !(FL & F_ZF);
            if (op == 0xe1) c = c && (FL & F_ZF);
            if (c) IP += (uint16_t)d;
            break;
        }
        case 0xe3: { int8_t d = (int8_t)fetch8(); if (!R[CX]) IP += (uint16_t)d; break; }
        case 0xe4: { uint8_t p = fetch8(); R[AX] = (uint16_t)((R[AX] & 0xff00) | rt_inb(p)); break; }
        case 0xe5: { uint8_t p = fetch8(); R[AX] = rt_inw(p); break; }
        case 0xe6: { uint8_t p = fetch8(); rt_outb(p, (uint8_t)R[AX]); break; }
        case 0xe7: { uint8_t p = fetch8(); rt_outw(p, R[AX]); break; }
        case 0xe8: { int16_t d = (int16_t)fetch16(); push(IP); IP += (uint16_t)d; break; }
        case 0xe9: { int16_t d = (int16_t)fetch16(); IP += (uint16_t)d; break; }
        case 0xea: { uint16_t o = fetch16(), s = fetch16(); IP = o; S[CS] = s; break; }
        case 0xeb: { int8_t d = (int8_t)fetch8(); IP += (uint16_t)d; break; }
        case 0xec: R[AX] = (uint16_t)((R[AX] & 0xff00) | rt_inb(R[DX])); break;
        case 0xed: R[AX] = rt_inw(R[DX]); break;
        case 0xee: rt_outb(R[DX], (uint8_t)R[AX]); break;
        case 0xef: rt_outw(R[DX], R[AX]); break;
        case 0xf4: return;                            /* hlt: give control back */
        case 0xf5: FL ^= F_CF; break;
        case 0xf6: case 0xf7: {
            modrm();
            switch (m_reg) {
            case 0: case 1: alu(4, rm_read(w), w ? fetch16() : fetch8(), w); break;
            case 2: rm_write(w, ~rm_read(w) & (w ? 0xffff : 0xff)); break;
            case 3: a = rm_read(w); r = alu(5, 0, a, w); rm_write(w, r); setf(F_CF, a != 0); break;
            case 4: if (w) { uint32_t p = (uint32_t)R[AX] * rm_read(1); R[AX] = (uint16_t)p; R[DX] = (uint16_t)(p >> 16); setf(F_CF | F_OF, R[DX] != 0); }
                    else { uint16_t p = (uint16_t)((R[AX] & 0xff) * rm_read(0)); R[AX] = p; setf(F_CF | F_OF, (p >> 8) != 0); } break;
            case 5: if (w) { int32_t p = (int16_t)R[AX] * (int16_t)rm_read(1); R[AX] = (uint16_t)p; R[DX] = (uint16_t)(p >> 16); setf(F_CF | F_OF, p != (int16_t)p); }
                    else { int16_t p = (int16_t)((int8_t)R[AX] * (int8_t)rm_read(0)); R[AX] = (uint16_t)p; setf(F_CF | F_OF, p != (int8_t)p); } break;
            case 6: if (w) { uint32_t d = rm_read(1), n = (uint32_t)R[DX] << 16 | R[AX]; if (!d || n / d > 0xffff) { do_int(0); break; } R[AX] = (uint16_t)(n / d); R[DX] = (uint16_t)(n % d); }
                    else { uint32_t d = rm_read(0), n = R[AX]; if (!d || n / d > 0xff) { do_int(0); break; } R[AX] = (uint16_t)((n % d) << 8 | (n / d)); } break;
            case 7: if (w) { int32_t d = (int16_t)rm_read(1), n = (int32_t)((uint32_t)R[DX] << 16 | R[AX]); if (!d || n / d > 32767 || n / d < -32768) { do_int(0); break; } R[AX] = (uint16_t)(n / d); R[DX] = (uint16_t)(n % d); }
                    else { int32_t d = (int8_t)rm_read(0), n = (int16_t)R[AX]; if (!d || n / d > 127 || n / d < -128) { do_int(0); break; } R[AX] = (uint16_t)(((uint8_t)(n % d)) << 8 | (uint8_t)(n / d)); } break;
            }
            break;
        }
        case 0xf8: FL &= (uint16_t)~F_CF; break; case 0xf9: FL |= F_CF; break;
        case 0xfa: FL &= (uint16_t)~F_IF; break; case 0xfb: FL |= F_IF; break;
        case 0xfc: FL &= (uint16_t)~F_DF; break; case 0xfd: FL |= F_DF; break;
        case 0xfe: case 0xff: {
            modrm();
            if (o32 && w && m_mod != 3 && m_reg < 2) {   /* inc/dec dword ptr [mem] (track driver tick count) */
                uint32_t v = (uint32_t)rw(m_seg, m_off) | (uint32_t)rw(m_seg, (uint16_t)(m_off + 2)) << 16;
                uint32_t r2 = m_reg == 0 ? v + 1 : v - 1;
                ww(m_seg, m_off, (uint16_t)r2); ww(m_seg, (uint16_t)(m_off + 2), (uint16_t)(r2 >> 16));
                setf(F_ZF, r2 == 0); setf(F_SF, (r2 >> 31) & 1);
                break;
            }
            if (o32) { unsupported(op, at_cs, at_ip); break; }
            switch (m_reg) {
            case 0: { int c = FL & F_CF; rm_write(w, alu(0, rm_read(w), 1, w)); setf(F_CF, c); break; }
            case 1: { int c = FL & F_CF; rm_write(w, alu(5, rm_read(w), 1, w)); setf(F_CF, c); break; }
            case 2: { uint16_t t = (uint16_t)rm_read(1); push(IP); IP = t; break; }
            case 3: { uint16_t o = rw(m_seg, m_off), s = rw(m_seg, (uint16_t)(m_off + 2)); push(S[CS]); push(IP); S[CS] = s; IP = o; break; }
            case 4: IP = (uint16_t)rm_read(1); break;
            case 5: { uint16_t o = rw(m_seg, m_off), s = rw(m_seg, (uint16_t)(m_off + 2)); IP = o; S[CS] = s; break; }
            case 6: push((uint16_t)rm_read(1)); break;
            default: unsupported(op, at_cs, at_ip); break;
            }
            break;
        }
        case 0x0f: {
            uint8_t op2 = fetch8();
            if (op2 >= 0x80 && op2 <= 0x8f) {         /* jcc near (386) */
                int16_t d = (int16_t)fetch16();
                if (cond(op2)) IP += (uint16_t)d;
            } else unsupported(op2, at_cs, at_ip);
            break;
        }
        default: unsupported(op, at_cs, at_ip); break;
        }
    }
    rt_log("rm86: instruction budget exhausted at %04x:%04x", S[CS], IP);
}

/* ---- host entry points -------------------------------------------------------------- */
static uint16_t stack_seg = 0;
int dos_alloc(uint16_t paras, uint16_t *seg, uint16_t *maxavail);

void rm86_init(void) {
    trace_rm = getenv("TFX_TRACE_RM") != NULL;
    for (int n = 0; n < 256; n++)                     /* default vectors: F000:n*4 */
        if (!rw(0, (uint16_t)(n * 4)) && !rw(0, (uint16_t)(n * 4 + 2))) { ww(0, (uint16_t)(n * 4), (uint16_t)(n * 4)); ww(0, (uint16_t)(n * 4 + 2), BIOS_SEG); }
    uint16_t maxav;
    if (!stack_seg && !dos_alloc(0x100, &stack_seg, &maxav)) rt_log("rm86: no stack memory");
}

static void enter(uint16_t cs_, uint16_t ip_) {
    S[SS] = stack_seg; R[SP] = 0x0ffe;
    push(FL); push((uint16_t)BIOS_SEG); push((uint16_t)HOST_RET);   /* iret/retf frame back to host */
    S[CS] = cs_; IP = ip_;
}

/* far call cs:ip with the given registers (retf or iret both return) */
void rm86_far_call(uint16_t cs_, uint16_t ip_, uint16_t *regs8, uint16_t *segs4,
                   const uint16_t *args, int nargs) {
    if (regs8) memcpy(R, regs8, sizeof R);
    if (segs4) memcpy(S, segs4, sizeof S);
    FL = 0x0202;
    uint16_t ss = stack_seg;
    S[SS] = ss; R[SP] = 0x0ffe;
    for (int i = nargs - 1; i >= 0; i--) push(args[i]);                /* C (cdecl) arguments */
    push((uint16_t)BIOS_SEG); push((uint16_t)HOST_RET);                 /* retf frame */
    S[CS] = cs_; IP = ip_;
    rm86_active++;
    run();
    rm86_active--;
    if (regs8) { memcpy(regs8, R, sizeof R); regs8[SP] = 0; }
    if (segs4) memcpy(segs4, S, sizeof S);
}

/* software / hardware interrupt n through the real-mode vector table */
void rm86_int(int n, uint16_t *regs8, uint16_t *segs4, uint16_t *flags) {
    if (regs8) memcpy(R, regs8, sizeof R);
    if (segs4) memcpy(S, segs4, sizeof S);
    FL = flags ? *flags : 0x0202;
    uint16_t vip = rw(0, (uint16_t)(n * 4)), vcs = rw(0, (uint16_t)(n * 4 + 2));
    if (vcs == BIOS_SEG) { bios_default(n); goto out; }
    enter(vcs, vip);
    rm86_active++;
    run();
    rm86_active--;
out:
    if (regs8) memcpy(regs8, R, sizeof R);
    if (segs4) memcpy(segs4, S, sizeof S);
    if (flags) *flags = FL;
}

/* is real-mode vector n hooked by real-mode code? */
int rm86_hooked(int n) { return rw(0, (uint16_t)(n * 4 + 2)) != BIOS_SEG && rw(0, (uint16_t)(n * 4 + 2)) != 0; }
