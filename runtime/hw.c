/* PC hardware: port dispatch, PIC, PIT timer, keyboard controller, IRQ delivery. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "rt.h"

/* ---- host clock ------------------------------------------------------------- */
static LARGE_INTEGER qpf, qp0;
uint64_t host_ticks_us(void) {
    LARGE_INTEGER n; QueryPerformanceCounter(&n);
    return (uint64_t)((n.QuadPart - qp0.QuadPart) * 1000000.0 / qpf.QuadPart);
}

/* ---- PIC ---------------------------------------------------------------------- */
static uint8_t pic_mask[2] = {0xb8, 0xff};
static uint8_t pic_pending;         /* IRQ0-7 requested */
static int pic_init_step[2];

/* ---- PIT ---------------------------------------------------------------------- */
static uint32_t pit_div = 65536;
static int pit_latch_lo = -1;
static uint8_t pit_mode_byte;
static uint64_t pit_last_us;
static double pit_accum;            /* fractional ticks */
static uint16_t pit_read_latch;
static int pit_read_hi;

void pit_out(uint16_t port, uint8_t v) {
    if (port == 0x43) {
        pit_mode_byte = v;
        if ((v >> 6) == 0) {
            if (((v >> 4) & 3) == 0) {   /* latch counter 0 */
                double us_per_tick = 1e6 / 1193182.0;
                uint64_t now = host_ticks_us();
                uint32_t elapsed = (uint32_t)((now - pit_last_us) / us_per_tick);
                pit_read_latch = (uint16_t)(pit_div - (elapsed % pit_div));
                pit_read_hi = 0;
            } else {
                pit_latch_lo = -1;
            }
        }
        return;
    }
    if (port == 0x40) {
        if (((pit_mode_byte >> 4) & 3) == 3) {
            if (pit_latch_lo < 0) { pit_latch_lo = v; return; }
            uint32_t d = pit_latch_lo | (v << 8);
            pit_latch_lo = -1;
            pit_div = d ? d : 65536;
        } else {
            pit_div = v ? v : 256;
        }
        rt_log("PIT channel 0 divisor %u (%.2f Hz)", pit_div, 1193182.0 / pit_div);
    }
}

uint8_t pit_in(uint16_t port) {
    if (port == 0x40) {
        uint8_t r = pit_read_hi ? (uint8_t)(pit_read_latch >> 8) : (uint8_t)pit_read_latch;
        pit_read_hi ^= 1;
        return r;
    }
    return 0;
}

/* ---- keyboard controller ----------------------------------------------------- */
static uint8_t kq[256];
static int kq_head, kq_tail;
static uint8_t kbd_data;
static int kbd_irq_waiting;

void kbd_push_scancode(uint8_t sc) {
    int n = (kq_tail + 1) & 255;
    if (n == kq_head) return;
    kq[kq_tail] = sc; kq_tail = n;
    irq_pending = 1;
}

/* responses to commands written to the keyboard (port 60h): polled, no IRQ */
static uint8_t kresp[8];
static int kresp_n;
static int kbd_expect_arg;

uint8_t kbd_read_data(void) {
    if (kresp_n) {
        uint8_t v = kresp[0];
        memmove(kresp, kresp + 1, --kresp_n);
        return v;
    }
    kbd_irq_waiting = 0;
    return kbd_data;
}

static void kbd_command(uint8_t v) {
    if (kresp_n < 8) kresp[kresp_n++] = 0xfa;      /* ACK */
    if (kbd_expect_arg) { kbd_expect_arg = 0; return; }
    switch (v) {
    case 0xed: case 0xf3: kbd_expect_arg = 1; break;   /* LEDs / typematic */
    case 0xee: kresp[kresp_n - 1] = 0xee; break;        /* echo */
    case 0xf2: if (kresp_n < 7) { kresp[kresp_n++] = 0xab; kresp[kresp_n++] = 0x83; } break;
    case 0xff: if (kresp_n < 8) kresp[kresp_n++] = 0xaa; break;
    }
}

/* BIOS keyboard buffer at 40:1E, head 40:1A, tail 40:1C */
static const uint8_t sc_ascii[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 8, 9,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 13, 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0};
static const uint8_t sc_shift[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 8, 9,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 13, 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0};
static int shift_down, ctrl_down, alt_down, e0_prefix;

