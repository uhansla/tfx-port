/* Guest CPU state and primitives used by recompiled code. */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- general registers ------------------------------------------------ */
extern uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
extern uint16_t es, cs, ss, ds, fs, gs;
extern uint32_t es_base, cs_base, ss_base, ds_base, fs_base, gs_base;
extern int df;                      /* direction flag */
extern int if_flag;                 /* interrupt flag */
extern volatile int irq_pending;
extern uint32_t rt_eip;              /* last recorded guest EIP (debug) */    /* set asynchronously: something needs servicing */

/* ---- guest memory ------------------------------------------------------- */
extern uint8_t *MEM;
#define MEM_SIZE   (96u << 20)      /* 96 MB guest address space */

void vga_write8(uint32_t a, uint8_t v);
uint8_t vga_read8(uint32_t a);
extern int vga_planar;              /* 1 when 0xA0000 window is not plain memory */

#define IS_VGA(a) (vga_planar && ((uint32_t)(a) - 0xA0000u) < 0x10000u)

static inline uint8_t RB(uint32_t a) {
    if (IS_VGA(a)) return vga_read8(a);
    return MEM[a];
}
static inline uint16_t RW(uint32_t a) {
    if (IS_VGA(a)) return vga_read8(a) | (vga_read8(a + 1) << 8);
    uint16_t v; memcpy(&v, MEM + a, 2); return v;
}
static inline uint32_t RD(uint32_t a) {
    if (IS_VGA(a)) return RW(a) | ((uint32_t)RW(a + 2) << 16);
    uint32_t v; memcpy(&v, MEM + a, 4); return v;
}
static inline uint64_t RQ(uint32_t a) {
    return RD(a) | ((uint64_t)RD(a + 4) << 32);
}
static inline void WB(uint32_t a, uint8_t v) {
    if (IS_VGA(a)) { vga_write8(a, v); return; }
    MEM[a] = v;
}
static inline void WW(uint32_t a, uint16_t v) {
    if (IS_VGA(a)) { vga_write8(a, (uint8_t)v); vga_write8(a + 1, (uint8_t)(v >> 8)); return; }
    memcpy(MEM + a, &v, 2);
}
static inline void WD(uint32_t a, uint32_t v) {
    if (IS_VGA(a)) { WW(a, (uint16_t)v); WW(a + 2, (uint16_t)(v >> 16)); return; }
    memcpy(MEM + a, &v, 4);
}
static inline void WQ(uint32_t a, uint64_t v) { WD(a, (uint32_t)v); WD(a + 4, (uint32_t)(v >> 32)); }

static inline float RF32(uint32_t a) { uint32_t v = RD(a); float f; memcpy(&f, &v, 4); return f; }
static inline double RF64(uint32_t a) { uint64_t v = RQ(a); double f; memcpy(&f, &v, 8); return f; }
static inline void WF32(uint32_t a, float f) { uint32_t v; memcpy(&v, &f, 4); WD(a, v); }
static inline void WF64(uint32_t a, double f) { uint64_t v; memcpy(&v, &f, 8); WQ(a, v); }
long double RF80(uint32_t a);
void WF80(uint32_t a, long double v);

/* ---- stack ------------------------------------------------------------- */
#define PUSH32(v) do { uint32_t v__ = (uint32_t)(v); esp -= 4; WD(ss_base + esp, v__); } while (0)
#define PUSH16(v) do { uint16_t v__ = (uint16_t)(v); esp -= 2; WW(ss_base + esp, v__); } while (0)
static inline uint32_t POP32(void) { uint32_t v = RD(ss_base + esp); esp += 4; return v; }
static inline uint16_t POP16(void) { uint16_t v = RW(ss_base + esp); esp += 2; return v; }

/* ---- lazy flags ---------------------------------------------------------
 * lf_op = kind*4 + size (0=8,1=16,2=32).  For kind FLAGS, lf_res holds the
 * explicit arithmetic flags.  lf_cf is the carry-in for ADC/SBB, the
 * preserved CF for INC/DEC and CF=OF for MUL.
 */
enum { LF_ADD, LF_ADC, LF_SUB, LF_SBB, LF_LOGIC, LF_INC, LF_DEC, LF_NEG,
       LF_SHL, LF_SHR, LF_SAR, LF_MUL, LF_FLAGS };
extern uint32_t lf_op, lf_a, lf_b, lf_res, lf_cf;
/* c is evaluated first: for INC/DEC it is CC_B() of the *previous* state */
#define LF_SET(op, a, b, r, c) (lf_cf = (uint32_t)(c), lf_op = (op), lf_a = (uint32_t)(a), \
                                lf_b = (uint32_t)(b), lf_res = (uint32_t)(r))

uint32_t flags_cf(void);
uint32_t flags_zf(void);
uint32_t flags_sf(void);
uint32_t flags_of(void);
uint32_t flags_pf(void);
uint32_t flags_arith(void);         /* OF SF ZF AF PF CF as eflags bits */
void rt_set_arith_flags(uint32_t f);
void rt_set_cf(uint32_t c);
uint32_t rt_get_eflags(void);
void rt_set_eflags(uint32_t f);

#define CC_O()  (flags_of())
#define CC_NO() (!flags_of())
#define CC_B()  (flags_cf())
#define CC_AE() (!flags_cf())
#define CC_E()  (flags_zf())
#define CC_NE() (!flags_zf())
#define CC_BE() (flags_cf() || flags_zf())
#define CC_A()  (!flags_cf() && !flags_zf())
#define CC_S()  (flags_sf())
#define CC_NS() (!flags_sf())
#define CC_P()  (flags_pf())
#define CC_NP() (!flags_pf())
#define CC_L()  (flags_sf() != flags_of())
#define CC_GE() (flags_sf() == flags_of())
#define CC_LE() (flags_zf() || flags_sf() != flags_of())
#define CC_G()  (!flags_zf() && flags_sf() == flags_of())

