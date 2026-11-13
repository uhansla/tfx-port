/* DPMI host emulation: descriptors, interrupt vectors, int dispatch. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

struct descr ldt[NSEL];
struct pm_vec pm_vectors[256];
/* real-mode vectors live in the guest's vector table at linear 0 (F000:n*4 =
 * BIOS default, see rm86.c), so real-mode code can hook and chain them */
void rm86_int(int n, uint16_t *regs8, uint16_t *segs4, uint16_t *flags);
void rm86_far_call(uint16_t cs_, uint16_t ip_, uint16_t *regs8, uint16_t *segs4, const uint16_t *args, int nargs);
int rm86_hooked(int n);
static struct pm_vec exc_handlers[32];

#define SET_AX(v) (eax = (eax & 0xffff0000u) | (uint16_t)(v))
#define AX ((uint16_t)eax)
#define BX ((uint16_t)ebx)
#define CX ((uint16_t)ecx)
#define DX ((uint16_t)edx)
#define SI ((uint16_t)esi)
#define DI ((uint16_t)edi)

static void set_cf(int c) { rt_set_cf(c ? 1 : 0); }

uint16_t sel_alloc(uint32_t base, uint32_t limit) {
    for (int i = SEL_FIRST_DYN >> 3; i < NSEL; i++) {
        if (!ldt[i].used) {
            ldt[i].used = 1; ldt[i].base = base; ldt[i].limit = limit; ldt[i].is32 = 1;
            return (uint16_t)(i << 3);
        }
    }
    rt_fatal("out of selectors");
}

void sel_free(uint16_t sel) {
    int i = sel >> 3;
    if (i >= SEL_FIRST_DYN >> 3 && i < NSEL) ldt[i].used = 0;
}

uint32_t sel_base(uint16_t sel) {
    int i = sel >> 3;
    if (i == 0) return 0;
    if (i >= NSEL || !ldt[i].used) rt_fatal("load of invalid selector 0x%04x", sel);
    return ldt[i].base;
}

static void sel_fixed(uint16_t sel, uint32_t base, uint32_t limit, int code) {
    int i = sel >> 3;
    ldt[i].used = 1; ldt[i].base = base; ldt[i].limit = limit; ldt[i].is32 = 1; ldt[i].code = code;
}

void dpmi_init(void) {
    sel_fixed(SEL_FLAT_CS, 0, 0xffffffffu, 1);
    sel_fixed(SEL_FLAT_DS, 0, 0xffffffffu, 0);
    sel_fixed(SEL_PSP, PSP_SEG << 4, 0xff, 0);
    sel_fixed(SEL_ENV, ENV_SEG << 4, 0x7ff, 0);
    sel_fixed(SEL_BIOS, 0x400, 0xff, 0);
    sel_fixed(SEL_VGA, 0xA0000, 0xffff, 0);
    for (int i = 0; i < 256; i++) {
        pm_vectors[i].sel = SEL_FLAT_CS;
        pm_vectors[i].off = MAGIC_INT(i);
        WW((uint32_t)i * 4, (uint16_t)(i * 4)); WW((uint32_t)i * 4 + 2, 0xF000);
    }
}

void rt_load_seg(int seg, uint16_t sel) {
    uint32_t base = sel_base(sel);
    switch (seg) {
    case 0: es = sel; es_base = base; break;
    case 1: cs = sel; cs_base = base; break;
    case 2: ss = sel; ss_base = base; break;
    case 3: ds = sel; ds_base = base; break;
    case 4: fs = sel; fs_base = base; break;
    case 5: gs = sel; gs_base = base; break;
    }
}

/* ---- calling guest interrupt handlers -------------------------------------- */
static int in_handler_depth;

/* Invoke the handler installed for vector vec as if by an interrupt. */
void call_guest_int(int vec) {
    struct pm_vec v = pm_vectors[vec];
    if (v.off >= MAGIC_BASE) {
        rt_magic_call(v.off);
        return;
    }
    uint32_t saved_if = if_flag;
    PUSH32(rt_get_eflags());
    PUSH32(cs);
    PUSH32(MAGIC_RET);
    if_flag = 0;
    in_handler_depth++;
    rt_call(v.off, MAGIC_RET);
    in_handler_depth--;
    (void)saved_if;
}

