/* BIOS-level services: video (10h), keyboard (16h), time (1Ah), mouse (33h), mux (2Fh). */
#include <stdio.h>
#include <string.h>
#include "rt.h"

#define SET_AX(v) (eax = (eax & 0xffff0000u) | (uint16_t)(v))
#define SET_AL(v) (eax = (eax & 0xffffff00u) | (uint8_t)(v))
#define SET_AH(v) (eax = (eax & 0xffff00ffu) | ((uint32_t)(uint8_t)(v) << 8))
#define SET_BX(v) (ebx = (ebx & 0xffff0000u) | (uint16_t)(v))
#define SET_CX(v) (ecx = (ecx & 0xffff0000u) | (uint16_t)(v))
#define SET_DX(v) (edx = (edx & 0xffff0000u) | (uint16_t)(v))
#define AX ((uint16_t)eax)
#define AH ((uint8_t)(eax >> 8))
#define AL ((uint8_t)eax)
#define BX ((uint16_t)ebx)
#define BH ((uint8_t)(ebx >> 8))
#define BL ((uint8_t)ebx)
#define CX ((uint16_t)ecx)
#define DX ((uint16_t)edx)

void bios_int10(void) {
    switch (AH) {
    case 0x00:
        vga_set_mode(AL & 0x7f);
        break;
    case 0x01: case 0x02: break;    /* cursor shape / position */
    case 0x03: SET_DX(0); SET_CX(0x0607); break;
    case 0x05: break;
    case 0x06: case 0x07: break;    /* scroll */
    case 0x08: SET_AX(0x0720); break;
    case 0x09: case 0x0a: case 0x0e: break;
    case 0x0b: break;
    case 0x0f:
        SET_AL(vga_get_mode());
        SET_AH(vga_get_mode() == 3 ? 80 : 40);
        ebx &= 0xffff00ffu;
        break;
    case 0x10: {
        extern uint8_t vga_dac[256][3];
        if (AL == 0x10) { vga_dac[BX & 0xff][0] = (uint8_t)(edx >> 8) & 63;
                          vga_dac[BX & 0xff][1] = (uint8_t)(ecx >> 8) & 63;
                          vga_dac[BX & 0xff][2] = (uint8_t)ecx & 63; }
        else if (AL == 0x12) {
            for (int i = 0; i < CX; i++)
                for (int c = 0; c < 3; c++)
                    vga_dac[(BX + i) & 0xff][c] = RB(es_base + edx + i * 3 + c) & 63;
        } else if (AL == 0x17) {
            for (int i = 0; i < CX; i++)
                for (int c = 0; c < 3; c++)
                    WB(es_base + edx + i * 3 + c, vga_dac[(BX + i) & 0xff][c]);
        }
        break;
    }
    case 0x11: break;               /* character generator */
    case 0x12:
        if (BL == 0x10) { SET_BX(0x0003); SET_CX(0x0009); }   /* EGA info: color, 256K */
        break;
    case 0x1a:
        if (AL == 0) { SET_AL(0x1a); SET_BX(0x0008); }        /* VGA with color display */
        break;
    case 0x1b: SET_AL(0); break;
    case 0x4f: SET_AX(0x014f); break;                       /* no VESA */
    default:
        rt_log("unhandled int 10h AX=%04x", AX);
        break;
    }
}

void bios_int16(void) {
    uint16_t key;
    switch (AH) {
    case 0x00: case 0x10:
        while (!kbd_bios_get(1, &key)) { video_pump(); rt_service(); }
        SET_AX(key);
        break;
    case 0x01: case 0x11:
        if (kbd_bios_get(0, &key)) { SET_AX(key); rt_set_arith_flags(0); }
        else rt_set_arith_flags(0x40);
        video_pump();
        break;
    case 0x02: case 0x12: SET_AL(RB(BDA_BASE + 0x17)); break;
    default:
        rt_log("unhandled int 16h AX=%04x", AX);
    }
}

void bios_int1a(void) {
    switch (AH) {
    case 0x00: {
        uint32_t t = RD(BDA_BASE + 0x6c);
        SET_CX(t >> 16); SET_DX(t & 0xffff); SET_AL(RB(BDA_BASE + 0x70)); WB(BDA_BASE + 0x70, 0);
        break;
    }
    default:
        rt_log("unhandled int 1Ah AX=%04x", AX);
    }
}