uint8_t  rt_rol8(uint8_t v, uint32_t n);   uint16_t rt_rol16(uint16_t v, uint32_t n);
uint32_t rt_rol32(uint32_t v, uint32_t n);
uint8_t  rt_ror8(uint8_t v, uint32_t n);   uint16_t rt_ror16(uint16_t v, uint32_t n);
uint32_t rt_ror32(uint32_t v, uint32_t n);
uint8_t  rt_rcl8(uint8_t v, uint32_t n);   uint16_t rt_rcl16(uint16_t v, uint32_t n);
uint32_t rt_rcl32(uint32_t v, uint32_t n);
uint8_t  rt_rcr8(uint8_t v, uint32_t n);   uint16_t rt_rcr16(uint16_t v, uint32_t n);
uint32_t rt_rcr32(uint32_t v, uint32_t n);
uint16_t rt_shld16(uint16_t d, uint16_t s, uint32_t n);
uint32_t rt_shld32(uint32_t d, uint32_t s, uint32_t n);
uint16_t rt_shrd16(uint16_t d, uint16_t s, uint32_t n);
uint32_t rt_shrd32(uint32_t d, uint32_t s, uint32_t n);
void rt_daa(void); void rt_das(void); void rt_aaa(void); void rt_aas(void);

/* ---- x87 ----------------------------------------------------------------- */
extern long double fpr[8];
extern int ftop;
extern uint16_t fpu_sw, fpu_cw;
#define ST(i) fpr[(ftop + (i)) & 7]
#define FPUSH(v) do { long double v__ = (v); ftop = (ftop - 1) & 7; fpr[ftop] = v__; } while (0)
#define FPOP() (ftop = (ftop + 1) & 7)
void fpu_compare(long double a, long double b);
void fpu_xam(void);
void fpu_prem(void);
void fpu_prem1(void);
void fpu_xtract(void);
void fpu_init(void);
uint16_t fpu_get_sw(void);
long double fpu_round(long double v);
int16_t fpu_to_i16(long double v);
int32_t fpu_to_i32(long double v);
int64_t fpu_to_i64(long double v);
void fpu_ldenv(uint32_t a);
void fpu_stenv(uint32_t a);
void fpu_save(uint32_t a);
void fpu_rstor(uint32_t a);

/* ---- runtime services called by generated code ------------------------- */
void rt_load_seg(int seg, uint16_t sel);
void rt_int(uint32_t n);
uint8_t rt_inb(uint16_t port);
uint16_t rt_inw(uint16_t port);
uint32_t rt_ind(uint16_t port);
void rt_outb(uint16_t port, uint8_t v);
void rt_outw(uint16_t port, uint16_t v);
void rt_outd(uint16_t port, uint32_t v);
void rt_cli(void);
void rt_sti(void);
void rt_hlt(uint32_t at);
void rt_call(uint32_t target, uint32_t ret);
void rt_retf(uint32_t n, uint32_t ret);
void rt_iretd(void);
void rt_div0(uint32_t at);
void rt_unimpl(uint32_t at, const char *what);
void rt_fallthrough(uint32_t at);
void rt_trace(uint32_t at);
void rt_service(void);
void rt_idle_hint(void);         /* see hw.c: busy-wait relief */
void scene_prim(uint32_t entry);   /* see scene.c: native renderer capture */
void rt_fix_chase_ease(void);
void rt_joy_frame(void);
void rt_head_track(void);
void rt_div_recovered(void);
int rt_fix_pan_rate(void);      /* see gamefix.c: 1-tick frames (framerate=high) */
int rt_wide_cull(void);            /* see gamefix.c: the object visibility test for wide views */
void rt_step(uint32_t at);
int rt_jcc_live(uint8_t op, uint32_t at);
void rt_interpret_func(uint32_t entry, uint32_t ret);
extern int rt_interp_any;
int rt_interp_hit(uint32_t a);
#define INTERP_CHECK(a, r) do { if (rt_interp_any && rt_interp_hit(a)) { rt_interpret_func((a), (r)); return; } } while (0)
extern uint32_t rt_unwind;
void rt_ret_mismatch(uint32_t target, uint32_t expected);

void rt_rep_movs1(uint32_t src_base);
void rt_rep_movs4(uint32_t src_base);
void rt_rep_stos1(void);
void rt_rep_stos4(void);

extern uint32_t ftrace_ring[4096];
extern unsigned ftrace_pos;
extern uint32_t fcount[1 << 20];   /* per-function call counts, index (addr >> 2) & 0xfffff */
#define FTRACE(a) (ftrace_ring[ftrace_pos++ & 4095] = (a), fcount[((a) >> 2) & 0xfffff]++)
void rt_dump_ftrace(int n);
#ifdef ESP_DEBUG
void rt_esp_mismatch(uint32_t func, uint32_t esp0);
#define ESP_ENTRY() const uint32_t esp0_ = esp
#define ESP_CHECK(f) do { if (esp != esp0_) rt_esp_mismatch(f, esp0_); } while (0)
#else
#define ESP_ENTRY() do {} while (0)
#define ESP_CHECK(f) do {} while (0)
#endif
#define IRQ_CHECK() do { if (irq_pending) rt_service(); } while (0)

typedef void (*guest_fn)(uint32_t ret);
struct func_entry { uint32_t addr; guest_fn fn; };
extern const struct func_entry guest_funcs[];
extern const unsigned guest_nfuncs;

#ifdef __cplusplus
}
#endif
