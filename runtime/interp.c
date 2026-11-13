/* x86-32 interpreter: fallback for guest code that was not statically
 * recompiled (code generated at run time, copied to the stack, or entered
 * at an address the analyzer did not know).  It shares all state with the
 * recompiled code and hands control back as soon as execution reaches a
 * recompiled function. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "rt.h"

extern guest_fn rt_find_func(uint32_t a);

static uint32_t ip;
static int opsz32, seg_ovr;           /* seg_ovr: -1 or index es..gs */
static int no_seg;                    /* lea: effective address only */
static uint32_t seg_base_of(int s) {
    switch (s) {
    case 0: return es_base; case 1: return cs_base; case 2: return ss_base;
    case 3: return ds_base; case 4: return fs_base; default: return gs_base;
    }
}

static inline uint8_t fetch8(void) { return RB(ip++); }
static inline uint16_t fetch16(void) { uint16_t v = RW(ip); ip += 2; return v; }
static inline uint32_t fetch32(void) { uint32_t v = RD(ip); ip += 4; return v; }

static uint32_t *reg32[8] = {&eax, &ecx, &edx, &ebx, &esp, &ebp, &esi, &edi};

static uint32_t getreg(int r, int sz) {
    if (sz == 4) return *reg32[r];
    if (sz == 2) return (uint16_t)*reg32[r];
    return r < 4 ? (uint8_t)*reg32[r] : (uint8_t)(*reg32[r - 4] >> 8);
}
static void setreg(int r, int sz, uint32_t v) {
    if (sz == 4) *reg32[r] = v;
    else if (sz == 2) *reg32[r] = (*reg32[r] & 0xffff0000u) | (uint16_t)v;
    else if (r < 4) *reg32[r] = (*reg32[r] & 0xffffff00u) | (uint8_t)v;
    else *reg32[r - 4] = (*reg32[r - 4] & 0xffff00ffu) | ((uint32_t)(uint8_t)v << 8);
}

/* ---- ModRM ---------------------------------------------------------------- */
static int m_mod, m_reg, m_rm;
static uint32_t m_ea;                 /* linear address (with segment base) */

static void decode_modrm(void) {
    uint8_t b = fetch8();
    m_mod = b >> 6; m_reg = (b >> 3) & 7; m_rm = b & 7;
    if (m_mod == 3) return;
    uint32_t a = 0;
    int def_ss = 0;
    if (m_rm == 4) {
        uint8_t sib = fetch8();
        int sc = sib >> 6, idx = (sib >> 3) & 7, base = sib & 7;
        if (base == 5 && m_mod == 0) a = fetch32();
        else { a = *reg32[base]; if (base == 4 || base == 5) def_ss = 1; }
        if (idx != 4) a += *reg32[idx] << sc;
    } else if (m_rm == 5 && m_mod == 0) {
        a = fetch32();
    } else {
        a = *reg32[m_rm];
        if (m_rm == 5) def_ss = 1;
    }
    if (m_mod == 1) a += (int8_t)fetch8();
    else if (m_mod == 2) a += fetch32();
    m_ea = no_seg ? a : a + (seg_ovr >= 0 ? seg_base_of(seg_ovr) : (def_ss ? ss_base : ds_base));
}

static uint32_t rm_read(int sz) {
    if (m_mod == 3) return getreg(m_rm, sz);
    return sz == 4 ? RD(m_ea) : sz == 2 ? RW(m_ea) : RB(m_ea);
}
static void rm_write(int sz, uint32_t v) {
    if (m_mod == 3) { setreg(m_rm, sz, v); return; }
    if (sz == 4) WD(m_ea, v); else if (sz == 2) WW(m_ea, (uint16_t)v); else WB(m_ea, (uint8_t)v);
}

static const uint32_t MSK[5] = {0, 0xff, 0xffff, 0, 0xffffffffu};
static int szi(int sz) { return sz == 1 ? 0 : sz == 2 ? 1 : 2; }

/* ALU op 0..7: add or adc sbb and sub xor cmp */
static uint32_t alu(int op, int sz, uint32_t a, uint32_t b) {
    uint32_t m = MSK[sz], r = 0, c;
    a &= m; b &= m;
    switch (op) {
    case 0: r = (a + b) & m; LF_SET(LF_ADD * 4 + szi(sz), a, b, r, 0); break;
    case 1: r = (a | b); LF_SET(LF_LOGIC * 4 + szi(sz), 0, 0, r, 0); break;
    case 2: c = flags_cf(); r = (a + b + c) & m; LF_SET(LF_ADC * 4 + szi(sz), a, b, r, c); break;
    case 3: c = flags_cf(); r = (a - b - c) & m; LF_SET(LF_SBB * 4 + szi(sz), a, b, r, c); break;
    case 4: r = a & b; LF_SET(LF_LOGIC * 4 + szi(sz), 0, 0, r, 0); break;
    case 5: case 7: r = (a - b) & m; LF_SET(LF_SUB * 4 + szi(sz), a, b, r, 0); break;
    case 6: r = a ^ b; LF_SET(LF_LOGIC * 4 + szi(sz), 0, 0, r, 0); break;
    }
    return r;
}

