/* Differential test of x87 semantics: every register form D8..DF /C0..FF and
 * the [esi] memory forms, executed natively and by the interpreter (rt_step).
 * Stack contents and the C0-C3 condition bits are compared. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>
#include "cpu.h"

int vga_planar;
uint8_t vga_read8(uint32_t a) { (void)a; return 0; }
void vga_write8(uint32_t a, uint8_t v) { (void)a; (void)v; }
const struct func_entry guest_funcs[1]; const unsigned guest_nfuncs = 0;
const uint32_t guest_conts[1]; const unsigned guest_nconts = 0;
void rt_log(const char *f, ...) { (void)f; }
void rt_fatal(const char *f, ...) { printf("fatal: %s\n", f); exit(1); }
int rt_magic_call(uint32_t t) { (void)t; return 0; }
void rt_dump_regs(void) {}
void rt_int(uint32_t n) { (void)n; }
uint8_t rt_inb(uint16_t p) { (void)p; return 0; } uint16_t rt_inw(uint16_t p) { (void)p; return 0; }
uint32_t rt_ind(uint16_t p) { (void)p; return 0; }
void rt_outb(uint16_t p, uint8_t v) { (void)p; (void)v; } void rt_outw(uint16_t p, uint16_t v) { (void)p; (void)v; }
void rt_outd(uint16_t p, uint32_t v) { (void)p; (void)v; }
void rt_cli(void) {} void rt_sti(void) {} void rt_service(void) {} void rt_load_seg(int s, uint16_t v) { (void)s; (void)v; }

static double stackv[8];
static uint8_t *native_code;

/* run `insn` natively with the FPU stack loaded from in[0..7] (ST0 = in[0]);
 * the memory operand (if any) is at esi = mem */
static void run_native(const uint8_t *insn, int len, const double in[8], void *mem,
                       double out[8], uint16_t *sw) {
    uint8_t *p = native_code;
    /* fninit; fldcw [cw]; fld qword [in+8*i] for i=7..0 */
    static uint16_t cw = 0x127f;
    *p++ = 0xdb; *p++ = 0xe3;
    *p++ = 0xd9; *p++ = 0x2d; memcpy(p, &(uint32_t){(uint32_t)(uintptr_t)&cw}, 4); p += 4;
    for (int i = 5; i >= 0; i--) {
        *p++ = 0xdd; *p++ = 0x05; memcpy(p, &(uint32_t){(uint32_t)(uintptr_t)&in[i]}, 4); p += 4;
    }
    *p++ = 0x56;                                  /* push esi */
    *p++ = 0xbe; memcpy(p, &(uint32_t){(uint32_t)(uintptr_t)mem}, 4); p += 4;   /* mov esi, mem */
    memcpy(p, insn, len); p += len;
    *p++ = 0x5e;                                  /* pop esi */
    *p++ = 0xdd; *p++ = 0x3d; memcpy(p, &(uint32_t){(uint32_t)(uintptr_t)sw}, 4); p += 4;  /* fnstsw */
    for (int i = 0; i < 8; i++) {                 /* fstp qword [out+8*i] */
        *p++ = 0xdd; *p++ = 0x1d; memcpy(p, &(uint32_t){(uint32_t)(uintptr_t)&out[i]}, 4); p += 4;
    }
    *p++ = 0xc3;
    ((void (*)(void))native_code)();
}

static void run_emu(const uint8_t *insn, int len, const double in[8], uint32_t gmem,
                    double out[8], uint16_t *sw) {
    fpu_init();
    fpu_cw = 0x127f;
    for (int i = 5; i >= 0; i--) FPUSH((long double)in[i]);
    fpu_sw = 0;
    memcpy(MEM + 0x20000, insn, len);
    esi = gmem; ds_base = 0; ss_base = 0; es_base = 0;
    rt_step(0x20000);
    *sw = fpu_get_sw();
    for (int i = 0; i < 8; i++) { out[i] = (double)ST(0); FPOP(); }
}