void kbd_bios_irq(void) {
    uint8_t sc = kbd_data;
    kbd_irq_waiting = 0;
    if (sc == 0xe0) { e0_prefix = 1; return; }
    int up = sc & 0x80; uint8_t k = sc & 0x7f;
    int ext = e0_prefix; e0_prefix = 0;
    if (k == 0x2a || k == 0x36) { shift_down = !up; goto flags; }
    if (k == 0x1d) { ctrl_down = !up; goto flags; }
    if (k == 0x38) { alt_down = !up; goto flags; }
    if (up) return;
    uint8_t ch = (shift_down ? sc_shift : sc_ascii)[k];
    if (ext) ch = (k == 0x1c) ? 13 : (k == 0x35 ? '/' : 0xe0);
    if (ctrl_down && ch >= 'a' && ch <= 'z') ch = ch - 'a' + 1;
    if (alt_down) ch = 0;
    {
        uint16_t head = RW(BDA_BASE + 0x1a), tail = RW(BDA_BASE + 0x1c);
        uint16_t nt = tail + 2; if (nt >= 0x3e) nt = 0x1e;
        if (nt != head) { WW(BDA_BASE + tail, (uint16_t)((k << 8) | ch)); WW(BDA_BASE + 0x1c, nt); }
    }
flags:
    WB(BDA_BASE + 0x17, (shift_down ? 3 : 0) | (ctrl_down ? 4 : 0) | (alt_down ? 8 : 0));
}

int kbd_bios_get(int remove, uint16_t *key) {
    uint16_t head = RW(BDA_BASE + 0x1a), tail = RW(BDA_BASE + 0x1c);
    if (head == tail) return 0;
    *key = RW(BDA_BASE + head);
    if (remove) { head += 2; if (head >= 0x3e) head = 0x1e; WW(BDA_BASE + 0x1a, head); }
    return 1;
}

/* ---- port dispatch --------------------------------------------------------------- */
uint8_t sb_in(uint16_t port); void sb_out(uint16_t port, uint8_t v);
int dma_port(uint16_t p); uint8_t dma_in(uint16_t p); void dma_out(uint16_t p, uint8_t v);
void sb_service(void); void sb_init(void); int sb_port(uint16_t p);
int mpu_port(uint16_t p); uint8_t mpu_in(uint16_t p); void mpu_out(uint16_t p, uint8_t v); void mpu_shutdown(void);
int opl_port(uint16_t p); uint8_t opl_in(uint16_t p); void opl_out(uint16_t p, uint8_t v);

uint8_t rt_inb(uint16_t port) {
    if (port >= 0x3b0 && port <= 0x3df) return vga_in(port);
    if (sb_port(port)) return sb_in(port);
    if (opl_port(port)) return opl_in(port);
    if (mpu_port(port)) return mpu_in(port);
    if (dma_port(port) && port != 0x80) return dma_in(port);
    switch (port) {
    case 0x20: return 0;
    case 0x21: return pic_mask[0];
    case 0xa1: return pic_mask[1];
    case 0x40: case 0x41: case 0x42: return pit_in(port);
    case 0x60: return kbd_read_data();
    case 0x61: return 0x20;
    case 0x64: return (kresp_n || kbd_irq_waiting) ? 0x1d : 0x1c;
    case 0x201: return joy_port_read();     /* game port (joy.c); F0h = no stick */
    }
    static int warned[0x10000];
    if (!warned[port]++) rt_log("in from unhandled port %04x", port);
    return 0xff;
}

uint16_t rt_inw(uint16_t port) { return rt_inb(port) | (rt_inb(port + 1) << 8); }
uint32_t rt_ind(uint16_t port) { return rt_inw(port) | ((uint32_t)rt_inw(port + 2) << 16); }

void rt_outb(uint16_t port, uint8_t v) {
    if (port >= 0x3b0 && port <= 0x3df) { vga_out(port, v); return; }
    if (sb_port(port)) { sb_out(port, v); return; }
    if (opl_port(port)) { opl_out(port, v); return; }
    if (mpu_port(port)) { mpu_out(port, v); return; }
    if (dma_port(port) && port != 0x80) { dma_out(port, v); return; }
    switch (port) {
    case 0x20: case 0xa0:
        if (v & 0x10) pic_init_step[port == 0xa0] = 1;
        return;                    /* EOI */
    case 0x21: case 0xa1: {
        int p = port == 0xa1;
        if (pic_init_step[p]) { if (++pic_init_step[p] > 3) pic_init_step[p] = 0; return; }
        pic_mask[p] = v;
        if (getenv("TFX_TRACE_SB")) rt_log("pic: mask %d = %02x", p, v);
        return;
    }
    case 0x40: case 0x41: case 0x42: case 0x43: pit_out(port, v); return;
    case 0x61: return;
    case 0x60: kbd_command(v); return;
    case 0x64: return;
    case 0x80: return;
    case 0x201: joy_port_write(); return;   /* starts the axis timers */
    }
    static int warned[0x10000];
    if (!warned[port]++) rt_log("out to unhandled port %04x = %02x", port, v);
}