void rt_iretd(void) {
    uint32_t eip = POP32();
    uint32_t c = POP32();
    uint32_t fl = POP32();
    (void)c;
    if (eip != MAGIC_RET) {
        /* iret to somewhere other than our injected frame */
        rt_set_eflags(fl);
        rt_fatal("iretd to unexpected address 0x%08x", eip);
    }
    rt_set_eflags(fl);
}

void rt_retf(uint32_t n, uint32_t ret) {
    uint32_t eip = POP32();
    uint32_t c = POP32();
    esp += n;
    (void)c;
    /* retf used as an indirect far jump (push sel; push off; retf) */
    if (eip == MAGIC_RET) return;
    rt_call(eip, ret);
}

/* BIOS timer tick (int 8 default; also where a real-mode hook chains to) */
void rt_bios_tick(void) {
    uint32_t t = RD(BDA_BASE + 0x6c) + 1;
    if (t >= 0x1800b0) { t = 0; WB(BDA_BASE + 0x70, 1); }
    WD(BDA_BASE + 0x6c, t);
    if (pm_vectors[0x1c].off < MAGIC_BASE) call_guest_int(0x1c);
}

/* Default (pre-installed) handler behaviour for vector n. */
int rt_magic_call(uint32_t target) {
    if (target < MAGIC_BASE) return 0;
    if (target == MAGIC_RET) return 1;
    uint32_t n = target - MAGIC_BASE;
    if (n < 256) {
        /* called via a far call / jmp chain: emulate the BIOS/DOS default,
         * then consume the interrupt frame the guest pushed (eflags, cs, eip) */
        switch (n) {
        case 0x08:                   /* timer: real-mode hook (music driver) or BIOS tick */
            if (rm86_hooked(8)) {
                static unsigned n8;
                if (getenv("TFX_TRACE_MUSIC") && !(n8++ % 500)) rt_log("music: real-mode timer interrupt #%u", n8);
                rm86_int(8, NULL, NULL, NULL);
            }
            else rt_bios_tick();
            break;
        case 0x09: {                 /* BIOS keyboard: translate to buffer */
            extern void kbd_bios_irq(void);
            kbd_bios_irq();
            break;
        }
        case 0x1c: break;
        default:
            if (n >= 0x08 && n <= 0x0f) break;     /* other IRQs: ignore */
            if (n >= 0x70 && n <= 0x77) break;
            rt_int(n);
            break;
        }
        return 1;
    }
    rt_fatal("call to unknown magic address 0x%08x", target);
}

/* far call/jmp into a magic "old handler" from guest code:
 * generated code reaches here via rt_call(target) after pushing a return
 * address; the iret frame (if any) is consumed by the handler's caller. */

/* ---- int dispatch ------------------------------------------------------------ */
void rt_int(uint32_t n) {
    n &= 0xff;
    {
        static int trace = -1;
        if (trace < 0) trace = getenv("TFX_TRACE_INT") != NULL;
        if (trace)
            rt_log("int %02x eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x @%08x t=%llu",
                   n, eax, ebx, ecx, edx, esi, edi, rt_eip, (unsigned long long)host_ticks_us() / 1000);
    }
    /* a protected-mode handler installed by the program takes precedence for
     * software interrupts it hooks (except the ones we implement as services) */
    if (pm_vectors[n].off < MAGIC_BASE && n != 0x21 && n != 0x31 && n != 0x10 && n != 0x33 &&
        n != 0x2f && n != 0x16 && n != 0x1a) {
        call_guest_int(n);
        return;
    }
    switch (n) {
    case 0x10: bios_int10(); break;
    case 0x16: bios_int16(); break;
    case 0x1a: bios_int1a(); break;
    case 0x21: dos_int21(); break;
    case 0x31: dpmi_int31(); break;
    case 0x33: mouse_int33(); break;
    case 0x2f: mux_int2f(); break;
    case 0x08: rt_magic_call(MAGIC_INT(8)); break;
    case 0x00: rt_fatal("int 0 (divide error) raised by program");
    default:
        rt_dump_regs();
        rt_fatal("unhandled software interrupt 0x%02x (ax=%04x)", n, AX);
    }
}