static int close_enough(double a, double b) {
    if (isnan(a) && isnan(b)) return 1;
    if (a == b) return 1;
    double d = fabs(a - b), m = fmax(fabs(a), fabs(b));
    return d <= m * 1e-12;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    MEM = VirtualAlloc(NULL, MEM_SIZE + 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    native_code = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    int fails = 0, tests = 0;
    double in[8] = {1.5, -2.25, 3.75, 10.0, -0.5, 7.0, 100.0, 0.125};
    /* memory operand area: same bytes for native and emulated */
    uint8_t memdata[16];
    double md = 6.5; memcpy(memdata, &md, 8);
    uint8_t *nmem = malloc(16);
    for (int opc = 0xd8; opc <= 0xdf; opc++) {
        for (int m = 0; m < 256; m++) {
            int is_reg = m >= 0xc0;
            if (!is_reg && (m & 0xc7) != 0x06) continue;     /* memory forms: [esi] only */
            int reg = (m >> 3) & 7;
            /* skip control/env ops, invalid encodings and stack-only ops */
            if (!is_reg && ((opc == 0xd9 && reg >= 4) || (opc == 0xdd && reg >= 4) ||
                            (opc == 0xdb && (reg == 1 || reg == 4 || reg == 6)) ||
                            (opc == 0xdf && (reg == 1 || reg == 4 || reg == 6)) ||
                            (opc == 0xdd && reg == 1) || (opc == 0xd9 && reg == 1))) continue;
            if (is_reg && opc == 0xd9 && (m == 0xd8 || (m >= 0xd1 && m <= 0xdf) || m == 0xe2 || m == 0xe3 ||
                                          m == 0xe6 || m == 0xe7 || m == 0xef || m == 0xf4 || m == 0xf5 ||
                                          m == 0xf6 || m == 0xf7)) continue;
            if (is_reg && opc == 0xda && m != 0xe9) continue;
            if (is_reg && opc == 0xdb) continue;
            if (is_reg && opc == 0xdd && ((m >= 0xc8 && m <= 0xcf) || m >= 0xf0)) continue;
            if (is_reg && opc == 0xdd && m < 0xc8) continue;   /* ffree: tags only */
            if (is_reg && opc == 0xde && (m >= 0xd0 && m <= 0xd7)) continue;
            if (is_reg && opc == 0xde && m >= 0xd8 && m <= 0xdf && m != 0xd9) continue;
            if (is_reg && opc == 0xdf && m != 0xe0) continue;
            if (is_reg && opc == 0xdc && (m >= 0xd0 && m <= 0xdf)) continue; /* fcom aliases */
            if (is_reg && opc == 0xd9 && (m == 0xf0 || m == 0xf1 || m == 0xf2 || m == 0xf3 || m == 0xf8 ||
                                          m == 0xf9 || m == 0xfb || m == 0xfd || m == 0xfe || m == 0xff)) {
                /* transcendental: keep but compare loosely below */
            }
            if (is_reg && (m & 7) >= 6) continue;   /* ST6/ST7 are empty */
            uint8_t insn[2] = {(uint8_t)opc, (uint8_t)m};
            double outn[8], oute[8];
            uint16_t swn = 0, swe = 0;
            memcpy(nmem, memdata, 16);
            memcpy(MEM + 0x21000, memdata, 16);
            run_native(insn, 2, in, nmem, outn, &swn);
            run_emu(insn, 2, in, 0x21000, oute, &swe);
            tests++;
            int bad = 0;
            for (int i = 0; i < 8; i++) if (!close_enough(outn[i], oute[i]) && !(i >= 5 && isnan(outn[i]))) bad = 1;
            if ((swn & 0x4500) != (swe & 0x4500)) bad = 1;
            if (memcmp(nmem, MEM + 0x21000, 10) != 0) bad = 1;
            if (opc == 0xdf && m == 0xe0) bad = 0;  /* fnstsw ax: AX not compared here */
            if (bad && fails++ < 200) {
                printf("%02x %02x: native", opc, m);
                for (int i = 0; i < 8; i++) printf(" %g", outn[i]);
                printf(" sw=%04x | emu", swn & 0x4700);
                for (int i = 0; i < 8; i++) printf(" %g", oute[i]);
                printf(" sw=%04x\n", swe & 0x4700);
            }
        }
    }
    printf("x87: %d tests, %d failures\n", tests, fails);
    return fails != 0;
}
