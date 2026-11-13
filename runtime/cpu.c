/* CPU primitives: lazy flags, rotates, BCD, x87 helpers, string ops. */
#include <stdio.h>
#include <stdlib.h>
#include "cpu.h"
#include "rt.h"
#include <stdlib.h>

uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
uint16_t es, cs, ss, ds, fs, gs;
uint32_t es_base, cs_base, ss_base, ds_base, fs_base, gs_base;
int df, if_flag = 1;
volatile int irq_pending;
uint32_t rt_eip;
uint8_t *MEM;

uint32_t lf_op = LF_FLAGS * 4 + 2, lf_a, lf_b, lf_res = 0, lf_cf;

static const uint32_t szmask[3] = {0xffu, 0xffffu, 0xffffffffu};
static const uint32_t szsign[3] = {0x80u, 0x8000u, 0x80000000u};
static const uint32_t szbits[3] = {8, 16, 32};

#define KIND (lf_op >> 2)
#define SZ (lf_op & 3)

uint32_t flags_cf(void) {
    uint32_t m = szmask[SZ];
    switch (KIND) {
    case LF_ADD: return (lf_res & m) < (lf_a & m);
    case LF_ADC: return lf_cf ? (lf_res & m) <= (lf_a & m) : (lf_res & m) < (lf_a & m);
    case LF_SUB: return (lf_a & m) < (lf_b & m);
    case LF_SBB: return lf_cf ? (lf_a & m) <= (lf_b & m) : (lf_a & m) < (lf_b & m);
    case LF_LOGIC: return 0;
    case LF_INC: case LF_DEC: return lf_cf & 1;
    case LF_NEG: return (lf_b & m) != 0;
    case LF_SHL: {
        uint32_t n = lf_b, bits = szbits[SZ];
        if (n > bits) return 0;
        return ((uint64_t)(lf_a & m) >> (bits - n)) & 1;
    }
    case LF_SHR: {
        uint32_t n = lf_b;
        if (n > szbits[SZ]) return 0;
        return ((uint64_t)(lf_a & m) >> (n - 1)) & 1;
    }
    case LF_SAR: {
        uint32_t n = lf_b, bits = szbits[SZ];
        int64_t v = (int64_t)(int32_t)(lf_a << (32 - bits)) >> (32 - bits);
        if (n >= bits) return (lf_a & szsign[SZ]) != 0;
        return (v >> (n - 1)) & 1;
    }
    case LF_MUL: return lf_cf & 1;
    case LF_FLAGS: return lf_res & 1;
    }
    return 0;
}

uint32_t flags_zf(void) {
    if (KIND == LF_FLAGS) return (lf_res >> 6) & 1;
    return (lf_res & szmask[SZ]) == 0;
}

uint32_t flags_sf(void) {
    if (KIND == LF_FLAGS) return (lf_res >> 7) & 1;
    return (lf_res & szsign[SZ]) != 0;
}

uint32_t flags_pf(void) {
    if (KIND == LF_FLAGS) return (lf_res >> 2) & 1;
    return !__builtin_parity(lf_res & 0xff);
}

static uint32_t flags_af(void) {
    switch (KIND) {
    case LF_ADD: case LF_ADC: case LF_SUB: case LF_SBB: case LF_INC: case LF_DEC:
        return ((lf_a ^ lf_b ^ lf_res) >> 4) & 1;
    case LF_NEG: return (lf_b & 0xf) != 0;
    case LF_FLAGS: return (lf_res >> 4) & 1;
    }
    return 0;
}

uint32_t flags_of(void) {
    uint32_t s = szsign[SZ];
    switch (KIND) {
    case LF_ADD: case LF_ADC: case LF_INC:
        return (((lf_a ^ lf_res) & (lf_b ^ lf_res)) & s) != 0;
    case LF_SUB: case LF_SBB: case LF_DEC:
        return (((lf_a ^ lf_b) & (lf_a ^ lf_res)) & s) != 0;
    case LF_NEG: return (lf_b & szmask[SZ]) == s;
    case LF_LOGIC: return 0;
    case LF_SHL: return ((lf_res & s) != 0) ^ flags_cf();
    case LF_SHR: return (lf_a & s) != 0;
    case LF_SAR: return 0;
    case LF_MUL: return lf_cf & 1;
    case LF_FLAGS: return (lf_res >> 11) & 1;
    }
    return 0;
}