static uint32_t shift_op(int op, int sz, uint32_t v, uint32_t n) {
    n &= 31;
    if (!n) return v;
    uint32_t m = MSK[sz], r;
    v &= m;
    switch (op) {
    case 0: r = sz == 4 ? rt_rol32(v, n) : sz == 2 ? rt_rol16((uint16_t)v, n) : rt_rol8((uint8_t)v, n); break;
    case 1: r = sz == 4 ? rt_ror32(v, n) : sz == 2 ? rt_ror16((uint16_t)v, n) : rt_ror8((uint8_t)v, n); break;
    case 2: r = sz == 4 ? rt_rcl32(v, n) : sz == 2 ? rt_rcl16((uint16_t)v, n) : rt_rcl8((uint8_t)v, n); break;
    case 3: r = sz == 4 ? rt_rcr32(v, n) : sz == 2 ? rt_rcr16((uint16_t)v, n) : rt_rcr8((uint8_t)v, n); break;
    case 4: case 6: r = (uint32_t)(((uint64_t)v << n) & m); LF_SET(LF_SHL * 4 + szi(sz), v, n, r, 0); break;
    case 5: r = (uint32_t)((uint64_t)v >> n); LF_SET(LF_SHR * 4 + szi(sz), v, n, r, 0); break;
    default: {
        int32_t sv = sz == 4 ? (int32_t)v : sz == 2 ? (int16_t)v : (int8_t)v;
        r = (uint32_t)(sv >> (n > 31 ? 31 : n)) & m;
        LF_SET(LF_SAR * 4 + szi(sz), v, n, r, 0);
    }
    }
    return r;
}

static int cond(int cc) {
    switch (cc & 15) {
    case 0: return CC_O(); case 1: return CC_NO(); case 2: return CC_B(); case 3: return CC_AE();
    case 4: return CC_E(); case 5: return CC_NE(); case 6: return CC_BE(); case 7: return CC_A();
    case 8: return CC_S(); case 9: return CC_NS(); case 10: return CC_P(); case 11: return CC_NP();
    case 12: return CC_L(); case 13: return CC_GE(); case 14: return CC_LE(); default: return CC_G();
    }
}


/* ---- main loop -------------------------------------------------------------- */
static int step_mode;
static uint32_t iring[256], iesp[256];
uint32_t itrace_lo, itrace_hi;     /* detailed per-instruction log window */
static unsigned ipos;
void rt_dump_iring(void) {
    char line[300]; int len = 0;
    rt_log("last interpreted instructions (oldest first):");
    for (int i = 256; i > 0; i--) {
        len += snprintf(line + len, sizeof line - len, "%08x/%05x ", iring[(ipos - i) & 255], iesp[(ipos - i) & 255] & 0xfffff);
        if (len > 240 || i == 1) { rt_log("  %s", line); len = 0; }
    }
}

/* TFX_INTERP=lo-hi[,lo-hi...] (hex): functions whose entry is in a range run
 * in the interpreter instead of their recompiled code (differential tests). */
int rt_interp_any;
static uint32_t irange[16][2];
static int nirange;
void rt_interp_set(const char *s) {
    nirange = 0;

    while (s && *s && nirange < 16) {
        char *e;
        irange[nirange][0] = (uint32_t)strtoul(s, &e, 16);
        irange[nirange][1] = (*e == '-') ? (uint32_t)strtoul(e + 1, &e, 16) : irange[nirange][0] + 1;
        nirange++;
        s = (*e == ',') ? e + 1 : e;
        if (!*e) break;
    }
    rt_interp_any = nirange > 0;
    if (rt_interp_any) rt_log("interpreting %d address range(s)", nirange);
}
void rt_interp_init(void) { rt_interp_set(getenv("TFX_INTERP")); }
int rt_interp_hit(uint32_t a) {
    for (int k = 0; k < nirange; k++) if (a >= irange[k][0] && a < irange[k][1]) return 1;
    return 0;
}
static uint32_t own_func;            /* function being interpreted on purpose */

void rt_interpret_func(uint32_t entry, uint32_t ret) {
    uint32_t save = own_func;
    if (getenv("TFX_DEBUG")) {
        char s[64]; int i;
        for (i = 0; i < 63 && eax + i < MEM_SIZE && RB(eax + i); i++) s[i] = (char)RB(eax + i);
        s[i] = 0;
        rt_log("interp func %08x eax=%08x '%s'", entry, eax, s);
    }
    own_func = entry;
    fcount[(entry >> 2) & 0xfffff]++;
    rt_interpret(entry, ret);
    own_func = save;
}

/* Execute exactly one (non control-transfer) instruction at `at`.
 * Used by recompiled code for self-modified instructions. */
void rt_step(uint32_t at) {
    uint32_t save_ip = ip;
    step_mode = 1;
    rt_interpret(at, 0);
    step_mode = 0;
    ip = save_ip;
}


