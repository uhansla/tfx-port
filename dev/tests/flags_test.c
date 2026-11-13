/* Differential test of the runtime's lazy flags and helper semantics against
 * the real CPU (32-bit build).  Each case runs the native instruction via
 * inline asm and the runtime's emulation, then compares result and the
 * arithmetic flags (CF PF AF ZF SF OF, masked to the ones the instruction
 * defines). */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include "cpu.h"

/* minimal stubs needed by cpu.c */
int vga_planar;
uint8_t vga_read8(uint32_t a) { (void)a; return 0; }
void vga_write8(uint32_t a, uint8_t v) { (void)a; (void)v; }
const struct func_entry guest_funcs[1]; const unsigned guest_nfuncs = 0;
const uint32_t guest_conts[1]; const unsigned guest_nconts = 0;

void rt_log(const char *f, ...) { (void)f; }
void rt_fatal(const char *f, ...) { printf("fatal %s\n", f); exit(1); }
int rt_magic_call(uint32_t t) { (void)t; return 0; }
void rt_interpret(uint32_t s, uint32_t e) { (void)s; (void)e; }
void rt_dump_regs(void) {}

static uint32_t rnd32(void) {
    static uint64_t x = 88172645463325252ull;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    uint32_t v = (uint32_t)x;
    switch (v & 7) {               /* bias towards edge cases */
    case 0: return 0;
    case 1: return 0xffffffffu;
    case 2: return 0x80000000u;
    case 3: return 0x7fffffffu;
    case 4: return (uint32_t)(x >> 32) & 0x1ff;
    default: return (uint32_t)(x >> 32);
    }
}

#define FL (0x8d5u)
static int fails;

#define CHECK(name, sz, a, b, cin, nres, nfl, eres, efl, mask) do {                 \
    if ((nres) != (eres) || (((nfl) ^ (efl)) & (mask))) {                           \
        if (fails++ < 40)                                                           \
            printf("%-6s sz%d a=%08x b=%08x cin=%d: native r=%08x f=%03x  emu r=%08x f=%03x\n", \
                   name, sz, (unsigned)(a), (unsigned)(b), (int)(cin), (unsigned)(nres),    \
                   (unsigned)((nfl) & FL), (unsigned)(eres), (unsigned)((efl) & FL));        \
    } } while (0)

/* native helpers: run op with given input CF, return result and flags */
#define NATIVE2(insn, reg_a, reg_b, a, b, cin, res, fl) \
    __asm__ volatile("pushfl; popl %%eax; andl $~1, %%eax; orl %4, %%eax; pushl %%eax; popfl;" \
                     insn "; pushfl; popl %1"                                     \
                     : "=&" reg_a(res), "=&r"(fl) : "0"(a), reg_b(b), "r"(cin) : "eax", "cc")