/* ---- real-mode interrupt simulation (DPMI 0300h) ---------------------------- */
static void simulate_rm_int(int vec, uint32_t rmregs) {
    /* rmregs: 50-byte DPMI real-mode call structure */
    uint32_t r_edi = RD(rmregs + 0x00), r_esi = RD(rmregs + 0x04), r_ebp = RD(rmregs + 0x08);
    uint32_t r_ebx = RD(rmregs + 0x10), r_edx = RD(rmregs + 0x14), r_ecx = RD(rmregs + 0x18);
    uint32_t r_eax = RD(rmregs + 0x1c);
    uint16_t r_es = RW(rmregs + 0x22), r_ds = RW(rmregs + 0x24);
    if (!(vec == 0x2f && ((uint16_t)r_eax >> 8) == 0x15) || getenv("TFX_TRACE_INT"))
        rt_log("real-mode int %02x ax=%04x bx=%04x cx=%04x dx=%04x es=%04x ds=%04x",
           vec, (uint16_t)r_eax, (uint16_t)r_ebx, (uint16_t)r_ecx, (uint16_t)r_edx, r_es, r_ds);
    (void)r_edi; (void)r_esi; (void)r_ebp;
    if (vec >= 0 && rm86_hooked(vec)) {
        /* real-mode code hooked this vector (START.EXE's music driver): run it */
        uint16_t r[8] = {(uint16_t)r_eax, (uint16_t)r_ecx, (uint16_t)r_edx, (uint16_t)r_ebx, 0,
                         (uint16_t)r_ebp, (uint16_t)r_esi, (uint16_t)r_edi};
        uint16_t sg[4] = {r_es, 0, 0, r_ds}, fl = (uint16_t)(RW(rmregs + 0x20) | 0x0202);
        rm86_int(vec, r, sg, &fl);
        if (getenv("TFX_TRACE_MUSIC")) rt_log("music: int %02x -> ax=%04x bx=%04x dx=%04x fl=%04x (int 08h %04x:%04x)", vec, r[0], r[3], r[2], fl, RW(0x22), RW(0x20));
        WW(rmregs + 0x1c, r[0]); WW(rmregs + 0x18, r[1]); WW(rmregs + 0x14, r[2]); WW(rmregs + 0x10, r[3]);
        WW(rmregs + 0x08, r[5]); WW(rmregs + 0x04, r[6]); WW(rmregs + 0x00, r[7]);
        WW(rmregs + 0x22, sg[0]); WW(rmregs + 0x24, sg[3]); WW(rmregs + 0x20, fl);
        return;
    }
    if (vec == 0x2f && mscdex_int2f(rmregs)) {
        WW(rmregs + 0x20, (uint16_t)(RW(rmregs + 0x20) & ~1));
        return;
    }
    /* nothing real-mode is resident: report failure with CF set */
    WW(rmregs + 0x20, (RW(rmregs + 0x20) | 1));
}