/* ---- x87 (same semantics as the recompiler's translation) ---------------- */
static void x87(uint8_t opc, uint32_t at) {
    uint8_t modrm = RB(ip);
    int reg = (modrm >> 3) & 7, rm = modrm & 7;
    if (modrm < 0xc0) {
        decode_modrm();
        uint32_t ea = m_ea;
        if (opc == 0xd8 || opc == 0xdc || opc == 0xda || opc == 0xde) {
            long double src = opc == 0xd8 ? (long double)RF32(ea) : opc == 0xdc ? (long double)RF64(ea)
                            : opc == 0xda ? (long double)(int32_t)RD(ea) : (long double)(int16_t)RW(ea);
            switch (reg) {
            case 0: ST(0) = ST(0) + src; break;
            case 1: ST(0) = ST(0) * src; break;
            case 2: fpu_compare(ST(0), src); break;
            case 3: fpu_compare(ST(0), src); FPOP(); break;
            case 4: ST(0) = ST(0) - src; break;
            case 5: ST(0) = src - ST(0); break;
            case 6: ST(0) = ST(0) / src; break;
            case 7: ST(0) = src / ST(0); break;
            }
            return;
        }
        switch (opc) {
        case 0xd9:
            switch (reg) {
            case 0: { long double v = RF32(ea); FPUSH(v); return; }
            case 2: WF32(ea, (float)ST(0)); return;
            case 3: WF32(ea, (float)ST(0)); FPOP(); return;
            case 4: fpu_ldenv(ea); return;
            case 5: fpu_cw = RW(ea); return;
            case 6: fpu_stenv(ea); return;
            case 7: WW(ea, fpu_cw); return;
            }
            break;
        case 0xdb:
            switch (reg) {
            case 0: { long double v = (int32_t)RD(ea); FPUSH(v); return; }
            case 2: WD(ea, (uint32_t)fpu_to_i32(ST(0))); return;
            case 3: WD(ea, (uint32_t)fpu_to_i32(ST(0))); FPOP(); return;
            case 5: { long double v = RF80(ea); FPUSH(v); return; }
            case 7: WF80(ea, ST(0)); FPOP(); return;
            }
            break;
        case 0xdd:
            switch (reg) {
            case 0: { long double v = RF64(ea); FPUSH(v); return; }
            case 2: WF64(ea, (double)ST(0)); return;
            case 3: WF64(ea, (double)ST(0)); FPOP(); return;
            case 4: fpu_rstor(ea); return;
            case 6: fpu_save(ea); return;
            case 7: WW(ea, fpu_get_sw()); return;
            }
            break;
        case 0xdf:
            switch (reg) {
            case 0: { long double v = (int16_t)RW(ea); FPUSH(v); return; }
            case 2: WW(ea, (uint16_t)fpu_to_i16(ST(0))); return;
            case 3: WW(ea, (uint16_t)fpu_to_i16(ST(0))); FPOP(); return;
            case 5: { long double v = (int64_t)RQ(ea); FPUSH(v); return; }
            case 7: WQ(ea, (uint64_t)fpu_to_i64(ST(0))); FPOP(); return;
            }
            break;
        }
        rt_fatal("interp: x87 %02x /%d (mem) at %08x", opc, reg, at);
    }
    ip++;                                   /* register form: consume modrm */
    switch (opc) {
    case 0xd8:
        switch (reg) {
        case 0: ST(0) = ST(0) + ST(rm); return;
        case 1: ST(0) = ST(0) * ST(rm); return;
        case 2: fpu_compare(ST(0), ST(rm)); return;
        case 3: fpu_compare(ST(0), ST(rm)); FPOP(); return;
        case 4: ST(0) = ST(0) - ST(rm); return;
        case 5: ST(0) = ST(rm) - ST(0); return;
        case 6: ST(0) = ST(0) / ST(rm); return;
        case 7: ST(0) = ST(rm) / ST(0); return;
        }
        break;
    case 0xdc: case 0xde: {
        if (opc == 0xde && modrm == 0xd9) { fpu_compare(ST(0), ST(1)); FPOP(); FPOP(); return; }
        switch (reg) {
        case 0: ST(rm) = ST(rm) + ST(0); break;
        case 1: ST(rm) = ST(rm) * ST(0); break;
        case 2: fpu_compare(ST(0), ST(rm)); break;
        case 3: fpu_compare(ST(0), ST(rm)); FPOP(); break;
        case 4: ST(rm) = ST(0) - ST(rm); break;
        case 5: ST(rm) = ST(rm) - ST(0); break;
        case 6: ST(rm) = ST(0) / ST(rm); break;
        case 7: ST(rm) = ST(rm) / ST(0); break;
        }
        if (opc == 0xde) FPOP();
        return;
    }
    case 0xd9:
        if (reg == 0) { long double v = ST(rm); FPUSH(v); return; }
        if (reg == 1) { long double v = ST(0); ST(0) = ST(rm); ST(rm) = v; return; }
        switch (modrm) {
        case 0xd0: return;
        case 0xe0: ST(0) = -ST(0); return;
        case 0xe1: ST(0) = fabsl(ST(0)); return;
        case 0xe4: fpu_compare(ST(0), 0.0L); return;
        case 0xe5: fpu_xam(); return;
        case 0xe8: FPUSH(1.0L); return;
        case 0xe9: FPUSH(3.321928094887362347870L); return;
        case 0xea: FPUSH(1.442695040888963407360L); return;
        case 0xeb: FPUSH(3.141592653589793238463L); return;
        case 0xec: FPUSH(0.301029995663981195214L); return;
        case 0xed: FPUSH(0.693147180559945309417L); return;
        case 0xee: FPUSH(0.0L); return;
        case 0xf0: ST(0) = exp2l(ST(0)) - 1.0L; return;
        case 0xf1: ST(1) = ST(1) * log2l(ST(0)); FPOP(); return;
        case 0xf2: ST(0) = tanl(ST(0)); FPUSH(1.0L); fpu_sw &= ~0x400; return;
        case 0xf3: ST(1) = atan2l(ST(1), ST(0)); FPOP(); return;
        case 0xf4: fpu_xtract(); return;
        case 0xf5: fpu_prem1(); return;
        case 0xf6: ftop = (ftop - 1) & 7; return;
        case 0xf7: ftop = (ftop + 1) & 7; return;
        case 0xf8: fpu_prem(); return;
        case 0xf9: ST(1) = ST(1) * log2l(ST(0) + 1.0L); FPOP(); return;
        case 0xfa: ST(0) = sqrtl(ST(0)); return;
        case 0xfb: { long double v = ST(0); ST(0) = sinl(v); FPUSH(cosl(v)); fpu_sw &= ~0x400; return; }
        case 0xfc: ST(0) = fpu_round(ST(0)); return;
        case 0xfd: ST(0) = ldexpl(ST(0), (int)truncl(ST(1))); return;
        case 0xfe: ST(0) = sinl(ST(0)); fpu_sw &= ~0x400; return;
        case 0xff: ST(0) = cosl(ST(0)); fpu_sw &= ~0x400; return;
        }
        break;
    case 0xda:
        if (modrm == 0xe9) { fpu_compare(ST(0), ST(1)); FPOP(); FPOP(); return; }
        break;
    case 0xdb:
        if (modrm == 0xe2) { fpu_sw &= 0x7f00; return; }
        if (modrm == 0xe3) { fpu_init(); return; }
        if (modrm == 0xe0 || modrm == 0xe1 || modrm == 0xe4) return;
        break;
    case 0xdd:
        switch (reg) {
        case 0: return;
        case 2: ST(rm) = ST(0); return;
        case 3: ST(rm) = ST(0); FPOP(); return;
        case 4: fpu_compare(ST(0), ST(rm)); return;
        case 5: fpu_compare(ST(0), ST(rm)); FPOP(); return;
        }
        break;
    case 0xdf:
        if (modrm == 0xe0) { setreg(0, 2, fpu_get_sw()); return; }
        break;
    }
    rt_fatal("interp: x87 %02x %02x at %08x", opc, modrm, at);
}