static void test_alu(void) {
    for (int it = 0; it < 200000; it++) {
        uint32_t a = rnd32(), b = rnd32(), cin = rnd32() & 1, r, f, er, ef;
        /* 32-bit */
#define T32(nm, ins, kind, emu)                                                    \
        { NATIVE2(ins " %3, %0", "c", "d", a, b, cin, r, f);                       \
          LF_SET(kind * 4 + 2, a, b, 0, cin); er = emu; lf_res = er;               \
          ef = flags_arith(); CHECK(nm, 32, a, b, cin, r, f, er, ef, FL); }
        T32("add", "addl", LF_ADD, a + b);
        T32("sub", "subl", LF_SUB, a - b);
        T32("adc", "adcl", LF_ADC, a + b + cin);
        T32("sbb", "sbbl", LF_SBB, a - b - cin);
        { NATIVE2("andl %3, %0", "c", "d", a, b, cin, r, f);
          LF_SET(LF_LOGIC * 4 + 2, 0, 0, a & b, 0); ef = flags_arith();
          CHECK("and", 32, a, b, cin, r, f, a & b, ef, FL & ~0x10u); }
        /* 8-bit */
        { uint8_t a8 = (uint8_t)a, b8 = (uint8_t)b; uint32_t r8, f8;
          __asm__ volatile("pushfl; popl %%eax; andl $~1, %%eax; orl %4, %%eax; pushl %%eax; popfl;"
                           "addb %b2, %b0; pushfl; popl %1"
                           : "=&q"(r8), "=&r"(f8) : "q"(b8), "0"((uint32_t)a8), "r"(cin) : "eax", "cc");
          LF_SET(LF_ADD * 4, a8, b8, (uint8_t)(a8 + b8), 0);
          CHECK("add8", 8, a8, b8, cin, (uint8_t)r8, f8, (uint8_t)(a8 + b8), flags_arith(), FL); }
        { uint8_t a8 = (uint8_t)a, b8 = (uint8_t)b; uint32_t r8, f8;
          __asm__ volatile("pushfl; popl %%eax; andl $~1, %%eax; orl %4, %%eax; pushl %%eax; popfl;"
                           "sbbb %b2, %b0; pushfl; popl %1"
                           : "=&q"(r8), "=&r"(f8) : "q"(b8), "0"((uint32_t)a8), "r"(cin) : "eax", "cc");
          LF_SET(LF_SBB * 4, a8, b8, (uint8_t)(a8 - b8 - cin), cin);
          CHECK("sbb8", 8, a8, b8, cin, (uint8_t)r8, f8, (uint8_t)(a8 - b8 - cin), flags_arith(), FL); }
        /* 16-bit sub (cmp) */
        { uint16_t a16 = (uint16_t)a, b16 = (uint16_t)b; uint32_t r16, f16;
          __asm__ volatile("subw %w2, %w0; pushfl; popl %1"
                           : "=&r"(r16), "=&r"(f16) : "r"(b16), "0"((uint32_t)a16) : "cc");
          LF_SET(LF_SUB * 4 + 1, a16, b16, (uint16_t)(a16 - b16), 0);
          CHECK("sub16", 16, a16, b16, 0, (uint16_t)r16, f16, (uint16_t)(a16 - b16), flags_arith(), FL); }
        /* inc / dec keep CF */
        { uint32_t f2;
          __asm__ volatile("pushfl; popl %%eax; andl $~1, %%eax; orl %3, %%eax; pushl %%eax; popfl;"
                           "decl %0; pushfl; popl %1" : "=&r"(r), "=&r"(f2) : "0"(a), "r"(cin) : "eax", "cc");
          LF_SET(LF_DEC * 4 + 2, a, 1, a - 1, cin);
          CHECK("dec", 32, a, 1, cin, r, f2, a - 1, flags_arith(), FL); }
        { uint32_t f2;
          __asm__ volatile("negl %0; pushfl; popl %1" : "=&r"(r), "=&r"(f2) : "0"(a) : "cc");
          LF_SET(LF_NEG * 4 + 2, 0, a, 0 - a, 0);
          CHECK("neg", 32, a, 0, 0, r, f2, 0 - a, flags_arith(), FL); }
    }
}

static void test_shifts(void) {
    for (int it = 0; it < 200000; it++) {
        uint32_t a = rnd32(), b = rnd32(), n = rnd32() & 31, cin = rnd32() & 1, r, f;
        if (!n) n = 1;
#define SH32(nm, ins, kind, emu, mask)                                             \
        { __asm__ volatile(ins " %%cl, %0; pushfl; popl %1" : "=&r"(r), "=&r"(f) : "0"(a), "c"(n) : "cc"); \
          uint32_t er = emu; LF_SET(kind * 4 + 2, a, n, er, 0);                    \
          CHECK(nm, 32, a, n, 0, r, f, er, flags_arith(), mask); }
        SH32("shl", "shll", LF_SHL, a << n, n == 1 ? FL & ~0x10u : 0x0c5u);
        SH32("shr", "shrl", LF_SHR, a >> n, n == 1 ? FL & ~0x10u : 0x0c5u);
        SH32("sar", "sarl", LF_SAR, (uint32_t)((int32_t)a >> n), n == 1 ? FL & ~0x10u : 0x0c5u);
        /* 16-bit sar/shl with count up to 31 */
        { uint16_t a16 = (uint16_t)a; uint32_t r16, f16, nn = n;
          __asm__ volatile("sarw %%cl, %w0; pushfl; popl %1" : "=&r"(r16), "=&r"(f16) : "0"((uint32_t)a16), "c"(nn) : "cc");
          uint16_t er = (uint16_t)((int16_t)a16 >> (nn >= 16 ? 15 : nn));
          LF_SET(LF_SAR * 4 + 1, a16, nn, er, 0);
          CHECK("sar16", 16, a16, nn, 0, (uint16_t)r16, f16, er, flags_arith(), nn == 1 ? FL & ~0x10u : 0x0c5u); }
        /* rotates (CF only; OF for count 1) */
        { __asm__ volatile("roll %%cl, %0; pushfl; popl %1" : "=&r"(r), "=&r"(f) : "0"(a), "c"(n) : "cc");
          rt_set_arith_flags(0); uint32_t er = rt_rol32(a, n);
          CHECK("rol", 32, a, n, 0, r, f, er, flags_arith(), n == 1 ? 0x801u : 0x001u); }
        { __asm__ volatile("rorl %%cl, %0; pushfl; popl %1" : "=&r"(r), "=&r"(f) : "0"(a), "c"(n) : "cc");
          rt_set_arith_flags(0); uint32_t er = rt_ror32(a, n);
          CHECK("ror", 32, a, n, 0, r, f, er, flags_arith(), n == 1 ? 0x801u : 0x001u); }
        { __asm__ volatile("pushfl; popl %%eax; andl $~1, %%eax; orl %4, %%eax; pushl %%eax; popfl;"
                           "rcll %%cl, %0; pushfl; popl %1" : "=&r"(r), "=&r"(f) : "0"(a), "c"(n), "r"(cin) : "eax", "cc");
          rt_set_arith_flags(cin); uint32_t er = rt_rcl32(a, n);
          CHECK("rcl", 32, a, n, cin, r, f, er, flags_arith(), n == 1 ? 0x801u : 0x001u); }
        { __asm__ volatile("pushfl; popl %%eax; andl $~1, %%eax; orl %4, %%eax; pushl %%eax; popfl;"
                           "rcrl %%cl, %0; pushfl; popl %1" : "=&r"(r), "=&r"(f) : "0"(a), "c"(n), "r"(cin) : "eax", "cc");
          rt_set_arith_flags(cin); uint32_t er = rt_rcr32(a, n);
          CHECK("rcr", 32, a, n, cin, r, f, er, flags_arith(), n == 1 ? 0x801u : 0x001u); }
        { __asm__ volatile("shldl %%cl, %2, %0; pushfl; popl %1" : "=&r"(r), "=&r"(f) : "r"(b), "0"(a), "c"(n) : "cc");
          uint32_t er = rt_shld32(a, b, n);
          CHECK("shld", 32, a, b, n, r, f, er, flags_arith(), n == 1 ? 0x8c5u : 0x0c5u); }
        { __asm__ volatile("shrdl %%cl, %2, %0; pushfl; popl %1" : "=&r"(r), "=&r"(f) : "r"(b), "0"(a), "c"(n) : "cc");
          uint32_t er = rt_shrd32(a, b, n);
          CHECK("shrd", 32, a, b, n, r, f, er, flags_arith(), n == 1 ? 0x8c5u : 0x0c5u); }
    }
}