uint32_t flags_arith(void) {
    if (KIND == LF_FLAGS) return lf_res & 0x8d5;
    return flags_cf() | (flags_pf() << 2) | (flags_af() << 4) | (flags_zf() << 6) |
           (flags_sf() << 7) | (flags_of() << 11);
}

void rt_set_arith_flags(uint32_t f) {
    lf_op = LF_FLAGS * 4 + 2;
    lf_res = f & 0x8d5;
}

void rt_set_cf(uint32_t c) {
    rt_set_arith_flags((flags_arith() & ~1u) | (c & 1));
}

uint32_t rt_get_eflags(void) {
    return flags_arith() | 2 | (if_flag ? 0x200 : 0) | (df ? 0x400 : 0);
}

void rt_set_eflags(uint32_t f) {
    rt_set_arith_flags(f);
    df = (f >> 10) & 1;
    int was = if_flag;
    if_flag = (f >> 9) & 1;
    if (!was && if_flag) irq_pending = 1;
}

/* ---- rotates: set CF and OF explicitly ---------------------------------- */
static void set_cf_of(uint32_t cf, uint32_t of) {
    uint32_t f = flags_arith() & ~(1u | 0x800u);
    rt_set_arith_flags(f | (cf & 1) | ((of & 1) << 11));
}

#define ROT(bits, T)                                                              \
T rt_rol##bits(T v, uint32_t n) {                                                 \
    n &= 31; if (!n) return v;                                                     \
    uint32_t k = n % bits; T r = k ? (T)((v << k) | (v >> (bits - k))) : v;        \
    set_cf_of(r & 1, ((r >> (bits - 1)) ^ r) & 1);                                 \
    return r;                                                                      \
}                                                                                  \
T rt_ror##bits(T v, uint32_t n) {                                                 \
    n &= 31; if (!n) return v;                                                     \
    uint32_t k = n % bits; T r = k ? (T)((v >> k) | (v << (bits - k))) : v;        \
    set_cf_of((r >> (bits - 1)) & 1, ((r >> (bits - 1)) ^ (r >> (bits - 2))) & 1); \
    return r;                                                                      \
}                                                                                  \
T rt_rcl##bits(T v, uint32_t n) {                                                 \
    n = (n & 31) % (bits + 1); if (!n) return v;                                   \
    uint32_t cf = flags_cf();                                                      \
    for (uint32_t i = 0; i < n; i++) {                                             \
        uint32_t out = (v >> (bits - 1)) & 1; v = (T)((v << 1) | cf); cf = out;    \
    }                                                                              \
    set_cf_of(cf, ((v >> (bits - 1)) & 1) ^ cf);                                   \
    return v;                                                                      \
}                                                                                  \
T rt_rcr##bits(T v, uint32_t n) {                                                 \
    n = (n & 31) % (bits + 1); if (!n) return v;                                   \
    uint32_t cf = flags_cf();                                                      \
    uint32_t of = ((v >> (bits - 1)) & 1) ^ cf;                                    \
    for (uint32_t i = 0; i < n; i++) {                                             \
        uint32_t out = v & 1; v = (T)((v >> 1) | ((T)cf << (bits - 1))); cf = out; \
    }                                                                              \
    set_cf_of(cf, of);                                                             \
    return v;                                                                      \
}
ROT(8, uint8_t)
ROT(16, uint16_t)
ROT(32, uint32_t)