/* ---- mouse ------------------------------------------------------------------- */
int mouse_x, mouse_y, mouse_buttons;
static int mouse_minx = 0, mouse_maxx = 639, mouse_miny = 0, mouse_maxy = 199;
static int mouse_last_x, mouse_last_y;
static int press_count[3], press_x[3], press_y[3], prev_buttons;
static int mickey_x, mickey_y;

/* relative motion in mickeys (host mouse in captured mode, test scripts) */
void mouse_relative(int dx, int dy, int buttons) {
    mickey_x += dx; mickey_y += dy;
    mouse_x += dx; mouse_y += dy / 2;
    if (mouse_x < mouse_minx) mouse_x = mouse_minx;
    if (mouse_x > mouse_maxx) mouse_x = mouse_maxx;
    if (mouse_y < mouse_miny) mouse_y = mouse_miny;
    if (mouse_y > mouse_maxy) mouse_y = mouse_maxy;
    for (int k = 0; k < 3; k++) {
        if ((buttons & (1 << k)) && !(prev_buttons & (1 << k))) {
            press_count[k]++; press_x[k] = mouse_x; press_y[k] = mouse_y;
        }
    }
    prev_buttons = buttons;
    mouse_buttons = buttons;
}

void mouse_update(int x, int y, int buttons) {
    /* x, y in 0..1 of the screen */
    int nx = mouse_minx + x * (mouse_maxx - mouse_minx) / 1000;
    int ny = mouse_miny + y * (mouse_maxy - mouse_miny) / 1000;
    mickey_x += (nx - mouse_x); mickey_y += (ny - mouse_y) * 2;
    mouse_x = nx; mouse_y = ny;
    for (int b = 0; b < 3; b++) {
        if ((buttons & (1 << b)) && !(prev_buttons & (1 << b))) {
            press_count[b]++; press_x[b] = mouse_x; press_y[b] = mouse_y;
        }
    }
    prev_buttons = buttons;
    mouse_buttons = buttons;
}

unsigned mouse_fn_count[64];      /* int 33h calls by function (live `m` command) */

void rt_host_sleep_ms(int ms);

void mouse_int33(void) {
    mouse_fn_count[AX & 63]++;
    {   /* busy-wait relief: menu screens poll fn 3 in a tight loop */
        static int lx = -1, ly, lb; static unsigned same;
        if (AX == 3 || AX == 0x0b) {
            if (mouse_x == lx && mouse_y == ly && mouse_buttons == lb) {
                if (++same >= 256) { same = 0; rt_host_sleep_ms(1); }
            } else { lx = mouse_x; ly = mouse_y; lb = mouse_buttons; same = 0; }
        }
    }
    switch (AX) {
    case 0x00: SET_AX(0xffff); SET_BX(2); break;
    case 0x01: case 0x02: break;
    case 0x03: SET_BX(mouse_buttons); SET_CX(mouse_x); SET_DX(mouse_y); break;
    case 0x04: mouse_x = CX; mouse_y = DX; break;
    case 0x05: {
        int b = BX < 3 ? BX : 0;
        SET_AX(mouse_buttons); SET_BX(press_count[b]); SET_CX(press_x[b]); SET_DX(press_y[b]);
        press_count[b] = 0;
        break;
    }
    case 0x06: SET_AX(mouse_buttons); SET_BX(0); SET_CX(mouse_x); SET_DX(mouse_y); break;
    case 0x07: mouse_minx = (int16_t)CX; mouse_maxx = (int16_t)DX; break;
    case 0x08: mouse_miny = (int16_t)CX; mouse_maxy = (int16_t)DX; break;
    case 0x0b: SET_CX(mickey_x); SET_DX(mickey_y); mickey_x = mickey_y = 0; break;
    case 0x0c: case 0x0f: case 0x1a: case 0x1d: break;
    case 0x24: SET_BX(0x0626); SET_CX(0x0400); break;
    default: rt_log("unhandled int 33h AX=%04x", AX);
    }
    (void)mouse_last_x; (void)mouse_last_y;
}

void mux_int2f(void) {
    switch (AX) {
    case 0x1600: SET_AL(0); break;  /* no Windows */
    case 0x1686: SET_AX(0); break;  /* in protected mode: yes */
    case 0x1687: SET_AX(0x8001); break;
    default:
        if (AH == 0xfa || AH == 0x15) {
            rt_log("int 2Fh AX=%04x (not installed)", AX);
            SET_AL(0);
        } else {
            rt_log("unhandled int 2Fh AX=%04x", AX);
        }
    }
}