static void test_conditions(void) {
    /* every jcc condition after cmp, against native setcc */
    for (int it = 0; it < 200000; it++) {
        uint32_t a = rnd32(), b = rnd32();
        uint8_t nat[16];
        __asm__ volatile("cmpl %17, %16;"
                         "seto %0; setno %1; setb %2; setae %3; sete %4; setne %5; setbe %6; seta %7;"
                         "sets %8; setns %9; setp %10; setnp %11; setl %12; setge %13; setle %14; setg %15"
                         : "=m"(nat[0]), "=m"(nat[1]), "=m"(nat[2]), "=m"(nat[3]), "=m"(nat[4]), "=m"(nat[5]),
                           "=m"(nat[6]), "=m"(nat[7]), "=m"(nat[8]), "=m"(nat[9]), "=m"(nat[10]), "=m"(nat[11]),
                           "=m"(nat[12]), "=m"(nat[13]), "=m"(nat[14]), "=m"(nat[15])
                         : "r"(a), "r"(b) : "cc");
        LF_SET(LF_SUB * 4 + 2, a, b, a - b, 0);
        int emu[16] = {CC_O(), CC_NO(), CC_B(), CC_AE(), CC_E(), CC_NE(), CC_BE(), CC_A(),
                       CC_S(), CC_NS(), CC_P(), CC_NP(), CC_L(), CC_GE(), CC_LE(), CC_G()};
        for (int c = 0; c < 16; c++)
            if (!!emu[c] != nat[c] && fails++ < 40)
                printf("cc%-2d a=%08x b=%08x native=%d emu=%d\n", c, a, b, nat[c], emu[c]);
    }
}

static void test_fpu_conv(void) {
    /* fistp word/dword with round-to-nearest, against native */
    fpu_cw = 0x1272;
    for (int it = 0; it < 200000; it++) {
        double v = ((int32_t)rnd32()) / 7.0;
        if (it & 1) v = ldexp(v, -12);
        int16_t n16; int32_t n32;
        uint16_t cw = 0x127f;   /* same rounding as TFX (1272h) but exceptions masked */
        __asm__ volatile("fldcw %2; fldl %3; fistps %0; fldl %3; fistpl %1" : "=m"(n16), "=m"(n32) : "m"(cw), "m"(v));
        int16_t e16 = fpu_to_i16(v); int32_t e32 = fpu_to_i32(v);
        if ((n16 != e16 || n32 != e32) && fails++ < 40)
            printf("fist v=%.6f native %d/%d emu %d/%d\n", v, n16, n32, e16, e32);
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_alu(); printf("alu done, fails=%d\n", fails);
    test_shifts(); printf("shifts done, fails=%d\n", fails);
    test_conditions(); printf("conditions done, fails=%d\n", fails);
    test_fpu_conv(); printf("fpu conversions done, fails=%d\n", fails);
    return fails != 0;
}