uint32_t rt_shld32(uint32_t d, uint32_t s, uint32_t n) {
    n &= 31; if (!n) return d;
    uint32_t r = (d << n) | (s >> (32 - n));
    LF_SET(LF_LOGIC * 4 + 2, 0, 0, r, 0);
    uint32_t cf = (d >> (32 - n)) & 1;
    set_cf_of(cf, ((r >> 31) ^ cf) & 1);
    return r;
}
uint32_t rt_shrd32(uint32_t d, uint32_t s, uint32_t n) {
    n &= 31; if (!n) return d;
    uint32_t r = (d >> n) | (s << (32 - n));
    LF_SET(LF_LOGIC * 4 + 2, 0, 0, r, 0);
    uint32_t cf = (d >> (n - 1)) & 1;
    set_cf_of(cf, ((r >> 31) ^ (d >> 31)) & 1);
    return r;
}
uint16_t rt_shld16(uint16_t d, uint16_t s, uint32_t n) {
    n &= 31; if (!n) return d;
    uint32_t v = ((uint32_t)d << 16) | s;
    uint16_t r = (uint16_t)((n < 32 ? (uint64_t)v << n : 0) >> 16);
    LF_SET(LF_LOGIC * 4 + 1, 0, 0, r, 0);
    set_cf_of((v >> (32 - n)) & 1, 0);
    return r;
}
uint16_t rt_shrd16(uint16_t d, uint16_t s, uint32_t n) {
    n &= 31; if (!n) return d;
    uint32_t v = ((uint32_t)s << 16) | d;
    uint16_t r = (uint16_t)(v >> n);
    LF_SET(LF_LOGIC * 4 + 1, 0, 0, r, 0);
    set_cf_of((v >> (n - 1)) & 1, 0);
    return r;
}

/* ---- BCD ---------------------------------------------------------------- */
#define SET_AL(v) (eax = (eax & 0xffffff00u) | (uint8_t)(v))
void rt_daa(void) {
    uint8_t al = (uint8_t)eax; uint32_t cf = flags_cf(), af = flags_af(), ncf = 0, naf = 0;
    uint8_t old = al;
    if ((al & 0xf) > 9 || af) { al += 6; naf = 1; ncf = cf || old > 0xf9; }
    if (old > 0x99 || cf) { al += 0x60; ncf = 1; }
    SET_AL(al);
    LF_SET(LF_LOGIC * 4, 0, 0, al, 0);
    rt_set_arith_flags((flags_arith() & ~0x11u) | ncf | (naf << 4));
}
void rt_das(void) {
    uint8_t al = (uint8_t)eax; uint32_t cf = flags_cf(), af = flags_af(), ncf = 0, naf = 0;
    uint8_t old = al;
    if ((al & 0xf) > 9 || af) { al -= 6; naf = 1; ncf = cf || old < 6; }
    if (old > 0x99 || cf) { al -= 0x60; ncf = 1; }
    SET_AL(al);
    LF_SET(LF_LOGIC * 4, 0, 0, al, 0);
    rt_set_arith_flags((flags_arith() & ~0x11u) | ncf | (naf << 4));
}
void rt_aaa(void) {
    uint32_t f = flags_arith() & ~0x11u;
    if ((eax & 0xf) > 9 || flags_af()) {
        eax = (eax & 0xffff0000u) | (uint16_t)((uint16_t)eax + 0x106); f |= 0x11;
    }
    eax &= 0xffffff0fu;
    rt_set_arith_flags(f);
}
void rt_aas(void) {
    uint32_t f = flags_arith() & ~0x11u;
    if ((eax & 0xf) > 9 || flags_af()) {
        eax = (eax & 0xffff0000u) | (uint16_t)((uint16_t)eax - 6);
        eax = (eax & 0xffff00ffu) | ((((eax >> 8) - 1) & 0xff) << 8); f |= 0x11;
    }
    eax &= 0xffffff0fu;
    rt_set_arith_flags(f);
}

/* ---- x87 ----------------------------------------------------------------- */
long double fpr[8];
int ftop;
uint16_t fpu_sw, fpu_cw = 0x037f;