void rt_outw(uint16_t port, uint16_t v) {
    if (port >= 0x3b0 && port <= 0x3df) { vga_out(port, (uint8_t)v); vga_out(port + 1, (uint8_t)(v >> 8)); return; }
    rt_outb(port, (uint8_t)v); rt_outb(port + 1, (uint8_t)(v >> 8));
}
void rt_outd(uint16_t port, uint32_t v) { rt_outw(port, (uint16_t)v); rt_outw(port + 2, (uint16_t)(v >> 16)); }

void rt_cli(void) { if_flag = 0; }
void rt_sti(void) { if_flag = 1; irq_pending = 1; }

/* ---- IRQ delivery ------------------------------------------------------------------ */
static int servicing;
static uint64_t last_present_us;

void irq_raise(int irq) { pic_pending |= (uint8_t)(1 << irq); irq_pending = 1; }

static void deliver(int irq) {
    pic_pending &= (uint8_t)~(1 << irq);
    if (irq >= 2 && getenv("TFX_TRACE_SB")) rt_log("pic: deliver IRQ %d", irq);
    call_guest_int(8 + irq);
}

void timer_service(void) {
    uint64_t now = host_ticks_us();
    double ticks = (now - pit_last_us) * (1193182.0 / 1e6) / pit_div + pit_accum;
    pit_last_us = now;
    int n = (int)ticks;
    pit_accum = ticks - n;
    if (n > 4) n = 4;              /* don't spiral if the program fell behind */
    if (n > 0) irq_raise(0);
    if (n > 1) pit_accum += n - 1; /* keep remaining ticks for subsequent services */
    if (pit_accum > 8) pit_accum = 8;
}

void hw_init(void) {
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&qp0);
    pit_last_us = 0;
    WW(BDA_BASE + 0x1a, 0x1e); WW(BDA_BASE + 0x1c, 0x1e);
    WW(BDA_BASE + 0x80, 0x1e); WW(BDA_BASE + 0x82, 0x3e);
    WB(BDA_BASE + 0x49, 3);
    WW(BDA_BASE + 0x4a, 80);
    WW(BDA_BASE + 0x63, 0x3d4);
    sb_init();
    atexit(mpu_shutdown);
}

/* Called from generated code at loop back-edges when irq_pending is set. */
/* Busy-wait relief.  The game spins on the BIOS tick count (frame limiter)
 * and on int 33h polls; once the polled value has stayed the same for 256
 * consecutive polls, sleep 1 ms (timeBeginPeriod(1) is set at start-up).
 * A program that polls once per frame never reaches the threshold. */
void rt_host_sleep_ms(int ms);
void rt_idle_hint(void) {
    static uint32_t last; static unsigned same;
    uint32_t t = RD(0x46c);
    if (t != last) { last = t; same = 0; return; }
    if (++same >= 256) { same = 0; rt_host_sleep_ms(1); }
}

void rt_service(void) {
    irq_pending = 0;
    if (servicing) return;
    servicing = 1;
    uint64_t now = host_ticks_us();
    /* present as soon as the game flips a page (else every 14 ms): a frame
     * waiting for the next 14 ms tick reached the screen 0-14 ms late, so
     * frames at 18 or 37 ms intervals were shown unevenly (DEBUG_LOG 107).
     * At most every 4 ms between new frames (screens that flip without a
     * frame limit), except with DLSS Frame Generation, which paces its
     * frames on the game's */
    extern int d3d_fg_active(void), video_flip_pending(void);
    static int tick_only = -1;                     /* TFX_PRESENT_TICK=1: the old 14 ms tick (comparison) */
    if (tick_only < 0) tick_only = getenv("TFX_PRESENT_TICK") != NULL;
    static uint64_t last_flip_present_us;          /* the 4 ms cap counts from the last new frame only */
    int flip = video_flip_pending();
    if (now - last_present_us >= 14000 ||
        (flip && (d3d_fg_active() || (!tick_only && now - last_flip_present_us >= 4000)))) {
        if (flip) last_flip_present_us = now;
        last_present_us = now;
        video_pump();
        video_present();
    }
    timer_service();
    sb_service();
    if (kq_head != kq_tail && !kbd_irq_waiting) {
        kbd_data = kq[kq_head]; kq_head = (kq_head + 1) & 255;
        kbd_irq_waiting = 1;
        irq_raise(1);
    }
    if (if_flag) {
        for (int irq = 0; irq < 8; irq++) {
            if ((pic_pending & (1 << irq)) && !(pic_mask[0] & (1 << irq))) {
                deliver(irq);
            }
        }
    }
    /* keyboard byte consumed by default handler if no one read it */
    if (kbd_irq_waiting && (pic_mask[0] & 2)) kbd_irq_waiting = 0;
    servicing = 0;
}