/* ---- int 31h ------------------------------------------------------------------ */
void dpmi_int31(void) {
    uint16_t ax = AX;
    set_cf(0);
    switch (ax) {
    case 0x0000: {                  /* allocate LDT descriptors */
        uint16_t first = 0;
        for (int i = 0; i < CX; i++) {
            uint16_t s = sel_alloc(0, 0);
            if (i == 0) first = s;
        }
        SET_AX(first);
        break;
    }
    case 0x0001: sel_free(BX); break;
    case 0x0002: {                  /* segment to descriptor */
        uint32_t base = (uint32_t)BX << 4;
        for (int i = SEL_FIRST_DYN >> 3; i < NSEL; i++)
            if (ldt[i].used && ldt[i].base == base && ldt[i].limit == 0xffff) { SET_AX(i << 3); return; }
        SET_AX(sel_alloc(base, 0xffff));
        break;
    }
    case 0x0003: SET_AX(8); break;   /* selector increment */
    case 0x0006: {                  /* get segment base */
        uint32_t b = sel_base(BX);
        ecx = (ecx & 0xffff0000u) | (b >> 16);
        edx = (edx & 0xffff0000u) | (b & 0xffff);
        break;
    }
    case 0x0007: {                  /* set segment base */
        int i = BX >> 3;
        if (i >= NSEL || !ldt[i].used) { set_cf(1); break; }
        ldt[i].base = ((uint32_t)CX << 16) | DX;
        /* refresh cached bases */
        if (ds == BX) ds_base = ldt[i].base;
        if (es == BX) es_base = ldt[i].base;
        if (fs == BX) fs_base = ldt[i].base;
        if (gs == BX) gs_base = ldt[i].base;
        break;
    }
    case 0x0008: {                  /* set segment limit */
        int i = BX >> 3;
        if (i >= NSEL || !ldt[i].used) { set_cf(1); break; }
        ldt[i].limit = ((uint32_t)CX << 16) | DX;
        break;
    }
    case 0x0009: break;             /* set access rights */
    case 0x000A: {                  /* create alias */
        int i = BX >> 3;
        SET_AX(sel_alloc(ldt[i].base, ldt[i].limit));
        break;
    }
    case 0x000C: break;             /* set descriptor (ignore) */
    case 0x0100: {                  /* allocate DOS memory: BX paragraphs */
        extern int dos_alloc(uint16_t paras, uint16_t *seg, uint16_t *maxavail);
        uint16_t seg, maxp;
        if (!dos_alloc(BX, &seg, &maxp)) { SET_AX(8); ebx = (ebx & 0xffff0000u) | maxp; set_cf(1); break; }
        SET_AX(seg);
        edx = (edx & 0xffff0000u) | sel_alloc((uint32_t)seg << 4, (uint32_t)BX * 16 - 1);
        break;
    }
    case 0x0101: {
        extern void dos_free(uint16_t seg);
        dos_free((uint16_t)(sel_base(DX) >> 4));
        sel_free(DX);
        break;
    }
    case 0x0200:                    /* get real-mode vector */
        ecx = (ecx & 0xffff0000u) | RW((uint32_t)(uint8_t)ebx * 4 + 2);
        edx = (edx & 0xffff0000u) | RW((uint32_t)(uint8_t)ebx * 4);
        break;
    case 0x0201:
        WW((uint32_t)(uint8_t)ebx * 4, DX); WW((uint32_t)(uint8_t)ebx * 4 + 2, CX);
        break;
    case 0x0202:
        ecx = (ecx & 0xffff0000u) | exc_handlers[(uint8_t)ebx & 31].sel;
        edx = exc_handlers[(uint8_t)ebx & 31].off;
        break;
    case 0x0203:
        exc_handlers[(uint8_t)ebx & 31].sel = CX;
        exc_handlers[(uint8_t)ebx & 31].off = edx;
        break;
    case 0x0204:                    /* get PM vector */
        ecx = (ecx & 0xffff0000u) | pm_vectors[(uint8_t)ebx].sel;
        edx = pm_vectors[(uint8_t)ebx].off;
        break;
    case 0x0205:
        pm_vectors[(uint8_t)ebx].sel = CX;
        pm_vectors[(uint8_t)ebx].off = edx;
        rt_log("DPMI set PM vector %02x -> %04x:%08x", (uint8_t)ebx, CX, edx);
        break;
    case 0x0300: case 0x0302:
        simulate_rm_int((uint8_t)ebx, es_base + edi);
        break;
    case 0x0301: {                  /* call real-mode procedure (far return) */
        uint32_t rr = es_base + edi;
        uint16_t r[8] = {RW(rr + 0x1c), RW(rr + 0x18), RW(rr + 0x14), RW(rr + 0x10), 0, RW(rr + 0x08), RW(rr + 0x04), RW(rr + 0x00)};
        uint16_t sg[4] = {RW(rr + 0x22), 0, 0, RW(rr + 0x24)};
        rm86_far_call(RW(rr + 0x2c), RW(rr + 0x2a), r, sg, NULL, 0);
        WW(rr + 0x1c, r[0]); WW(rr + 0x18, r[1]); WW(rr + 0x14, r[2]); WW(rr + 0x10, r[3]);
        WW(rr + 0x08, r[5]); WW(rr + 0x04, r[6]); WW(rr + 0x00, r[7]);
        break;
    }
    case 0x0303: {                  /* allocate real-mode callback */
        static uint16_t next_cb = 0;
        rt_log("DPMI alloc RM callback -> %04x:%08x (regs at %04x:%08x)", ds, esi, es, edi);
        ecx = (ecx & 0xffff0000u) | 0xF100;
        edx = (edx & 0xffff0000u) | (next_cb += 0x10);
        break;
    }
    case 0x0304: break;
    case 0x0305: SET_AX(0); ebx &= 0xffff0000u; ecx &= 0xffff0000u; esi = 0; edi = 0; break;
    case 0x0306: break;
    case 0x0400:                    /* version */
        SET_AX(0x005a);             /* 0.90 */
        ebx = (ebx & 0xffff0000u) | 0x0005;
        ecx = (ecx & 0xffff0000u) | 0x0003;   /* 386 */
        edx = (edx & 0xffff0000u) | 0x0870;   /* PIC bases */
        break;
    case 0x0500: {                  /* free memory info */
        uint32_t b = es_base + edi;
        for (int i = 0; i < 0x30; i += 4) WD(b + i, 0xffffffffu);
        WD(b + 0, MEM_SIZE - HEAP_LO);
        WD(b + 4, (MEM_SIZE - HEAP_LO) / 4096);
        WD(b + 8, (MEM_SIZE - HEAP_LO) / 4096);
        break;
    }
    case 0x0501: {                  /* allocate memory block BX:CX */
        uint32_t size = ((uint32_t)BX << 16) | CX;
        uint32_t a = heap_alloc(size);
        if (!a) { set_cf(1); SET_AX(0x8013); break; }
        ebx = (ebx & 0xffff0000u) | (a >> 16);
        ecx = (ecx & 0xffff0000u) | (a & 0xffff);
        esi = (esi & 0xffff0000u) | (a >> 16);
        edi = (edi & 0xffff0000u) | (a & 0xffff);
        break;
    }
    case 0x0502: heap_free(((uint32_t)SI << 16) | DI); break;
    case 0x0503: {
        uint32_t a = heap_resize(((uint32_t)SI << 16) | DI, ((uint32_t)BX << 16) | CX);
        if (!a) { set_cf(1); break; }
        ebx = (ebx & 0xffff0000u) | (a >> 16); ecx = (ecx & 0xffff0000u) | (a & 0xffff);
        esi = (esi & 0xffff0000u) | (a >> 16); edi = (edi & 0xffff0000u) | (a & 0xffff);
        break;
    }
    case 0x0600: case 0x0601: case 0x0602: case 0x0603: break;   /* lock/unlock */
    case 0x0604: ebx &= 0xffff0000u; ecx = (ecx & 0xffff0000u) | 4096; break;
    case 0x0800: {                  /* physical address mapping: identity */
        uint32_t phys = ((uint32_t)BX << 16) | CX;
        ebx = (ebx & 0xffff0000u) | (phys >> 16);
        ecx = (ecx & 0xffff0000u) | (phys & 0xffff);
        break;
    }
    case 0x0900: { int old = if_flag; if_flag = 0; eax = (eax & 0xffffff00u) | old; break; }
    case 0x0901: { int old = if_flag; if_flag = 1; irq_pending = 1; eax = (eax & 0xffffff00u) | old; break; }
    case 0x0902: eax = (eax & 0xffffff00u) | (if_flag ? 1 : 0); break;
    case 0x0A00:                    /* vendor API entry: not supported */
        set_cf(1);
        break;
    default:
        rt_dump_regs();
        rt_fatal("unhandled DPMI function %04x", ax);
    }
}