long double RF80(uint32_t a) {
    uint8_t b[16] = {0};
    for (int i = 0; i < 10; i++) b[i] = RB(a + i);
    long double v; memcpy(&v, b, sizeof(v) < 16 ? sizeof(v) : 16); return v;
}
void WF80(uint32_t a, long double v) {
    uint8_t b[16] = {0};
    memcpy(b, &v, sizeof(v) < 16 ? sizeof(v) : 16);
    for (int i = 0; i < 10; i++) WB(a + i, b[i]);
}

void fpu_compare(long double a, long double b) {
    fpu_sw &= ~0x4500;
    if (isnan(a) || isnan(b)) fpu_sw |= 0x4500;
    else if (a < b) fpu_sw |= 0x0100;
    else if (a == b) fpu_sw |= 0x4000;
}

void fpu_xam(void) {
    long double v = ST(0);
    fpu_sw &= ~0x4700;
    if (signbit(v)) fpu_sw |= 0x0200;
    if (isnan(v)) fpu_sw |= 0x0100;
    else if (isinf(v)) fpu_sw |= 0x0500;
    else if (v == 0) fpu_sw |= 0x4000;
    else fpu_sw |= 0x0400;
}

void fpu_prem(void) {
    long double a = ST(0), b = ST(1);
    long double q = truncl(a / b);
    ST(0) = fmodl(a, b);
    long long iq = (long long)fabsl(q);
    fpu_sw &= ~0x4700;
    if (iq & 1) fpu_sw |= 0x0200;
    if (iq & 2) fpu_sw |= 0x4000;
    if (iq & 4) fpu_sw |= 0x0100;
}

void fpu_prem1(void) {
    ST(0) = remainderl(ST(0), ST(1));
    fpu_sw &= ~0x4700;
}

void fpu_xtract(void) {
    long double v = ST(0);
    int e;
    long double m = frexpl(v, &e);
    ST(0) = (long double)(e - 1);
    FPUSH(m * 2);
}

void fpu_init(void) {
    fpu_cw = 0x037f; fpu_sw = 0; ftop = 0;
    for (int i = 0; i < 8; i++) fpr[i] = 0;
}

uint16_t fpu_get_sw(void) {
    return (fpu_sw & ~0x3800) | ((ftop & 7) << 11);
}

long double fpu_round(long double v) {
    switch ((fpu_cw >> 10) & 3) {
    case 0: return nearbyintl(v);
    case 1: return floorl(v);
    case 2: return ceill(v);
    default: return truncl(v);
    }
}

int16_t fpu_to_i16(long double v) {
    long double r = fpu_round(v);
    if (!(r >= -32768.0L && r <= 32767.0L)) return (int16_t)0x8000;
    return (int16_t)r;
}
int32_t fpu_to_i32(long double v) {
    long double r = fpu_round(v);
    if (!(r >= -2147483648.0L && r <= 2147483647.0L)) return (int32_t)0x80000000u;
    return (int32_t)r;
}
int64_t fpu_to_i64(long double v) {
    long double r = fpu_round(v);
    if (!(r >= -9223372036854775808.0L && r < 9223372036854775808.0L))
        return (int64_t)0x8000000000000000ull;
    return (int64_t)r;
}

void fpu_stenv(uint32_t a) {
    WD(a, fpu_cw); WD(a + 4, fpu_get_sw()); WD(a + 8, 0);
    for (int i = 12; i < 28; i += 4) WD(a + i, 0);
}
void fpu_ldenv(uint32_t a) {
    fpu_cw = (uint16_t)RD(a);
    uint16_t sw = (uint16_t)RD(a + 4);
    fpu_sw = sw & ~0x3800; ftop = (sw >> 11) & 7;
}
void fpu_save(uint32_t a) {
    fpu_stenv(a);
    for (int i = 0; i < 8; i++) WF80(a + 28 + i * 10, ST(i));
    fpu_init();
}
void fpu_rstor(uint32_t a) {
    fpu_ldenv(a);
    for (int i = 0; i < 8; i++) ST(i) = RF80(a + 28 + i * 10);
}