static void interp_run(uint32_t start, uint32_t expected_ret);
/* push/pop honouring the 0x66 operand-size prefix (osz is 2 or 4) */
#define PUSHV(v) do { if (osz == 4) PUSH32(v); else PUSH16(v); } while (0)
#define POPV() (osz == 4 ? POP32() : (uint32_t)POP16())

/* Re-entrant wrapper: interpreted code may call recompiled functions that in
 * turn run interpreted code (nested), so the decoder state is saved. */
void rt_interpret(uint32_t start, uint32_t expected_ret) {
    uint32_t s_ip = ip; int s_op = opsz32, s_seg = seg_ovr;
    interp_run(start, expected_ret);
    ip = s_ip; opsz32 = s_op; seg_ovr = s_seg;
}

static void interp_run(uint32_t start, uint32_t expected_ret) {
    ip = start;
    if (!step_mode && start != own_func) {
        /* log each distinct entry point once, with the code bytes */
        static uint32_t seen[256];
        static int nseen;
        int k;
        for (k = 0; k < nseen; k++) if (seen[k] == start) break;
        if (k == nseen && nseen < 256) {
            seen[nseen++] = start;
            char hex[3 * 48 + 1];
            for (int b = 0; b < 48; b++) snprintf(hex + 3 * b, 4, "%02x ", RB(start + b));
            rt_log("interpreter: entering at %08x (expect ret %08x) code: %s", start, expected_ret, hex);
        }
    }
    long long count = 0;
    for (;;) {
        if (step_mode && count) return;
        /* control reached the caller's continuation by a jump rather than a ret
         * (patched `jmp` back to the caller, see 0x46AC29): hand back to it */
        if (count && ip == expected_ret) return;
        /* reaching a recompiled function entry: hand over */
        if (!step_mode && (ip != start || count)) {
            guest_fn f = ip == own_func ? NULL : rt_find_func(ip);
            if (f) { f(expected_ret); return; }
        }
        count++;
        if ((count & 0xffff) == 0) IRQ_CHECK();
        uint32_t at = ip;
        iesp[ipos & 255] = esp; iring[ipos++ & 255] = at;
        if (at >= itrace_lo && at < itrace_hi)
            rt_log("I %08x esp=%08x [esp]=%08x [esp+4]=%08x eax=%08x ebp=%08x ebx=%08x %02x %02x %02x",
                   at, esp, RD(esp), RD(esp + 4), eax, ebp, ebx, RB(at), RB(at + 1), RB(at + 2));
        if (at < 0x10000u || at >= MEM_SIZE - 16) rt_fatal("interp: execution reached bad address %08x", at);
        rt_eip = at;
        opsz32 = 1; seg_ovr = -1;
        int rep = 0, addr16 = 0;
        uint8_t op;
        for (;;) {
            op = fetch8();
            if (op == 0x66) opsz32 = 0;
            else if (op == 0x26) seg_ovr = 0; else if (op == 0x2e) seg_ovr = 1;
            else if (op == 0x36) seg_ovr = 2; else if (op == 0x3e) seg_ovr = 3;
            else if (op == 0x64) seg_ovr = 4; else if (op == 0x65) seg_ovr = 5;
            else if (op == 0xf3) rep = 3; else if (op == 0xf2) rep = 2;
            else if (op == 0x67) addr16 = 1;      /* only jcxz/loop use it (CX instead of ECX) */
            else if (op == 0xf0) { }
            else break;
        }
        int osz = opsz32 ? 4 : 2;
        uint32_t a, b, r;
        if (step_mode && ((op >= 0x70 && op <= 0x7f) || op == 0xc2 || op == 0xc3 || op == 0xca || op == 0xcb || op == 0xcf || op == 0xe8 ||
                          op == 0xe9 || op == 0xeb || op == 0xe2 || op == 0xe3 ||
                          (op == 0x0f && RB(ip) >= 0x80 && RB(ip) <= 0x8f)))
            rt_fatal("self-modified control transfer at %08x", at);
        switch (op) {
        /* ALU r/m,r  r,r/m  acc,imm */
        case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05:
        case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d:
        case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
        case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d:
        case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25:
        case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2c: case 0x2d:
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35:
        case 0x38: case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d: {
            int aop = op >> 3, form = op & 7, sz = (form & 1) ? osz : 1;
            if (form < 4) {
                decode_modrm();
                if (form & 2) { a = getreg(m_reg, sz); b = rm_read(sz); r = alu(aop, sz, a, b); if (aop != 7) setreg(m_reg, sz, r); }
                else { a = rm_read(sz); b = getreg(m_reg, sz); r = alu(aop, sz, a, b); if (aop != 7) rm_write(sz, r); }
            } else {
                b = sz == 1 ? fetch8() : sz == 2 ? fetch16() : fetch32();
                r = alu(aop, sz, getreg(0, sz), b);
                if (aop != 7) setreg(0, sz, r);
            }
            break;
        }
        case 0x06: PUSHV(es); break;
        case 0x07: rt_load_seg(0, (uint16_t)POPV()); break;
        case 0x0e: PUSHV(cs); break;
        case 0x16: PUSHV(ss); break;
        case 0x17: rt_load_seg(2, (uint16_t)POPV()); break;
        case 0x1e: PUSHV(ds); break;
        case 0x1f: rt_load_seg(3, (uint16_t)POPV()); break;
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: {
            uint32_t cf = flags_cf(); a = getreg(op & 7, osz); r = (a + 1) & MSK[osz];
            LF_SET(LF_INC * 4 + szi(osz), a, 1, r, cf); setreg(op & 7, osz, r); break;
        }
        case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f: {
            uint32_t cf = flags_cf(); a = getreg(op & 7, osz); r = (a - 1) & MSK[osz];
            LF_SET(LF_DEC * 4 + szi(osz), a, 1, r, cf); setreg(op & 7, osz, r); break;
        }
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
            if (osz == 4) PUSH32(*reg32[op & 7]); else PUSH16(*reg32[op & 7]);
            break;
        case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f:
            if (osz == 4) { r = POP32(); *reg32[op & 7] = r; } else setreg(op & 7, 2, POP16());
            break;
        case 0x60: { uint32_t t = esp; PUSH32(eax); PUSH32(ecx); PUSH32(edx); PUSH32(ebx); PUSH32(t);
                     PUSH32(ebp); PUSH32(esi); PUSH32(edi); break; }
        case 0x61: edi = POP32(); esi = POP32(); ebp = POP32(); esp += 4; ebx = POP32();
                   edx = POP32(); ecx = POP32(); eax = POP32(); break;
        case 0x68: if (osz == 4) PUSH32(fetch32()); else PUSH16(fetch16()); break;
        case 0x6a: PUSHV((uint32_t)(int8_t)fetch8()); break;
        case 0x69: case 0x6b: {
            decode_modrm();
            int32_t s = osz == 4 ? (int32_t)rm_read(4) : (int16_t)rm_read(2);
            int32_t imm = op == 0x6b ? (int8_t)fetch8() : (osz == 4 ? (int32_t)fetch32() : (int16_t)fetch16());
            int64_t p = (int64_t)s * imm;
            setreg(m_reg, osz, (uint32_t)p);
            LF_SET(LF_MUL * 4 + szi(osz), 0, 0, (uint32_t)p, osz == 4 ? p != (int32_t)p : p != (int16_t)p);
            break;
        }
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: case 0x7e: case 0x7f: {
            int8_t d = (int8_t)fetch8();
            if (cond(op)) ip += d;
            break;
        }
        case 0x80: case 0x81: case 0x83: {
            int sz = op == 0x80 ? 1 : osz;
            decode_modrm();
            uint32_t imm = op == 0x81 ? (sz == 2 ? fetch16() : fetch32()) : (uint32_t)(int8_t)fetch8();
            r = alu(m_reg, sz, rm_read(sz), imm);
            if (m_reg != 7) rm_write(sz, r);
            break;
        }
        case 0x84: case 0x85: {
            int sz = op == 0x84 ? 1 : osz; decode_modrm();
            alu(4, sz, rm_read(sz), getreg(m_reg, sz)); break;
        }
        case 0x86: case 0x87: {
            int sz = op == 0x86 ? 1 : osz; decode_modrm();
            a = rm_read(sz); rm_write(sz, getreg(m_reg, sz)); setreg(m_reg, sz, a); break;
        }
        case 0x88: case 0x89: { int sz = op == 0x88 ? 1 : osz; decode_modrm(); rm_write(sz, getreg(m_reg, sz)); break; }
        case 0x8a: case 0x8b: { int sz = op == 0x8a ? 1 : osz; decode_modrm(); setreg(m_reg, sz, rm_read(sz)); break; }
        case 0x8c: {
            decode_modrm();
            uint16_t sv[6] = {es, cs, ss, ds, fs, gs};
            if (m_mod == 3) setreg(m_rm, osz, sv[m_reg]); else WW(m_ea, sv[m_reg]);
            break;
        }
        case 0x8d: {
            no_seg = 1; decode_modrm(); no_seg = 0;
            setreg(m_reg, osz, m_ea); break;
        }
        case 0x8e: decode_modrm(); rt_load_seg(m_reg, (uint16_t)rm_read(2)); break;
        case 0x8f: { r = POPV(); decode_modrm(); rm_write(osz, r); break; }
        case 0x90: break;
        case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
            a = getreg(0, osz); setreg(0, osz, getreg(op & 7, osz)); setreg(op & 7, osz, a); break;
        case 0x98: if (osz == 4) eax = (uint32_t)(int16_t)eax; else setreg(0, 2, (uint16_t)(int8_t)eax); break;
        case 0x99: if (osz == 4) edx = ((int32_t)eax < 0) ? 0xffffffffu : 0; else setreg(2, 2, ((int16_t)eax < 0) ? 0xffff : 0); break;
        case 0x9c: PUSHV(rt_get_eflags()); break;
        case 0x9d: if (osz == 4) rt_set_eflags(POP32()); else rt_set_eflags((rt_get_eflags() & 0xffff0000u) | POP16()); break;
        case 0x9e: rt_set_arith_flags((rt_get_eflags() & ~0xd5u) | ((eax >> 8) & 0xd5u)); break;
        case 0x9f: setreg(4, 1, (rt_get_eflags() & 0xd5) | 2); break;
        case 0xa0: case 0xa1: case 0xa2: case 0xa3: {
            int sz = (op & 1) ? osz : 1;
            uint32_t ad = fetch32() + (seg_ovr >= 0 ? seg_base_of(seg_ovr) : ds_base);
            if (op < 0xa2) setreg(0, sz, sz == 4 ? RD(ad) : sz == 2 ? RW(ad) : RB(ad));
            else { if (sz == 4) WD(ad, eax); else if (sz == 2) WW(ad, (uint16_t)eax); else WB(ad, (uint8_t)eax); }
            break;
        }
        case 0xa4: case 0xa5: case 0xaa: case 0xab: case 0xac: case 0xad: case 0xa6: case 0xa7: case 0xae: case 0xaf: {
            int sz = (op & 1) ? osz : 1;
            int step = df ? -sz : sz;
            uint32_t sb = seg_ovr >= 0 ? seg_base_of(seg_ovr) : ds_base;
            for (;;) {
                if (rep && !ecx) break;
                switch (op) {
                case 0xa4: case 0xa5:
                    if (sz == 4) WD(es_base + edi, RD(sb + esi)); else if (sz == 2) WW(es_base + edi, RW(sb + esi)); else WB(es_base + edi, RB(sb + esi));
                    esi += step; edi += step; break;
                case 0xaa: case 0xab:
                    if (sz == 4) WD(es_base + edi, eax); else if (sz == 2) WW(es_base + edi, (uint16_t)eax); else WB(es_base + edi, (uint8_t)eax);
                    edi += step; break;
                case 0xac: case 0xad:
                    setreg(0, sz, sz == 4 ? RD(sb + esi) : sz == 2 ? RW(sb + esi) : RB(sb + esi)); esi += step; break;
                case 0xa6: case 0xa7:
                    alu(7, sz, sz == 4 ? RD(sb + esi) : sz == 2 ? RW(sb + esi) : RB(sb + esi),
                        sz == 4 ? RD(es_base + edi) : sz == 2 ? RW(es_base + edi) : RB(es_base + edi));
                    esi += step; edi += step; break;
                case 0xae: case 0xaf:
                    alu(7, sz, getreg(0, sz), sz == 4 ? RD(es_base + edi) : sz == 2 ? RW(es_base + edi) : RB(es_base + edi));
                    edi += step; break;
                }
                if (!rep) break;
                ecx--;
                if ((op == 0xa6 || op == 0xa7 || op == 0xae || op == 0xaf) &&
                    ((rep == 3 && !CC_E()) || (rep == 2 && CC_E()))) break;
            }
            break;
        }
        case 0xa8: alu(4, 1, eax, fetch8()); break;
        case 0xa9: alu(4, osz, eax, osz == 4 ? fetch32() : fetch16()); break;
        case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7:
            setreg(op & 7, 1, fetch8()); break;
        case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
            setreg(op & 7, osz, osz == 4 ? fetch32() : fetch16()); break;
        case 0xc0: case 0xc1: case 0xd0: case 0xd1: case 0xd2: case 0xd3: {
            int sz = (op & 1) ? osz : 1;
            decode_modrm();
            uint32_t n = (op <= 0xc1) ? fetch8() : (op <= 0xd1) ? 1 : (uint8_t)ecx;
            rm_write(sz, shift_op(m_reg, sz, rm_read(sz), n));
            break;
        }
        case 0xc2: case 0xc3: {          /* ret [imm16] */
            uint16_t n = op == 0xc2 ? fetch16() : 0;
            uint32_t t = POP32();
            esp += n;
            if (t == expected_ret) return;
            {
                guest_fn f = rt_find_func(t);
                if (f) { f(expected_ret); return; }
            }
            ip = t;                      /* keep interpreting */
            break;
        }
        case 0xcf: {                     /* iretd */
            uint32_t t = POP32(); POP32(); uint32_t fl = POP32();
            rt_set_eflags(fl);
            if (t == expected_ret || t == MAGIC_RET) return;
            rt_fatal("interp: iretd to %08x at %08x", t, at);
        }
        case 0xca: case 0xcb: {          /* retf [n] */
            uint16_t n = op == 0xca ? fetch16() : 0;
            uint32_t t = POP32(); POP32(); esp += n;
            if (t == expected_ret || t == MAGIC_RET) return;
            ip = t;
            break;
        }
        case 0xc6: case 0xc7: {
            int sz = op == 0xc6 ? 1 : osz; decode_modrm();
            rm_write(sz, sz == 1 ? fetch8() : sz == 2 ? fetch16() : fetch32()); break;
        }
        case 0xc8: { uint16_t n = fetch16(); fetch8(); PUSH32(ebp); ebp = esp; esp -= n; break; }
        case 0xc9: esp = ebp; ebp = POP32(); break;
        case 0xcd: rt_int(fetch8()); break;
        case 0xe2: { int8_t d = (int8_t)fetch8();
                     if (addr16) { setreg(1, 2, (uint16_t)(ecx - 1)); if ((uint16_t)ecx) ip += d; }
                     else if (--ecx) ip += d;
                     break; }
        case 0xe3: { int8_t d = (int8_t)fetch8(); if (addr16 ? !(uint16_t)ecx : !ecx) ip += d; break; }
        case 0xe4: setreg(0, 1, rt_inb(fetch8())); break;
        case 0xe6: rt_outb(fetch8(), (uint8_t)eax); break;
        case 0xe8: {
            int32_t d = (int32_t)fetch32();
            uint32_t t = ip + d, ret = ip;
            PUSH32(ret);
            guest_fn f = rt_find_func(t);
            if (f) { f(ret); if (rt_unwind) { if (rt_unwind == ret) rt_unwind = 0; else return; } }
            else { fcount[(t >> 2) & 0xfffff]++; ip = t; }  /* stay in this loop: ret pops `ret` */
            break;
        }
        case 0xe9: ip += (int32_t)fetch32(); break;
        case 0xeb: ip += (int8_t)fetch8(); break;
        case 0xec: setreg(0, 1, rt_inb((uint16_t)edx)); break;
        case 0xed: setreg(0, osz, osz == 4 ? rt_ind((uint16_t)edx) : rt_inw((uint16_t)edx)); break;
        case 0xee: rt_outb((uint16_t)edx, (uint8_t)eax); break;
        case 0xef: if (osz == 4) rt_outd((uint16_t)edx, eax); else rt_outw((uint16_t)edx, (uint16_t)eax); break;
        case 0xf5: rt_set_cf(!flags_cf()); break;
        case 0xd7: {                     /* xlatb */
            uint32_t sb = seg_ovr >= 0 ? seg_base_of(seg_ovr) : ds_base;
            setreg(0, 1, RB(sb + ebx + (uint8_t)eax));
            break;
        }
        case 0xe0: case 0xe1: {          /* loopne / loope */
            int8_t d = (int8_t)fetch8();
            int z = CC_E();
            if (--ecx && (op == 0xe1 ? z : !z)) ip += d;
            break;
        }
        case 0xe5: setreg(0, osz, osz == 4 ? rt_ind(fetch8()) : rt_inw(fetch8())); break;
        case 0xe7: { uint8_t p = fetch8(); if (osz == 4) rt_outd(p, eax); else rt_outw(p, (uint16_t)eax); break; }
        case 0x9b: break;                /* wait */
        case 0xc4: case 0xc5: {          /* les / lds */
            decode_modrm();
            uint32_t o = RD(m_ea); uint16_t s = RW(m_ea + 4);
            rt_load_seg(op == 0xc4 ? 0 : 3, s);
            setreg(m_reg, 4, o);
            break;
        }
        case 0xd4: { uint8_t base = fetch8(); uint8_t al = (uint8_t)eax;
                     setreg(4, 1, al / base); setreg(0, 1, al % base);
                     LF_SET(LF_LOGIC * 4, 0, 0, (uint8_t)eax, 0); break; }
        case 0xd5: { uint8_t base = fetch8(); uint8_t r2 = (uint8_t)(((eax >> 8) & 0xff) * base + (eax & 0xff));
                     setreg(0, 2, r2); LF_SET(LF_LOGIC * 4, 0, 0, r2, 0); break; }
        case 0x27: rt_daa(); break;
        case 0x2f: rt_das(); break;
        case 0x37: rt_aaa(); break;
        case 0x3f: rt_aas(); break;
        case 0x6c: case 0x6d: case 0x6e: case 0x6f: {   /* ins / outs (byte, word/dword) */
            int sz = (op & 1) ? osz : 1, step = df ? -sz : sz;
            uint32_t sb = seg_ovr >= 0 ? seg_base_of(seg_ovr) : ds_base;
            uint16_t port = (uint16_t)edx;
            for (;;) {
                if (rep && !ecx) break;
                if (op >= 0x6e) {
                    if (sz == 1) rt_outb(port, RB(sb + esi));
                    else if (sz == 2) rt_outw(port, RW(sb + esi));
                    else rt_outd(port, RD(sb + esi));
                    esi += step;
                } else {
                    if (sz == 1) WB(es_base + edi, rt_inb(port));
                    else if (sz == 2) WW(es_base + edi, rt_inw(port));
                    else WD(es_base + edi, rt_ind(port));
                    edi += step;
                }
                if (!rep) break;
                ecx--;
            }
            break;
        }
        case 0xf6: case 0xf7: {
            int sz = op == 0xf6 ? 1 : osz;
            decode_modrm();
            uint32_t v = rm_read(sz);
            switch (m_reg) {
            case 0: case 1: alu(4, sz, v, sz == 1 ? fetch8() : sz == 2 ? fetch16() : fetch32()); break;
            case 2: rm_write(sz, ~v); break;
            case 3: r = (0 - v) & MSK[sz]; LF_SET(LF_NEG * 4 + szi(sz), 0, v, r, 0); rm_write(sz, r); break;
            case 4: if (sz == 4) { uint64_t p = (uint64_t)eax * v; eax = (uint32_t)p; edx = (uint32_t)(p >> 32); LF_SET(LF_MUL * 4 + 2, 0, 0, eax, edx != 0); }
                    else if (sz == 2) { uint32_t p = (uint16_t)eax * v; setreg(0, 2, p); setreg(2, 2, p >> 16); LF_SET(LF_MUL * 4 + 1, 0, 0, p, (p >> 16) != 0); }
                    else { uint32_t p = (uint8_t)eax * v; setreg(0, 2, p); LF_SET(LF_MUL * 4, 0, 0, p, (p >> 8) != 0); }
                    break;
            case 5: if (sz == 4) { int64_t p = (int64_t)(int32_t)eax * (int32_t)v; eax = (uint32_t)p; edx = (uint32_t)(p >> 32); LF_SET(LF_MUL * 4 + 2, 0, 0, eax, p != (int32_t)p); }
                    else if (sz == 2) { int32_t p = (int16_t)eax * (int16_t)v; setreg(0, 2, p); setreg(2, 2, p >> 16); LF_SET(LF_MUL * 4 + 1, 0, 0, p, p != (int16_t)p); }
                    else { int16_t p = (int8_t)eax * (int8_t)v; setreg(0, 2, (uint16_t)p); LF_SET(LF_MUL * 4, 0, 0, p, p != (int8_t)p); }
                    break;
            case 6: if (!v) rt_div0(at);
                    if (sz == 4) { uint64_t n = ((uint64_t)edx << 32) | eax; eax = (uint32_t)(n / v); edx = (uint32_t)(n % v); }
                    else if (sz == 2) { uint32_t n = ((edx & 0xffff) << 16) | (eax & 0xffff); setreg(0, 2, n / v); setreg(2, 2, n % v); }
                    else { uint32_t n = eax & 0xffff; setreg(0, 1, n / v); setreg(4, 1, n % v); }
                    break;
            case 7: if (!v) rt_div0(at);
                    if (sz == 4) { int64_t n = (int64_t)(((uint64_t)edx << 32) | eax); eax = (uint32_t)(n / (int32_t)v); edx = (uint32_t)(n % (int32_t)v); }
                    else if (sz == 2) { int32_t n = (int32_t)(((edx & 0xffff) << 16) | (eax & 0xffff)); setreg(0, 2, n / (int16_t)v); setreg(2, 2, n % (int16_t)v); }
                    else { int16_t n = (int16_t)eax; setreg(0, 1, n / (int8_t)v); setreg(4, 1, n % (int8_t)v); }
                    break;
            }
            break;
        }
        case 0xf8: rt_set_cf(0); break;
        case 0xf9: rt_set_cf(1); break;
        case 0xfa: rt_cli(); break;
        case 0xfb: rt_sti(); break;
        case 0xfc: df = 0; break;
        case 0xfd: df = 1; break;
        case 0xfe: case 0xff: {
            int sz = op == 0xfe ? 1 : osz;
            decode_modrm();
            switch (m_reg) {
            case 0: { uint32_t cf = flags_cf(); a = rm_read(sz); r = (a + 1) & MSK[sz]; LF_SET(LF_INC * 4 + szi(sz), a, 1, r, cf); rm_write(sz, r); break; }
            case 1: { uint32_t cf = flags_cf(); a = rm_read(sz); r = (a - 1) & MSK[sz]; LF_SET(LF_DEC * 4 + szi(sz), a, 1, r, cf); rm_write(sz, r); break; }
            case 2: {                    /* call r/m */
                uint32_t t = rm_read(4), ret = ip;
                PUSH32(ret);
                guest_fn f = rt_find_func(t);
                if (f) { f(ret); if (rt_unwind) { if (rt_unwind == ret) rt_unwind = 0; else return; } }
                else { fcount[(t >> 2) & 0xfffff]++; ip = t; }  /* stay in this loop: ret pops `ret` */
                break;
            }
            case 4: ip = rm_read(4); break;
            case 6: PUSHV(rm_read(osz)); break;    /* 66 FF /6 = push word (scan converter exit 0x46AC21) */
            default: rt_fatal("interp: ff /%d at %08x", m_reg, at);
            }
            break;
        }
        case 0xd8: case 0xd9: case 0xda: case 0xdb: case 0xdc: case 0xdd: case 0xde: case 0xdf:
            x87(op, at);
            break;
        case 0x0f: {
            uint8_t op2 = fetch8();
            if (op2 >= 0x80 && op2 <= 0x8f) { int32_t d = (int32_t)fetch32(); if (cond(op2)) ip += d; break; }
            if (op2 >= 0x90 && op2 <= 0x9f) { decode_modrm(); rm_write(1, cond(op2) ? 1 : 0); break; }
            switch (op2) {
            case 0xb6: decode_modrm(); setreg(m_reg, osz, rm_read(1)); break;
            case 0xb7: decode_modrm(); setreg(m_reg, osz, rm_read(2)); break;
            case 0xbe: decode_modrm(); setreg(m_reg, osz, (uint32_t)(int8_t)rm_read(1)); break;
            case 0xbf: decode_modrm(); setreg(m_reg, osz, (uint32_t)(int16_t)rm_read(2)); break;
            case 0xaf: {
                decode_modrm();
                int64_t p = osz == 4 ? (int64_t)(int32_t)getreg(m_reg, 4) * (int32_t)rm_read(4)
                                     : (int64_t)(int16_t)getreg(m_reg, 2) * (int16_t)rm_read(2);
                setreg(m_reg, osz, (uint32_t)p);
                LF_SET(LF_MUL * 4 + szi(osz), 0, 0, (uint32_t)p, osz == 4 ? p != (int32_t)p : p != (int16_t)p);
                break;
            }
            case 0xbc: case 0xbd: {      /* bsf / bsr */
                decode_modrm();
                uint32_t v = rm_read(osz);
                if (!v) { LF_SET(LF_LOGIC * 4 + 2, 0, 0, 0, 0); break; }
                setreg(m_reg, osz, op2 == 0xbc ? (uint32_t)__builtin_ctz(v) : 31u - __builtin_clz(v));
                LF_SET(LF_LOGIC * 4 + 2, 0, 0, 1, 0);
                break;
            }
            case 0xa3: case 0xab: case 0xb3: case 0xbb: case 0xba: {   /* bt/bts/btr/btc */
                decode_modrm();
                uint32_t bit, kind;
                if (op2 == 0xba) { kind = m_reg & 3; bit = fetch8(); }
                else { kind = op2 == 0xa3 ? 0 : op2 == 0xab ? 1 : op2 == 0xb3 ? 2 : 3; bit = getreg(m_reg, osz); }
                if (m_mod != 3 && op2 != 0xba) { m_ea += (int32_t)bit >> 5 << 2; bit &= 31; }
                bit &= osz * 8 - 1;
                uint32_t v = rm_read(osz);
                rt_set_cf((v >> bit) & 1);
                if (kind == 1) rm_write(osz, v | (1u << bit));
                else if (kind == 2) rm_write(osz, v & ~(1u << bit));
                else if (kind == 3) rm_write(osz, v ^ (1u << bit));
                break;
            }
            case 0xa0: PUSHV(fs); break;
            case 0xa1: rt_load_seg(4, (uint16_t)POPV()); break;
            case 0xa8: PUSHV(gs); break;
            case 0xa9: rt_load_seg(5, (uint16_t)POPV()); break;
            case 0xa4: case 0xa5: case 0xac: case 0xad: {
                decode_modrm();
                uint32_t n = (op2 == 0xa4 || op2 == 0xac) ? fetch8() : (uint8_t)ecx;
                uint32_t d = rm_read(osz), s2 = getreg(m_reg, osz);
                if (osz == 4) rm_write(4, op2 < 0xac ? rt_shld32(d, s2, n) : rt_shrd32(d, s2, n));
                else rm_write(2, op2 < 0xac ? rt_shld16((uint16_t)d, (uint16_t)s2, n) : rt_shrd16((uint16_t)d, (uint16_t)s2, n));
                break;
            }
            default: rt_fatal("interp: unsupported opcode 0f %02x at %08x", op2, at);
            }
            break;
        }
        default:
            rt_fatal("interp: unsupported opcode %02x at %08x", op, at);
        }
    }
}