/* ---- string ops ---------------------------------------------------------- */
void rt_rep_movs1(uint32_t sb) {
    if (!ecx) return;
    uint32_t s = sb + esi, d = es_base + edi;
    if (!df && !vga_planar && s + ecx <= MEM_SIZE && d + ecx <= MEM_SIZE &&
        (d >= s + ecx || s >= d + ecx || d < s)) {
        /* forward copy, non-overlapping or overlap that memmove-forward handles */
        if (d < s || d >= s + ecx) { memmove(MEM + d, MEM + s, ecx); esi += ecx; edi += ecx; ecx = 0; return; }
    }
    int step = df ? -1 : 1;
    while (ecx) { WB(es_base + edi, RB(sb + esi)); esi += step; edi += step; ecx--; }
}
void rt_rep_movs4(uint32_t sb) {
    if (!ecx) return;
    uint32_t n = ecx * 4, s = sb + esi, d = es_base + edi;
    if (!df && !vga_planar && ecx < (1u << 28) && s + n <= MEM_SIZE && d + n <= MEM_SIZE &&
        (d + n <= s || s + n <= d || d < s)) {
        memmove(MEM + d, MEM + s, n); esi += n; edi += n; ecx = 0; return;
    }
    int step = df ? -4 : 4;
    while (ecx) { WD(es_base + edi, RD(sb + esi)); esi += step; edi += step; ecx--; }
}
void rt_rep_stos1(void) {
    if (!ecx) return;
    uint32_t d = es_base + edi;
    if (!df && !vga_planar && d + ecx <= MEM_SIZE) {
        memset(MEM + d, (uint8_t)eax, ecx); edi += ecx; ecx = 0; return;
    }
    int step = df ? -1 : 1;
    while (ecx) { WB(es_base + edi, (uint8_t)eax); edi += step; ecx--; }
}
void rt_rep_stos4(void) {
    if (!ecx) return;
    uint32_t d = es_base + edi;
    if (!df && !vga_planar && ecx < (1u << 28) && d + ecx * 4 <= MEM_SIZE) {
        for (uint32_t i = 0; i < ecx; i++) memcpy(MEM + d + i * 4, &eax, 4);
        edi += ecx * 4; ecx = 0; return;
    }
    int step = df ? -4 : 4;
    while (ecx) { WD(es_base + edi, eax); edi += step; ecx--; }
}

/* ---- control transfer ---------------------------------------------------- */
guest_fn rt_find_func(uint32_t a);
static int is_call_continuation(uint32_t a);
static guest_fn find_func(uint32_t a) {
    unsigned lo = 0, hi = guest_nfuncs;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (guest_funcs[mid].addr == a) return guest_funcs[mid].fn;
        if (guest_funcs[mid].addr < a) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

/* Code copied at run time (e.g. interrupt handlers moved into DOS memory):
 * find the original function whose bytes match the copy. */
#define NALIAS 64
static struct { uint32_t addr; guest_fn fn; } aliases[NALIAS];
static int naliases;

static guest_fn find_alias(uint32_t target) {
    for (int i = 0; i < naliases; i++) if (aliases[i].addr == target) return aliases[i].fn;
    if (target + 32 > MEM_SIZE) return NULL;
    guest_fn found = NULL;
    uint32_t found_addr = 0;
    int matches = 0;
    for (unsigned i = 0; i < guest_nfuncs; i++) {
        uint32_t a = guest_funcs[i].addr;
        if (a + 32 > MEM_SIZE || a == target) continue;
        if (memcmp(MEM + a, MEM + target, 32) == 0) { found = guest_funcs[i].fn; found_addr = a; matches++; }
    }
    if (matches != 1) return NULL;
    rt_log("code at %08x is a copy of function %08x", target, found_addr);
    if (naliases < NALIAS) { aliases[naliases].addr = target; aliases[naliases].fn = found; naliases++; }
    return found;
}

void rt_call(uint32_t target, uint32_t ret) {
    /* A jump to our own return address (e.g. the scan converter 0x46AB50 patches a
     * `jmp` to land on its caller's continuation, leaving results on the stack),
     * or `call $+5`: either way execution simply continues at `ret` in the caller. */
    if (target == ret) return;
    if (rt_magic_call(target)) return;
    guest_fn f = find_func(target);
    if (!f) f = find_alias(target);
    if (!f) { rt_interpret(target, ret); return; }
    f(ret);
}

guest_fn rt_find_func(uint32_t a) { return find_func(a); }

extern const uint32_t guest_conts[];
extern const unsigned guest_nconts;
static int is_call_continuation(uint32_t a) {
    unsigned lo = 0, hi = guest_nconts;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (guest_conts[mid] == a) return 1;
        if (guest_conts[mid] < a) lo = mid + 1; else hi = mid;
    }
    return 0;
}

uint32_t code_lo, code_hi;         /* recompiled code object range */

uint32_t rt_unwind;
uint32_t ftrace_ring[4096];
unsigned ftrace_pos;
uint32_t fcount[1 << 20];

void rt_dump_ftrace(int n) {
    extern void rt_log(const char *fmt, ...);
    char line[256]; int len = 0;
    if (n > 4096) n = 4096;
    rt_log("last %d function entries (oldest first):", n);
    for (int i = n; i > 0; i--) {
        len += snprintf(line + len, sizeof line - len, "%08x ", ftrace_ring[(ftrace_pos - i) & 4095]);
        if (len > 200 || i == 1) { rt_log("  %s", line); len = 0; }
    }
}

/* A guest `ret` popped an address other than the one present at function
 * entry.  Either `push addr; ret` used as a computed jump (target is a
 * function), or a return into an outer frame (call-over-data idiom):
 * unwind C frames until a call site with that continuation is reached. */

void rt_ret_mismatch(uint32_t target, uint32_t expected) {
    guest_fn f = (target < MAGIC_BASE) ? find_func(target) : NULL;
    if (f) {                       /* push addr; ret == computed jump */
        f(expected);
        return;
    }
    if (!is_call_continuation(target)) {
        /* `ret` used as a jump to code that is not a function entry
         * (threaded code, code built at run time): interpret from there */
        rt_interpret(target, expected);
        return;
    }
    rt_unwind = target;
    if (getenv("TFX_DEBUG")) { rt_log("ret mismatch: popped %08x expected %08x esp=%08x", target, expected, esp); }
}


void rt_div0(uint32_t at) {
    rt_fatal("divide error at 0x%08x", at);
}

void rt_unimpl(uint32_t at, const char *what) {
    rt_fatal("unimplemented instruction at 0x%08x: %s", at, what);
}

void rt_fallthrough(uint32_t at) {
    rt_fatal("execution fell through into undecoded code at 0x%08x", at);
}

void rt_hlt(uint32_t at) {
    rt_fatal("hlt at 0x%08x", at);
}

void rt_trace(uint32_t at) {
    fprintf(stderr, "[trace] %08x eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x\n",
            at, eax, ebx, ecx, edx, esi, edi, ebp, esp);
}

#ifdef ESP_DEBUG
void rt_esp_mismatch(uint32_t func, uint32_t esp0) {
    static int n;
    if (n++ < 20) rt_log("esp mismatch at ret of f_%08x: entry esp %08x, ret esp %08x (eip %08x)",
                         func, esp0, esp, rt_eip);
}
#endif

/* Condition of a short jcc/jmp opcode patched at run time (self-modifying). */
int rt_jcc_live(uint8_t op, uint32_t at) {
    if (op == 0xeb) return 1;
    if (op < 0x70 || op > 0x7f) rt_fatal("patched jump at %08x has opcode %02x", at, op);
    switch (op & 15) {
    case 0: return CC_O();  case 1: return CC_NO(); case 2: return CC_B();  case 3: return CC_AE();
    case 4: return CC_E();  case 5: return CC_NE(); case 6: return CC_BE(); case 7: return CC_A();
    case 8: return CC_S();  case 9: return CC_NS(); case 10: return CC_P(); case 11: return CC_NP();
    case 12: return CC_L(); case 13: return CC_GE(); case 14: return CC_LE(); default: return CC_G();
    }
}
