/* VGA emulation (planar memory, registers, DAC) and SDL2 presentation. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <direct.h>
#include <SDL.h>
#include "rt.h"
#include "settings.h"
#include "scene.h"
#include "present.h"
#include "hud.h"
#include "native.h"
#include "art.h"
static struct native_stats native_last;

int vga_planar;                     /* A0000 accesses go through vga_read8/write8 */
uint8_t vga_dac[256][3];
static uint8_t planes[4][65536];
/* Native renderer (RENDERER.md stage 3): which code last wrote each VGA byte.
 * 1 = TFX's flat polygon fill (471F00h-4726FFh: the span filler, its unrolled
 * short-span entries and the wide-span path), i.e. the 3D world; 0 = anything
 * else (cockpit, HUD, MFDs, sprites, ...).  The mask of the displayed page
 * decides where the natively drawn world shows through. */
static uint8_t owner[4][65536];
/* ui_filter=remacri (art.h): what each byte shows.  0 nothing known;
 * FFh << 24 | y << 12 | x: copied (write mode 1) from the cockpit template at
 * BB80h, from its pixel (x, y); FEh << 24 | n: written by sprite n (scene.c
 * gives it its asset once the sprite is recognised); (asset + 1) << 24 |
 * y << 12 | x: pixel (x, y) of an art asset */
static uint32_t prov[4][65536];
static uint32_t put_tag;
static uint16_t latch_off;
static uint32_t prov_frame[320 * 200];
/* pixels written by the 3D span fills: flat 471F00h-4726FFh plus the
 * stippled cloud fills 4726CEh/4727FFh (to 472930h, TFX_CODE.md 9.15),
 * textured 475334h and Gouraud 4756CCh (to 4759B0h) */
static inline uint8_t writer_3d(void) {
    return (rt_eip >= 0x471f00u && rt_eip < 0x472930u) || (rt_eip >= 0x475334u && rt_eip < 0x4759b0u) ||
           (scene_line_draw && rt_eip >= 0x44ea99u && rt_eip < 0x44f26au) ||  /* model lines (0x44EA99) */
           (scene_point_draw && rt_eip >= 0x4759a8u && rt_eip < 0x475a10u) ||  /* VM points (0x4759A8/D4) */
           (scene_line2_draw && rt_eip >= 0x454336u && rt_eip < 0x454446u) ||  /* model lines through 0x454336 */
           (scene_disc_draw && rt_eip >= 0x472930u && rt_eip < 0x474320u);     /* discs' other fill styles (0x47292F..) */
}
/* pixels of a captured HUD line, bitmap or dot (TFX_CODE.md 9.17): the HUD
 * line rasteriser 454212h-454335h, the bitmap blit 45CFA4h-45D171h and the
 * pixel plot 45658Ch-4565D6h */
static inline uint8_t writer_hud(void) {
    return scene_hud_draw && ((rt_eip >= 0x454212u && rt_eip < 0x454336u) || (rt_eip >= 0x45cfa4u && rt_eip < 0x45d172u) ||
                              (rt_eip >= 0x45658cu && rt_eip < 0x4565d7u));
}
static uint8_t mask3d[640 * 480];
/* under[]: the last value a 3D span fill overwrote while the byte belonged
 * to the UI (the background behind the 3D).  With renderer=native the
 * picture under the native scene uses it, so where the sharper native
 * silhouette doesn't cover a coarse game 3D pixel the background shows. */
static uint8_t under[4][65536], under_ok[4][65536];
static uint32_t last_eip[4][65536];   /* debug (cmdfile w X Y): the code that last wrote each byte */
static uint16_t ui_stamp[4][65536];    /* frame (flip count) of the last UI write */
static uint32_t flips;                 /* page flips (display start changes) */
/* owner: 0 UI, 1 3D drawn natively, 2 3D kept from the game (screen-space
 * sky/haze), 3 HUD overlay drawn natively (lines, bitmaps).  Under a HUD
 * pixel is what was there before it, as under a 3D pixel.  under[] is trusted only when the UI pixel was drawn in the
 * same frame (arming screen: background copied, then the model); in flight
 * the cockpit is drawn after the 3D, so an older value would be stale. */
static inline void put(int p, uint32_t off, uint8_t val) {
    uint8_t o = writer_3d() ? (scene_bg_draw ? 2 : 1) : writer_hud() ? 3 :
                (scene_sprite_draw && rt_eip >= 0x44fb8cu && rt_eip < 0x4508c0u && scene_sprite_pixel(p, (uint16_t)off, val)) ? 3 : 0;
    uint8_t cur = owner[p][off];
    if ((o == 1 || o == 3) && cur != 1 && cur != 3) {
        under[p][off] = planes[p][off];
        under_ok[p][off] = cur == 2 || ui_stamp[p][off] == (uint16_t)flips;
    }
    if (!o) ui_stamp[p][off] = (uint16_t)flips;
    else if (scene_disc_draw && scene_disc_draw <= 10) scene_fill_px[scene_disc_draw - 1]++;
    planes[p][off] = val; owner[p][off] = o; last_eip[p][off] = rt_eip;
    prov[p][off] = (o == 3 && scene_sprite_draw && !writer_hud()) ? 0xfe000000u | (scene_sprite_seq & 0xffffffu) : o ? 0 : put_tag;
}
/* scene.c: a recognised sprite's pixels that still show it get its asset */
void vga_prov_resolve(int p, uint16_t off, uint32_t pending, uint32_t tag) {
    if (prov[p][off] == pending) prov[p][off] = tag;
}
/* a sprite pixel taken as HUD overlay goes back to the UI (the sprite turned
 * out to be cockpit art: scene.c) */
void vga_owner_to_ui(int p, uint16_t off) {
    if (owner[p][off] == 3) { owner[p][off] = 0; ui_stamp[p][off] = (uint16_t)flips; }
}
/* asset dump (cmdfile A): an image of palette indices (0x100 = transparent,
 * saved as magenta with alpha 0) */
char asset_dir[260];
uint32_t vga_palette_argb(uint8_t idx);
void vga_save_indexed(const char *path, int w, int h, const uint16_t *idx) {
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!s) return;
    for (int y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)s->pixels + y * s->pitch);
        for (int x = 0; x < w; x++) {
            uint16_t c = idx[y * w + x];
            row[x] = c > 255 ? 0x00ff00ffu : (vga_palette_argb((uint8_t)c) | 0xff000000u);
        }
    }
    SDL_SaveBMP(s, path);
    SDL_FreeSurface(s);
}
static uint8_t latch[4];
static uint8_t seq[8], gc[16], crtc[32], attr[32], misc = 0x63;

/* native.c, widescreen: the colour the game cleared its 3D view to, if any
 * of that fill still shows in rows top..bottom of the displayed picture: the
 * commonest index among the UI pixels last written by the rectangle fill
 * 0x458E34 (to 4590DFh).  -1 when fewer than 200 are left (the sky bands and
 * the world normally cover it all). */
int vga_clear_index(int top, int bottom) {
    static unsigned cnt[256];
    uint32_t start = ((uint32_t)crtc[0x0c] << 8) | crtc[0x0d], pitch = (uint32_t)crtc[0x13] * 2;
    unsigned total = 0;
    if (top < 0) top = 0;
    if (bottom > 199) bottom = 199;
    memset(cnt, 0, sizeof cnt);
    for (int y = top; y <= bottom; y++)
        for (int x = 0; x < 320; x++) {
            uint32_t o = (start + (uint32_t)y * pitch + (uint32_t)(x >> 2)) & 0xffff;
            int q = x & 3;
            if (owner[q][o] || last_eip[q][o] < 0x458e34u || last_eip[q][o] >= 0x4590e0u) continue;
            cnt[planes[q][o]]++; total++;
        }
    if (total < 200) return -1;
    int best = 0;
    for (int i = 1; i < 256; i++) if (cnt[i] > cnt[best]) best = i;
    return best;
}

static int seq_idx, gc_idx, crtc_idx, attr_idx, attr_ff;
static int dac_w, dac_r, dac_wc, dac_rc;
static uint8_t dac_mask = 0xff;
static int cur_mode = 3;

/* ---- mode set ---------------------------------------------------------------- */
static const uint8_t crtc_13h[25] = {
    0x5f, 0x4f, 0x50, 0x82, 0x54, 0x80, 0xbf, 0x1f, 0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x9c, 0x8e, 0x8f, 0x28, 0x40, 0x96, 0xb9, 0xa3, 0xff};
static const uint8_t crtc_03h[25] = {
    0x5f, 0x4f, 0x50, 0x82, 0x55, 0x81, 0xbf, 0x1f, 0x00, 0x4f, 0x0d, 0x0e, 0x00, 0x00, 0x00,
    0x00, 0x9c, 0x8e, 0x8f, 0x28, 0x1f, 0x96, 0xb9, 0xa3, 0xff};

static void default_palette(void) {
    /* EGA 16 colours, then a greyscale ramp: enough for text mode / startup */
    static const uint8_t ega[16][3] = {
        {0, 0, 0}, {0, 0, 42}, {0, 42, 0}, {0, 42, 42}, {42, 0, 0}, {42, 0, 42}, {42, 21, 0},
        {42, 42, 42}, {21, 21, 21}, {21, 21, 63}, {21, 63, 21}, {21, 63, 63}, {63, 21, 21},
        {63, 21, 63}, {63, 63, 21}, {63, 63, 63}};
    for (int i = 0; i < 256; i++) {
        if (i < 16) memcpy(vga_dac[i], ega[i], 3);
        else vga_dac[i][0] = vga_dac[i][1] = vga_dac[i][2] = (uint8_t)((i & 63));
    }
}

void vga_set_mode(int mode) {
    rt_log("video mode %02xh", mode);
    cur_mode = mode;
    memset(seq, 0, sizeof seq); memset(gc, 0, sizeof gc); memset(attr, 0, sizeof attr);
    for (int i = 0; i < 16; i++) attr[i] = (uint8_t)i;
    if (mode == 0x13) {
        memcpy(crtc, crtc_13h, 25);
        seq[0] = 3; seq[1] = 1; seq[2] = 0x0f; seq[4] = 0x0e;
        gc[5] = 0x40; gc[6] = 0x05; gc[7] = 0x0f; gc[8] = 0xff;
        attr[0x10] = 0x41; attr[0x12] = 0x0f;
        vga_planar = 1;
        memset(planes, 0, sizeof planes);
    } else {
        memcpy(crtc, crtc_03h, 25);
        seq[0] = 3; seq[1] = 0; seq[2] = 3; seq[4] = 2;
        gc[5] = 0x10; gc[6] = 0x0e; gc[8] = 0xff;
        attr[0x10] = 0x0c;
        vga_planar = 0;
        if (mode == 3) {
            for (int i = 0; i < 80 * 25; i++) { MEM[0xb8000 + i * 2] = ' '; MEM[0xb8001 + i * 2] = 7; }
        }
    }
    default_palette();
    WB(BDA_BASE + 0x49, (uint8_t)mode);
}

int vga_get_mode(void) { return cur_mode; }

/* ---- memory access ------------------------------------------------------------ */
static inline int chain4(void) { return seq[4] & 8; }

struct vprof { uint32_t eip, n; };
static struct vprof vga_prof[4096];
void vga_write8(uint32_t a, uint8_t v) {
    { struct vprof *e = &vga_prof[(rt_eip * 2654435761u) >> 20]; if (e->eip == rt_eip || !e->n) { e->eip = rt_eip; e->n++; } }
    uint32_t off = a - 0xA0000u;
    uint8_t mm = seq[2] & 0x0f;
    if (chain4()) {
        int p = off & 3;
        if (mm & (1 << p)) put(p, (off >> 2) & 0xffff, v);
        return;
    }
    off &= 0xffff;
    uint8_t mode = gc[5] & 3;
    uint8_t bitmask = gc[8];
    uint8_t rot = gc[3] & 7, func = (gc[3] >> 3) & 3;
    uint8_t res[4];
    if (mode == 1) {
        uint32_t rel = (uint32_t)latch_off - 0xbb80u;   /* from the cockpit template: remember the source pixel */
        for (int p = 0; p < 4; p++) if (mm & (1 << p)) {
            put_tag = latch_off >= 0xbb80u ? 0xff000000u | (rel / 80u) << 12 | ((rel % 80u) * 4u + (uint32_t)p) : 0;
            put(p, off, latch[p]);
        }
        put_tag = 0;
        return;
    }
    if (mode == 0) {
        uint8_t vv = (uint8_t)((v >> rot) | (v << (8 - rot)));
        for (int p = 0; p < 4; p++) {
            uint8_t d = vv;
            if (gc[1] & (1 << p)) d = (gc[0] & (1 << p)) ? 0xff : 0x00;
            res[p] = d;
        }
    } else if (mode == 2) {
        for (int p = 0; p < 4; p++) res[p] = (v & (1 << p)) ? 0xff : 0x00;
    } else {                        /* mode 3 */
        uint8_t vv = (uint8_t)((v >> rot) | (v << (8 - rot)));
        bitmask &= vv;
        for (int p = 0; p < 4; p++) res[p] = (gc[0] & (1 << p)) ? 0xff : 0x00;
    }
    for (int p = 0; p < 4; p++) {
        if (!(mm & (1 << p))) continue;
        uint8_t d = res[p], l = latch[p];
        switch (func) {
        case 1: d &= l; break;
        case 2: d |= l; break;
        case 3: d ^= l; break;
        }
        put(p, off, (uint8_t)((d & bitmask) | (l & ~bitmask)));
    }
}

uint8_t vga_read8(uint32_t a) {
    uint32_t off = a - 0xA0000u;
    if (chain4()) return planes[off & 3][(off >> 2) & 0xffff];
    off &= 0xffff;
    for (int p = 0; p < 4; p++) latch[p] = planes[p][off];
    latch_off = (uint16_t)off;
    if (gc[5] & 8) {                /* read mode 1: colour compare */
        uint8_t r = 0;
        for (int bit = 0; bit < 8; bit++) {
            int match = 1;
            for (int p = 0; p < 4; p++) {
                if (!(gc[7] & (1 << p))) continue;
                int pb = (latch[p] >> bit) & 1, cb = (gc[2] >> p) & 1;
                if (pb != cb) { match = 0; break; }
            }
            if (match) r |= (uint8_t)(1 << bit);
        }
        return r;
    }
    return latch[gc[4] & 3];
}

/* ---- ports ---------------------------------------------------------------------- */
static uint64_t frame_us0;
static uint32_t flip_last = 0xffffffffu;
static int flip_dirty;
/* palette index at (x, y) of the page now displayed (320x200 planar) */
static uint8_t page_pixel(int x, int y) {
    uint32_t start = ((uint32_t)crtc[0x0c] << 8) | crtc[0x0d];
    uint32_t pitch = (uint32_t)crtc[0x13] * 2;
    return planes[x & 3][(start + (uint32_t)y * pitch + (uint32_t)(x >> 2)) & 0xffff];
}

static uint64_t flip_us;              /* host time of the last page flip */
static uint64_t flip_delay_sum, flip_delay_max, flip_delay_n;   /* flip -> present (cmdfile f) */
static void check_flip(void) {
    if (!flip_dirty) return;
    flip_dirty = 0;
    uint32_t st = ((uint32_t)crtc[0x0c] << 8) | crtc[0x0d];
    if (st != flip_last) { flip_last = st; flips++; flip_us = host_ticks_us(); scene_end_frame(); scene_check(page_pixel); }
}

void vga_flip_check(void) { check_flip(); }      /* scene.c, before each captured primitive */

/* a page flip since the last present (hw.c, with DLSS Frame Generation) */
static uint32_t flips_presented;
int video_flip_pending(void) {
    check_flip();
    return flips != flips_presented;
}


uint8_t vga_in(uint16_t port) {
    switch (port) {
    case 0x3c0: return (uint8_t)attr_idx;
    case 0x3c1: return attr[attr_idx & 31];
    case 0x3c4: return (uint8_t)seq_idx;
    case 0x3c5: return seq[seq_idx & 7];
    case 0x3c6: return dac_mask;
    case 0x3c7: return 3;
    case 0x3c8: return (uint8_t)dac_w;
    case 0x3c9: {
        uint8_t v = vga_dac[dac_r][dac_rc];
        if (++dac_rc == 3) { dac_rc = 0; dac_r = (dac_r + 1) & 255; }
        return v;
    }
    case 0x3cc: return misc;
    case 0x3ce: return (uint8_t)gc_idx;
    case 0x3cf: return gc[gc_idx & 15];
    case 0x3d4: case 0x3b4: return (uint8_t)crtc_idx;
    case 0x3d5: case 0x3b5: return crtc[crtc_idx & 31];
    case 0x3da: case 0x3ba: {
        check_flip();
        attr_ff = 0;
        /* 70 Hz frame: 14286 us, vertical retrace for the last ~1100 us */
        uint64_t t = (host_ticks_us() - frame_us0) % 14286;
        uint8_t r = 0;
        if (t >= 13186) r |= 0x08 | 0x01;
        else if ((t % 32) < 6) r |= 0x01;  /* horizontal blanking */
        {   /* busy-wait relief: a program spinning until vertical retrace
             * sleeps 1 ms at a time while retrace is still > 2 ms away */
            static unsigned polls; static uint64_t frame_no;
            uint64_t fn = (host_ticks_us() - frame_us0) / 14286;
            if (fn != frame_no) { frame_no = fn; polls = 0; }
            if (++polls > 64 && t < 13186 - 2000) { extern void rt_host_sleep_ms(int); rt_host_sleep_ms(1); }
        }
        /* every retrace poll is a good moment to let the runtime breathe */
        irq_pending = 1;
        return r;
    }
    }
    return 0xff;
}

void vga_out(uint16_t port, uint8_t v) {
    switch (port) {
    case 0x3c0:
        if (!attr_ff) attr_idx = v & 0x3f; else attr[attr_idx & 31] = v;
        attr_ff ^= 1;
        break;
    case 0x3c2: misc = v; break;
    case 0x3c4: seq_idx = v; break;
    case 0x3c5:
        seq[seq_idx & 7] = v;
        break;
    case 0x3c6: dac_mask = v; break;
    case 0x3c7: dac_r = v; dac_rc = 0; break;
    case 0x3c8: dac_w = v; dac_wc = 0; if (getenv("TFX_DEBUG")) { static int n; if (n++ < 40) rt_log("DAC write start index %d @%08x", v, rt_eip); } break;
    case 0x3c9:
        vga_dac[dac_w][dac_wc] = v & 63;
        if (++dac_wc == 3) { dac_wc = 0; dac_w = (dac_w + 1) & 255; }
        break;
    case 0x3ce: gc_idx = v; break;
    case 0x3cf: gc[gc_idx & 15] = v; break;
    case 0x3d4: case 0x3b4: crtc_idx = v; break;
    case 0x3d5: case 0x3b5:
        crtc[crtc_idx & 31] = v;
        if ((crtc_idx & 31) == 0x0c || (crtc_idx & 31) == 0x0d) flip_dirty = 1;
        break;
    }
}

/* ---- presentation ---------------------------------------------------------------- */
static SDL_Window *win;
static SDL_Renderer *ren;
static int use_d3d;                  /* presenter: Direct3D 12 (d3d12.c) or the SDL renderer */
static SDL_Texture *tex;
static int tex_w, tex_h;
static uint32_t pixels[640 * 480];
static int last_w = 320, last_h = 200;
static const uint8_t *font8x16;     /* optional; text mode shown as blocks without it */

static Uint32 tick_cb(Uint32 interval, void *param) {
    (void)param;
    irq_pending = 1;
    return interval;
}

void vga_init(void) {
    vga_set_mode(3);
    frame_us0 = host_ticks_us();
}

void video_init(void) {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_AUDIO) != 0)
        rt_fatal("SDL_Init: %s", SDL_GetError());
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    win = SDL_CreateWindow("TFX", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           settings.window_w, settings.window_h,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                           (settings.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!win) rt_fatal("SDL_CreateWindow: %s", SDL_GetError());
    joy_init();
    if (settings.presenter == PRESENTER_D3D12) {
        use_d3d = d3d_init(win) == 0;
        if (!use_d3d) rt_log("presenter: Direct3D 12 unavailable, using the SDL renderer");
    }
    if (!use_d3d && settings.renderer == RENDERER_NATIVE) {
        settings.renderer = RENDERER_GAME;
        rt_log("settings: renderer=native needs the Direct3D 12 presenter; using renderer=game");
    }
    if (!use_d3d) {
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC * 0);
        if (!ren) rt_fatal("SDL_CreateRenderer: %s", SDL_GetError());
    }
    SDL_AddTimer(1, tick_cb, NULL);
    SDL_ShowCursor(SDL_DISABLE);
}

static void ensure_tex(int w, int h) {
    if (tex && w == tex_w && h == tex_h) return;
    if (tex) SDL_DestroyTexture(tex);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
    tex_w = w; tex_h = h;
}

static inline uint32_t dac_rgb(uint8_t i);
uint32_t vga_palette_argb(uint8_t i) { return dac_rgb(i); }
static inline uint32_t dac_rgb(uint8_t i) {
    i &= dac_mask;
    uint8_t r = vga_dac[i][0], g = vga_dac[i][1], b = vga_dac[i][2];
    return 0xff000000u | ((uint32_t)((r << 2) | (r >> 4)) << 16) |
           ((uint32_t)((g << 2) | (g >> 4)) << 8) | (uint32_t)((b << 2) | (b >> 4));
}

static int wide_trim;          /* set by present_scaled: widescreen with the world at the sides is on */
static float wide_ext;         /* game pixels the 3D view extends at each side (0: not wide) */
float video_wide_ext(void) { return wide_ext; }
static void render_256(int *pw, int *ph) {
    int hde = crtc[1] + 1;
    int w = hde * 4;
    int vde = crtc[0x12] | ((crtc[7] & 0x02) << 7) | ((crtc[7] & 0x40) << 3);
    vde += 1;
    int scan = (crtc[9] & 0x1f) + 1;
    if (crtc[9] & 0x80) scan *= 2;
    int h = vde / scan;
    if (w > 640) w = 640;
    if (h > 480) h = 480;
    if (w < 1) w = 320;
    if (h < 1) h = 200;
    check_flip();
    uint32_t start = ((uint32_t)crtc[0x0c] << 8) | crtc[0x0d];
    uint32_t pitch = (uint32_t)crtc[0x13] * 2;
    if (chain4()) {
        /* chain-4: CRTC addresses dwords; plane index = x & 3 */
        pitch = (uint32_t)crtc[0x13] * 2;
    }
    uint32_t lut[256];
    for (int i = 0; i < 256; i++) lut[i] = dac_rgb((uint8_t)i);
    int bg = settings.renderer == RENDERER_NATIVE && w == 320 && h == 200 && scene_current();
    for (int y = 0; y < h; y++) {
        uint32_t row = start + y * pitch;
        uint32_t *dst = pixels + y * w;
        for (int x = 0; x < w; x++) {
            uint32_t o = (row + (x >> 2)) & 0xffff;
            int q = x & 3;
            uint8_t ow = owner[q][o];
            dst[x] = lut[bg && (ow == 1 || ow == 3) && under_ok[q][o] ? under[q][o] : planes[q][o]];
        }
        uint8_t *m = mask3d + y * w;
        /* 0 UI (the game's pixel shows), 255 3D, 160 HUD overlay (drawn natively) */
        for (int x = 0; x < w; x++) {
            uint8_t ow = owner[x & 3][(row + (x >> 2)) & 0xffff];
            m[x] = ow == 3 ? 160 : ow ? 255 : 0;
        }
        if (wide_trim && w == 320) {
            /* widescreen, world at the sides: the cockpit art fills its outer
             * edges with one flat dark colour (#242424 in the F-22 cockpit);
             * a run of it touching the left or right edge becomes see-through,
             * so the panel ends at its instrument housings and the world
             * continues.  Only very dark runs: status bars etc. stay. */
            for (int side = 0; side < 2; side++) {
                int x0 = side ? w - 1 : 0, dir = side ? -1 : 1;
                uint32_t o0 = (row + (x0 >> 2)) & 0xffff;
                if (m[x0]) continue;
                uint8_t idx = planes[x0 & 3][o0];
                uint32_t c = lut[idx];
                if (((c >> 16) & 255) > 0x30 || ((c >> 8) & 255) > 0x30 || (c & 255) > 0x30) continue;
                for (int x = x0, k = 0; k < w / 4; x += dir, k++) {
                    uint32_t o = (row + (x >> 2)) & 0xffff;
                    if (owner[x & 3][o] || planes[x & 3][o] != idx) break;
                    m[x] = 255;
                }
            }
        }
    }
    *pw = w; *ph = h;
    /* ui_filter=remacri: per pixel of the displayed page, the art asset it
     * shows.  The template is recognised by the hash of its rows */
    if (bg && art_active()) {
        static uint16_t tbuf[320 * 200];
        int tmpl = -1, tgh = 0;
        for (int i = 0, ty, gw, gh; i < art_count(); i++) {
            uint32_t hs;
            if (!art_info(i, &ty, &gw, &gh, &hs) || ty != ART_TEMPLATE || gw != 320 || gh < 1 || gh > 200) continue;
            for (int y = 0; y < gh; y++)
                for (int x = 0; x < 320; x++) tbuf[y * 320 + x] = planes[x & 3][(0xbb80u + y * 80u + (x >> 2)) & 0xffff];
            if (art_hash(tbuf, 320, gh) == hs) { tmpl = i; tgh = gh; break; }
        }
        for (int y = 0; y < 200; y++) {
            uint32_t row = start + y * pitch;
            for (int x = 0; x < 320; x++) {
                uint32_t o = (row + (x >> 2)) & 0xffff, t = prov[x & 3][o], hi = t >> 24;
                if (owner[x & 3][o]) t = 0;
                else if (hi == 0xff) t = tmpl >= 0 && (int)((t >> 12) & 0xfff) < tgh ? (uint32_t)(tmpl + 1) << 24 | (t & 0xffffffu) : 0;
                else if (hi == 0xfe) t = 0;
                prov_frame[y * 320 + x] = t;
            }
        }
        d3d_set_prov(prov_frame, 320, 200);
    } else d3d_set_prov(NULL, 0, 0);
}

static void render_text(int *pw, int *ph) {
    static const uint8_t pal16[16] = {0, 1, 2, 3, 4, 5, 20, 7, 56, 57, 58, 59, 60, 61, 62, 63};
    (void)pal16;
    int cols = 80, rows = 25, cw = 8, chh = 16;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            uint8_t ch = MEM[0xb8000 + (r * cols + c) * 2];
            uint8_t at = MEM[0xb8001 + (r * cols + c) * 2];
            uint32_t fg = dac_rgb(at & 15), bg = dac_rgb((at >> 4) & 7);
            for (int y = 0; y < chh; y++) {
                uint8_t bits;
                if (font8x16) bits = font8x16[ch * 16 + y];
                else bits = (ch != ' ' && ch && y > 3 && y < 13) ? 0x7e : 0;
                uint32_t *dst = pixels + (r * chh + y) * 640 + c * cw;
                for (int x = 0; x < cw; x++) dst[x] = (bits & (0x80 >> x)) ? fg : bg;
            }
        }
    }
    *pw = 640; *ph = 400;
}

/* Debug view of the native renderer's capture: the last frame's polygons,
 * re-projected from their view-space vertices at window resolution. */
static struct overlay_vertex *wire;
static int wire_n, wire_cap;
static void build_scene_wireframe(const SDL_Rect *d) {
    const struct scene_frame *f = scene_last();
    wire_n = 0;
    if (!f) return;
    double sx = d->w / 320.0, sy = d->h / 200.0;
    struct overlay_vertex pts[65];
    for (int i = 0; i < f->nprims; i++) {
        const struct scene_prim *p = &f->prims[i];
        int ok = 1;
        for (int k = 0; k < p->count; k++) {
            const struct scene_vert *v = &f->verts[p->first + k];
            if (v->near || v->z <= 0) { ok = 0; break; }
            double gx = 256.0 * v->x / v->z + p->cx + 0.5, gy = 256.0 * v->y / v->z + p->cy + 0.5;
            pts[k].x = (float)(d->x + gx * sx); pts[k].y = (float)(d->y + gy * sy);
            pts[k].argb = 0xa000ff60u;
        }
        if (!ok) continue;
        if (wire_n + 2 * p->count > wire_cap) {
            wire_cap = (wire_n + 2 * p->count) * 2;
            wire = realloc(wire, (size_t)wire_cap * sizeof *wire);
        }
        for (int k = 0; k < p->count; k++) { wire[wire_n++] = pts[k]; wire[wire_n++] = pts[(k + 1) % p->count]; }
    }
}
static void draw_scene_wireframe(const SDL_Rect *d) {    /* SDL presenter */
    build_scene_wireframe(d);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 0, 255, 96, 160);
    for (int i = 0; i + 1 < wire_n; i += 2) SDL_RenderDrawLineF(ren, wire[i].x, wire[i].y, wire[i + 1].x, wire[i + 1].y);
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
}

/* Scale the w x h frame to the window per tfx.ini (aspect, scaling). */
static SDL_Texture *up;              /* integer-upscaled copy for "sharp" scaling */
static char window_shot[512];        /* live command W FILE: save the presented window */
static void save_window_shot(void) {
    if (!window_shot[0]) return;
    int ow, oh;
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
    if (s && SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0)
        SDL_SaveBMP(s, window_shot);
    if (s) SDL_FreeSurface(s);
    window_shot[0] = 0;
}
static int up_w, up_h;
/* destination rectangle of a w x h frame in an ow x oh output (tfx.ini aspect/scaling) */
static SDL_Rect fit_rect(int w, int h, int ow, int oh) {
    double src_aspect = (double)w / h;
    double disp_aspect = settings.aspect == ASPECT_4_3 ? 4.0 / 3.0 :
                         settings.aspect == ASPECT_SQUARE ? src_aspect : (double)ow / oh;
    SDL_Rect d;
    if (settings.scaling == SCALE_PIXEL) {
        /* whole-number factors; vertical factor follows the pixel aspect */
        double pa = disp_aspect / src_aspect;
        int kx = ow / w, ky = 1;
        for (; kx >= 1; kx--) {
            ky = settings.aspect == ASPECT_FILL ? oh / h : (int)(kx / pa + 0.5);
            if (ky < 1) ky = 1;
            if (h * ky <= oh) break;
        }
        if (kx < 1) kx = 1;
        d.w = w * kx; d.h = h * ky;
    } else if ((double)ow / oh > disp_aspect) {
        d.h = oh; d.w = (int)(oh * disp_aspect + 0.5);
    } else {
        d.w = ow; d.h = (int)(ow / disp_aspect + 0.5);
    }
    d.x = (ow - d.w) / 2; d.y = (oh - d.h) / 2;
    return d;
}

static void present_scaled(int w, int h) {
    int ow, oh;
    if (use_d3d) {
        d3d_output_size(&ow, &oh);
        SDL_Rect d = fit_rect(w, h, ow, oh);
        wire_n = 0;
        int view3d = vga_planar && w == 320 && h == 200 && scene_current();
        if (settings.renderer == RENDERER_WIREFRAME && view3d) build_scene_wireframe(&d);
        const struct native_vertex *tris = NULL;
        const struct native_res *nres = NULL;
        int ntris = 0;
        /* widescreen: a full-screen 3D view fills the window width; the
         * game's picture (cockpit, HUD) stays in d, its edges continued */
        int wx = d.x, ww = d.w;
        float ext = 0;
        if (settings.renderer == RENDERER_NATIVE && view3d && settings.widescreen && d.x > 0 &&
            native_full_view(scene_last())) {
            ext = (float)d.x * 320.0f / (float)d.w;
            wx = 0; ww = ow;
        }
        wide_trim = ext > 0 && !settings.widescreen_cockpit;   /* for the next picture (render_256) */
        wide_ext = ext;
        if (settings.renderer == RENDERER_NATIVE && view3d)
            ntris = native_build(scene_last(), d.x, d.y, d.w, d.h, ow, oh, ext, &tris, &nres, &native_last);
        if (!ntris) { wx = d.x; ww = d.w; }
        /* upscaler: new scene frame or a re-present; a camera cut (most
         * primitives without a match) discards the history */
        struct upscale_params up = {0};
        up.enabled = settings.renderer == RENDERER_NATIVE && settings.upscaler != UPSCALER_OFF;
        up.scale = (float)settings.render_scale / 100.0f;
        up.fsr = settings.upscaler == UPSCALER_FSR;
        up.dlss = settings.upscaler == UPSCALER_DLSS;
        up.new_scene = native_last.new_frame;
        int tot = native_last.matched + native_last.unmatched;
        up.reset = native_last.new_frame && (native_last.history_frames != 1 || (tot > 0 && native_last.matched * 2 < tot));
        {   /* the HUD lines and bitmaps, drawn sharp over the native view */
            const struct hud_vertex *hv = NULL; const uint8_t *ha = NULL;
            int hn = ntris ? hud_build(scene_last(), d.x, d.y, d.w, d.h, ow, oh, &hv, &ha) : 0;
            d3d_set_hud(hv, hn, ha);
        }
        d3d_present(pixels, w, h, settings.scaling, d.x, d.y, d.w, d.h, wire, wire_n, window_shot,
                    tris, ntris, ntris ? mask3d : NULL, nres, wx, ww, &up);
        window_shot[0] = 0;
        return;
    }
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    SDL_Rect d = fit_rect(w, h, ow, oh);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    if (settings.scaling == SCALE_SHARP && SDL_RenderTargetSupported(ren)) {
        int k = (d.w + w - 1) / w, k2 = (d.h + h - 1) / h;
        if (k2 > k) k = k2;
        if (k > 8) k = 8;
        if (k < 1) k = 1;
        if (!up || up_w != w * k || up_h != h * k) {
            if (up) SDL_DestroyTexture(up);
            up = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w * k, h * k);
            up_w = w * k; up_h = h * k;
            if (up) SDL_SetTextureScaleMode(up, SDL_ScaleModeLinear);
        }
        if (up) {
            SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);
            SDL_SetRenderTarget(ren, up);
            SDL_RenderCopy(ren, tex, NULL, NULL);
            SDL_SetRenderTarget(ren, NULL);
            SDL_RenderCopy(ren, up, NULL, &d);
            goto overlay;
        }
    }
    SDL_SetTextureScaleMode(tex, settings.scaling == SCALE_SMOOTH ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    SDL_RenderCopy(ren, tex, NULL, &d);
overlay:
    if (settings.renderer == RENDERER_WIREFRAME && vga_planar && w == 320 && h == 200) draw_scene_wireframe(&d);
    save_window_shot();
    SDL_RenderPresent(ren);
}

void video_present(void) {
    if (!ren && !use_d3d) return;
    int w, h;
    {   /* debug heartbeat once per second */
        static uint64_t last;
        if (getenv("TFX_DEBUG") && host_ticks_us() - last > 1000000) {
            last = host_ticks_us();
            int nz = 0, pal = 0;
            for (int p = 0; p < 4; p++) for (int i = 0; i < 65536; i++) nz += planes[p][i] != 0;
            for (int i = 0; i < 256; i++) pal += vga_dac[i][0] + vga_dac[i][1] + vga_dac[i][2];
            rt_log("[hb] eip=%08x esp=%08x planes_nonzero=%d palsum=%d seq2=%02x seq4=%02x "
                   "crtc_start=%02x%02x off=%02x stack: %08x %08x %08x %08x", rt_eip, esp, nz, pal,
                   seq[2], seq[4], crtc[0xc], crtc[0xd], crtc[0x13], RD(ss_base + esp),
                   RD(ss_base + esp + 4), RD(ss_base + esp + 8), RD(ss_base + esp + 12));
            rt_log("[hb] fpu top=%d cw=%04x sw=%04x st0=%Lg st1=%Lg st7=%Lg crtc: 01=%02x 07=%02x 09=%02x 12=%02x 18=%02x",
                   ftop, fpu_cw, fpu_get_sw(), ST(0), ST(1), ST(7), crtc[1], crtc[7], crtc[9], crtc[0x12],
                   crtc[0x18]);
        }
    }
    if (flips != flips_presented) {
        uint64_t dl = host_ticks_us() - flip_us;
        flip_delay_sum += dl; flip_delay_n++;
        if (dl > flip_delay_max) flip_delay_max = dl;
    }
    flips_presented = flips;
    if (vga_planar) render_256(&w, &h);
    else render_text(&w, &h);
    if (!use_d3d) ensure_tex(w, h);
    last_w = w; last_h = h;
    if (!use_d3d) SDL_UpdateTexture(tex, NULL, pixels, w * 4);
    {   /* debug: TFX_SHOT=<path> saves the current frame every second */
        static const char *shot = (const char *)-1;
        static uint64_t last;
        if (shot == (const char *)-1) shot = getenv("TFX_SHOT");
        if (shot && host_ticks_us() - last > 1000000) {
            last = host_ticks_us();
            SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(pixels, w, h, 32, w * 4,
                                                                SDL_PIXELFORMAT_ARGB8888);
            if (s) { SDL_SaveBMP(s, shot); SDL_FreeSurface(s); }
        }
    }
    present_scaled(w, h);
}

/* ---- input ------------------------------------------------------------------------ */
int sdl_to_xt(SDL_Scancode s, int *ext) {
    *ext = 0;
    switch (s) {
    case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_1: return 0x02; case SDL_SCANCODE_2: return 0x03; case SDL_SCANCODE_3: return 0x04;
    case SDL_SCANCODE_4: return 0x05; case SDL_SCANCODE_5: return 0x06; case SDL_SCANCODE_6: return 0x07;
    case SDL_SCANCODE_7: return 0x08; case SDL_SCANCODE_8: return 0x09; case SDL_SCANCODE_9: return 0x0a;
    case SDL_SCANCODE_0: return 0x0b; case SDL_SCANCODE_MINUS: return 0x0c; case SDL_SCANCODE_EQUALS: return 0x0d;
    case SDL_SCANCODE_BACKSPACE: return 0x0e; case SDL_SCANCODE_TAB: return 0x0f;
    case SDL_SCANCODE_Q: return 0x10; case SDL_SCANCODE_W: return 0x11; case SDL_SCANCODE_E: return 0x12;
    case SDL_SCANCODE_R: return 0x13; case SDL_SCANCODE_T: return 0x14; case SDL_SCANCODE_Y: return 0x15;
    case SDL_SCANCODE_U: return 0x16; case SDL_SCANCODE_I: return 0x17; case SDL_SCANCODE_O: return 0x18;
    case SDL_SCANCODE_P: return 0x19; case SDL_SCANCODE_LEFTBRACKET: return 0x1a; case SDL_SCANCODE_RIGHTBRACKET: return 0x1b;
    case SDL_SCANCODE_RETURN: return 0x1c; case SDL_SCANCODE_LCTRL: return 0x1d;
    case SDL_SCANCODE_A: return 0x1e; case SDL_SCANCODE_S: return 0x1f; case SDL_SCANCODE_D: return 0x20;
    case SDL_SCANCODE_F: return 0x21; case SDL_SCANCODE_G: return 0x22; case SDL_SCANCODE_H: return 0x23;
    case SDL_SCANCODE_J: return 0x24; case SDL_SCANCODE_K: return 0x25; case SDL_SCANCODE_L: return 0x26;
    case SDL_SCANCODE_SEMICOLON: return 0x27; case SDL_SCANCODE_APOSTROPHE: return 0x28; case SDL_SCANCODE_GRAVE: return 0x29;
    case SDL_SCANCODE_LSHIFT: return 0x2a; case SDL_SCANCODE_BACKSLASH: return 0x2b;
    case SDL_SCANCODE_Z: return 0x2c; case SDL_SCANCODE_X: return 0x2d; case SDL_SCANCODE_C: return 0x2e;
    case SDL_SCANCODE_V: return 0x2f; case SDL_SCANCODE_B: return 0x30; case SDL_SCANCODE_N: return 0x31;
    case SDL_SCANCODE_M: return 0x32; case SDL_SCANCODE_COMMA: return 0x33; case SDL_SCANCODE_PERIOD: return 0x34;
    case SDL_SCANCODE_SLASH: return 0x35; case SDL_SCANCODE_RSHIFT: return 0x36; case SDL_SCANCODE_KP_MULTIPLY: return 0x37;
    case SDL_SCANCODE_LALT: return 0x38; case SDL_SCANCODE_SPACE: return 0x39; case SDL_SCANCODE_CAPSLOCK: return 0x3a;
    case SDL_SCANCODE_F1: return 0x3b; case SDL_SCANCODE_F2: return 0x3c; case SDL_SCANCODE_F3: return 0x3d;
    case SDL_SCANCODE_F4: return 0x3e; case SDL_SCANCODE_F5: return 0x3f; case SDL_SCANCODE_F6: return 0x40;
    case SDL_SCANCODE_F7: return 0x41; case SDL_SCANCODE_F8: return 0x42; case SDL_SCANCODE_F9: return 0x43;
    case SDL_SCANCODE_F10: return 0x44; case SDL_SCANCODE_NUMLOCKCLEAR: return 0x45; case SDL_SCANCODE_SCROLLLOCK: return 0x46;
    case SDL_SCANCODE_KP_7: return 0x47; case SDL_SCANCODE_KP_8: return 0x48; case SDL_SCANCODE_KP_9: return 0x49;
    case SDL_SCANCODE_KP_MINUS: return 0x4a; case SDL_SCANCODE_KP_4: return 0x4b; case SDL_SCANCODE_KP_5: return 0x4c;
    case SDL_SCANCODE_KP_6: return 0x4d; case SDL_SCANCODE_KP_PLUS: return 0x4e; case SDL_SCANCODE_KP_1: return 0x4f;
    case SDL_SCANCODE_KP_2: return 0x50; case SDL_SCANCODE_KP_3: return 0x51; case SDL_SCANCODE_KP_0: return 0x52;
    case SDL_SCANCODE_KP_PERIOD: return 0x53; case SDL_SCANCODE_F11: return 0x57; case SDL_SCANCODE_F12: return 0x58;
    case SDL_SCANCODE_KP_ENTER: *ext = 1; return 0x1c; case SDL_SCANCODE_RCTRL: *ext = 1; return 0x1d;
    case SDL_SCANCODE_KP_DIVIDE: *ext = 1; return 0x35; case SDL_SCANCODE_RALT: *ext = 1; return 0x38;
    case SDL_SCANCODE_HOME: *ext = 1; return 0x47; case SDL_SCANCODE_UP: *ext = 1; return 0x48;
    case SDL_SCANCODE_PAGEUP: *ext = 1; return 0x49; case SDL_SCANCODE_LEFT: *ext = 1; return 0x4b;
    case SDL_SCANCODE_RIGHT: *ext = 1; return 0x4d; case SDL_SCANCODE_END: *ext = 1; return 0x4f;
    case SDL_SCANCODE_DOWN: *ext = 1; return 0x50; case SDL_SCANCODE_PAGEDOWN: *ext = 1; return 0x51;
    case SDL_SCANCODE_INSERT: *ext = 1; return 0x52; case SDL_SCANCODE_DELETE: *ext = 1; return 0x53;
    default: return 0;
    }
}

/* Debug/test aid: TFX_SCRIPT="ms action args;..." replays input.
 * ms is the delay after the previous step.
 *   w         wait until no file has been opened for 1.5 s (loading done)
 *   m X Y B   mouse at X,Y (0..1000 of the screen), buttons B (1=left 2=right)
 *   k SC      press and release XT scancode SC (hex)
 *   s PATH    save a screenshot (BMP) */
/* Live control for testing: TFX_CMDFILE=<path>.  Lines appended to the file
 * are executed as soon as they appear (same actions as TFX_SCRIPT, without
 * the delay field): "k 1c", "r 10 0 1", "s shot.bmp", "q" (quit). */
/* Log every code byte that differs from the loaded image as 'smc ADDR HEX'
 * (32 bytes per line); tools/smc_merge.py turns these into the analyzer's
 * runtime-observed self-modification list. */
/* Command file "P SECONDS": a sampling profile of the game thread.  A second
 * thread reads, about 1000 times a second, the game function entered last
 * (the function trace) and the last recorded instruction address (rt_eip,
 * set before port accesses, memory writes that may hit the VGA and single
 * steps), and logs the commonest of each.  Host code (the runtime's own
 * work) counts for the game code that called it. */
static int prof_ms;
static int prof_thread(void *unused) {
    extern uint32_t ftrace_ring[4096]; extern unsigned ftrace_pos;
    static struct { uint32_t a; unsigned n; } fn[512], ip[512];
    int nf = 0, ni = 0, total = 0;
    (void)unused;
    memset(fn, 0, sizeof fn); memset(ip, 0, sizeof ip);
    Uint32 end = SDL_GetTicks() + (Uint32)prof_ms;
    while (SDL_GetTicks() < end) {
        uint32_t f = ftrace_ring[(ftrace_pos - 1) & 4095], e = rt_eip & ~15u;
        int k;
        for (k = 0; k < nf && fn[k].a != f; k++) ;
        if (k == nf && nf < 512) fn[nf++].a = f;
        if (k < 512) fn[k].n++;
        for (k = 0; k < ni && ip[k].a != e; k++) ;
        if (k == ni && ni < 512) ip[ni++].a = e;
        if (k < 512) ip[k].n++;
        total++;
        SDL_Delay(1);
    }
    for (int pass = 0; pass < 2; pass++) {
        int n = pass ? ni : nf;
        rt_log("profile: %d samples, by %s:", total, pass ? "last instruction address (16-byte buckets)" : "function entered last");
        for (int r = 0; r < 14; r++) {
            int best = -1;
            for (int k = 0; k < n; k++) { unsigned v = pass ? ip[k].n : fn[k].n; if (v && (best < 0 || v > (pass ? ip[best].n : fn[best].n))) best = k; }
            if (best < 0) break;
            rt_log("profile:   %08x  %5.1f%%", pass ? ip[best].a : fn[best].a, 100.0 * (pass ? ip[best].n : fn[best].n) / total);
            if (pass) ip[best].n = 0; else fn[best].n = 0;
        }
    }
    prof_ms = 0;
    return 0;
}

void rt_dump_smc(void) {
    extern uint8_t *code_pristine; extern uint32_t code_lo, code_hi;
    int runs = 0;
    for (uint32_t a = code_lo; code_pristine && a < code_hi; a++) {
        if (MEM[a] == code_pristine[a - code_lo]) continue;
        uint32_t b = a;
        while (b < code_hi && MEM[b] != code_pristine[b - code_lo]) b++;
        for (uint32_t c = a; c < b; c += 32) {   /* one line per 32 bytes */
            char hex[2 * 32 + 1];
            uint32_t n = b - c < 32 ? b - c : 32;
            for (uint32_t k = 0; k < n; k++) snprintf(hex + 2 * k, 3, "%02x", MEM[c + k]);
            rt_log("smc %08x %s", c, hex);
        }
        runs++; a = b;
    }
    rt_log("smc: %d changed runs", runs);
}

static void run_cmdfile(void) {
    static const char *path = (const char *)-1;
    static long off;
    static uint64_t last;
    if (path == (const char *)-1) path = getenv("TFX_CMDFILE");
    if (!path || host_ticks_us() - last < 100000) return;
    last = host_ticks_us();
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, off, SEEK_SET);
    char line[300];
    while (fgets(line, sizeof line, f)) {
        if (!strchr(line, '\n')) break;                 /* incomplete line: retry later */
        off = ftell(f);
        char *p = line;
        while (*p == ' ') p++;
        char act = *p++;
        if (act == 'r') {
            extern void mouse_relative(int, int, int);
            int dx = (int)strtol(p, &p, 10), dy = (int)strtol(p, &p, 10), b = (int)strtol(p, &p, 10);
            mouse_relative(dx, dy, b);
        } else if (act == 'k' || act == 'd' || act == 'u') {
            int sc = (int)strtol(p, &p, 16);
            if (act != 'u') kbd_push_scancode((uint8_t)sc);            /* d = key down only */
            if (act != 'd') kbd_push_scancode((uint8_t)(sc | 0x80));   /* u = key up only */
        } else if (act == 's') {
            while (*p == ' ') p++;
            char *e = p + strlen(p);
            while (e > p && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
            extern void video_save_shot(const char *);
            video_save_shot(p);
        } else if (act == 't') {          /* t LO HI: log every interpreted instruction in [LO,HI) */
            extern uint32_t itrace_lo, itrace_hi;
            itrace_lo = (uint32_t)strtoul(p, &p, 16); itrace_hi = (uint32_t)strtoul(p, &p, 16);
        } else if (act == 'i') {          /* i lo-hi[,lo-hi]: interpret these code ranges (i alone: none) */
            extern void rt_interp_set(const char *); while (*p == ' ') p++;
            char *e2 = p + strlen(p); while (e2 > p && (e2[-1] == 10 || e2[-1] == 13)) *--e2 = 0;
            rt_interp_set(*p ? p : NULL);
        } else if (act == 'b') {          /* b ADDR VAL: write a byte of guest memory */
            uint32_t a = (uint32_t)strtoul(p, &p, 16); uint32_t v = (uint32_t)strtoul(p, &p, 16);
            if (a < MEM_SIZE) { WB(a, (uint8_t)v); rt_log("poke %08x = %02x", a, v & 0xff); }
        } else if (act == 'p') {          /* p ADDR [n]: log n dwords of guest memory */
            uint32_t a = (uint32_t)strtoul(p, &p, 16); int n = (int)strtol(p, &p, 10); if (n <= 0) n = 1;
            for (int i = 0; i < n && a + 4 * i + 4 <= MEM_SIZE; i++) rt_log("peek %08x = %08x", a + 4 * i, RD(a + 4 * i));
        } else if (act == 'v') {         /* v X Y: displayed pixel index and DAC colour */
            int x = (int)strtol(p, &p, 10), y = (int)strtol(p, &p, 10);
            uint32_t start = ((uint32_t)crtc[0x0c] << 8) | crtc[0x0d], pitch = (uint32_t)crtc[0x13] * 2;
            uint8_t ix = planes[x & 3][(start + y * pitch + (x >> 2)) & 0xffff];
            rt_log("pixel %d,%d = %d rgb %d,%d,%d (start %04x)", x, y, ix, vga_dac[ix][0], vga_dac[ix][1], vga_dac[ix][2], start);
            for (int pg = 0; pg < 4; pg++) rt_log("  page @%04x: %d", pg * 0x3e80, planes[x & 3][(pg * 0x3e80 + y * pitch + (x >> 2)) & 0xffff]);
        } else if (act == 'g') {         /* VGA write profile by last eip: g = dump and reset */
            for (int i = 0; i < 4096; i++) if (vga_prof[i].n) rt_log("vgaw %08x %u", vga_prof[i].eip, vga_prof[i].n);
            memset(vga_prof, 0, sizeof vga_prof);
        } else if (act == 'c') {         /* code bytes changed since load (self-modification) */
            rt_dump_smc();
        } else if (act == 'f') {         /* frame rate since the previous `f` */
            static uint32_t f0; static uint64_t t0;
            uint64_t t = SDL_GetTicks64();
            if (t0) rt_log("fps: %u flips in %llu ms = %.2f fps; flip to present %.1f ms mean, %.1f ms max", flips - f0,
                           (unsigned long long)(t - t0), (flips - f0) * 1000.0 / (double)(t - t0 ? t - t0 : 1),
                           flip_delay_n ? flip_delay_sum / 1000.0 / (double)flip_delay_n : 0.0, flip_delay_max / 1000.0);
            flip_delay_sum = flip_delay_max = flip_delay_n = 0;
            f0 = flips; t0 = t;
        } else if (act == 'm') {         /* int 33h calls by function since the last `m` */
            extern unsigned mouse_fn_count[64]; extern int mouse_x, mouse_y, mouse_buttons;
            for (int i = 0; i < 64; i++) if (mouse_fn_count[i]) rt_log("int33 fn %02x: %u", i, mouse_fn_count[i]);
            rt_log("mouse at %d,%d buttons %d", mouse_x, mouse_y, mouse_buttons);
            memset(mouse_fn_count, 0, sizeof mouse_fn_count);
        } else if (act == 'O') {         /* O key=value: change a display setting live */
            char kv[128], *eq;
            while (*p == ' ') p++;
            snprintf(kv, sizeof kv, "%s", p);
            for (char *q = kv; *q; q++) if (*q == '\n' || *q == '\r') *q = 0;
            if ((eq = strchr(kv, '=')) != NULL) {
                *eq = 0;
                settings_set(kv, eq + 1);
                if (!_stricmp(kv, "window")) SDL_SetWindowSize(win, settings.window_w, settings.window_h);
                if (!_stricmp(kv, "fullscreen")) SDL_SetWindowFullscreen(win, settings.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
            }
        } else if (act == 'P') {         /* P SECONDS: sampling profile of the game thread */
            if (!prof_ms) { prof_ms = (int)(strtod(p, &p) * 1000); if (prof_ms < 500) prof_ms = 3000; SDL_DetachThread(SDL_CreateThread(prof_thread, "profile", NULL)); }
        } else if (act == 'J') {         /* J X Y RUDDER THROTTLE BUTTONS HAT | J off: a virtual joystick (joy.c) */
            joy_virtual(p);
        } else if (act == 'S') {         /* native renderer capture statistics */
            { extern int native_probe; native_probe = 1; }
            scene_log_stats();
            { extern void rt_wide_cull_log(void); rt_wide_cull_log(); }
            joy_log();
            { extern void rt_head_track_log(void); rt_head_track_log(); }
            { extern void rt_div_recovered_log(void); rt_div_recovered_log(); }
            rt_log("native: %d primitives drawn (%d triangles; %d Gouraud, %d textured, %d discs, %d lines (%d through 0x454336), %d points), %d screen-space rebuilt, %d back faces culled, %d clipped copies skipped, %d over the texture/table limit; orientation agree %d / disagree %d",
                   native_last.drawn, native_last.triangles, native_last.gouraud, native_last.textured, native_last.discs, native_last.lines, native_last.lines_454336, native_last.points, native_last.screen, native_last.culled,
                   native_last.skipped_derived, native_last.tex_overflow, native_last.orient_agree, native_last.orient_disagree);
            rt_log("native: motion over %d scene frame(s): %d primitives matched (%d among instances), %d new; on-screen vertex-0 motion mean %.2f px, max %.1f px",
                   native_last.history_frames, native_last.matched, native_last.instanced, native_last.unmatched,
                   native_last.mv_mean, native_last.mv_max);
            rt_log("native: motion px buckets <2/<10/<50/>=50: unique %d %d %d %d, instanced %d %d %d %d; %d unmatched moved with the camera (%d instance matches rejected)",
                   native_last.mv_hist[0][0], native_last.mv_hist[0][1], native_last.mv_hist[0][2], native_last.mv_hist[0][3],
                   native_last.mv_hist[1][0], native_last.mv_hist[1][1], native_last.mv_hist[1][2], native_last.mv_hist[1][3],
                   native_last.from_camera, native_last.instance_rejected);
            rt_log("native: camera delta %s (%d rigid group(s)): %d of %d vertex pairs inliers, rotation %.3f deg, move %.2f, residual rms %.3f (game units)",
                   native_last.cam_valid ? "fitted" : "not fitted", native_last.cam_groups, native_last.cam_inliers, native_last.cam_pairs,
                   native_last.cam_angle, native_last.cam_move, native_last.cam_rms);
            native_log_rejects();
        } else if (act == 'w') {         /* w X Y: the code that last wrote this displayed pixel */
            int x = (int)strtol(p, &p, 10), y = (int)strtol(p, &p, 10);
            uint32_t start = ((uint32_t)crtc[0x0c] << 8) | crtc[0x0d], pitch = (uint32_t)crtc[0x13] * 2;
            uint32_t o = (start + (uint32_t)y * pitch + (uint32_t)(x >> 2)) & 0xffff;
            rt_log("writer %d,%d = %08x (owner %d, index %d)", x, y, last_eip[x & 3][o], owner[x & 3][o], planes[x & 3][o]);
        } else if (act == 'U') {         /* U [Y0 Y1]: the displayed page's UI pixels (owner 0) by writer, rows Y0..Y1 */
            int y0 = (int)strtol(p, &p, 10), y1 = (int)strtol(p, &p, 10);
            if (y1 <= 0 || y1 > 199) y1 = 199;
            uint32_t start = ((uint32_t)crtc[0x0c] << 8) | crtc[0x0d], pitch = (uint32_t)crtc[0x13] * 2;
            struct { uint32_t eip, n; int x, y; } h[256]; int nh = 0;
            for (int y = y0; y <= y1; y++)
                for (int x = 0; x < 320; x++) {
                    uint32_t o = (start + (uint32_t)y * pitch + (uint32_t)(x >> 2)) & 0xffff;
                    if (owner[x & 3][o]) continue;
                    uint32_t e = last_eip[x & 3][o]; int k;
                    if (e < 0x400000u || e >= 0x480000u) e = 0;   /* host code (e.g. block copies) */
                    for (k = 0; k < nh && h[k].eip != e; k++) ;
                    if (k == nh) { if (nh == 256) continue; h[nh].eip = e; h[nh].n = 0; h[nh].x = x; h[nh].y = y; nh++; }
                    h[k].n++;
                }
            for (int k = 0; k < nh; k++) rt_log("ui writer %08x: %u pixels (e.g. %d,%d)", h[k].eip, h[k].n, h[k].x, h[k].y);
        } else if (act == 'A') {         /* A DIR: dump the cockpit template and the next frame's sprites */
            while (*p == ' ') p++;
            snprintf(asset_dir, sizeof asset_dir, "%s", p);
            char *e = asset_dir + strlen(asset_dir);
            while (e > asset_dir && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
            _mkdir(asset_dir);
            uint16_t *img = malloc(320 * 200 * sizeof *img);
            if (img) {      /* the template Mode X keeps at BB80h, copied onto each frame with write mode 1 */
                for (int y = 0; y < 200; y++)
                    for (int x = 0; x < 320; x++) img[y * 320 + x] = planes[x & 3][(0xbb80u + y * 80u + (x >> 2)) & 0xffff];
                char path[300]; snprintf(path, sizeof path, "%s\\cockpit_template.bmp", asset_dir);
                vga_save_indexed(path, 320, 200, img);
                snprintf(path, sizeof path, "%s\\manifest.txt", asset_dir);
                FILE *mf = fopen(path, "a");
                if (mf) { fprintf(mf, "cockpit_template.bmp template rows 55 hash %08x\n", art_hash(img, 320, 55)); fclose(mf); }
                free(img);
            }
            scene_asset_dump = 1;
            rt_log("assets: template saved, sprites of the next frame go to %s", asset_dir);
        } else if (act == 'D') {         /* D: dump the last captured frame's primitives */
            scene_dump();
        } else if (act == 'W') {         /* W FILE: save the presented window (after scaling) */
            while (*p == ' ') p++;
            snprintf(window_shot, sizeof window_shot, "%s", p);
            char *e = window_shot + strlen(window_shot);
            while (e > window_shot && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        } else if (act == 'z') {         /* zero the call histogram */
            memset(fcount, 0, sizeof fcount);
        } else if (act == 'h') {         /* dump the call histogram (address approx. to 4 bytes) */
            for (int i = 0; i < (1 << 20); i++) if (fcount[i]) rt_log("hist %08x %u", 0x400000u + ((uint32_t)i << 2), fcount[i]);
        } else if (act == 'q') {
            rt_log("cmdfile: quit"); rt_dump_ftrace(4096);
            exit(0);
        }
        rt_log("cmdfile: %s", line);
    }
    fclose(f);
}

static void run_script(void) {
    run_cmdfile();
    static char *script;
    static int init, pos;
    static uint64_t t0, last_ms;
    if (!init) {
        init = 1;
        const char *s = getenv("TFX_SCRIPT");
        if (s) script = SDL_strdup(s);
        t0 = host_ticks_us();
    }
    if (!script || !script[pos]) return;
    uint64_t now_ms = (host_ticks_us() - t0) / 1000;
    for (;;) {
        char *p = script + pos;
        while (*p == ';' || *p == ' ') p++;
        if (!*p) { pos = (int)(p - script); return; }
        char *entry = p;
        unsigned long when = strtoul(p, &p, 10);       /* delay after previous step */
        if (now_ms < last_ms + when) { pos = (int)(entry - script); return; }
        while (*p == ' ') p++;
        char act = *p++;
        if (act == 'w') {
            /* wait until the program has not opened a file for 1.5 s */
            extern uint64_t dos_last_open_us;
            if (host_ticks_us() - dos_last_open_us < 1500000) { pos = (int)(entry - script); return; }
        } else if (act == 'r') {
            extern void mouse_relative(int, int, int);
            int dx = (int)strtol(p, &p, 10), dy = (int)strtol(p, &p, 10), b = (int)strtol(p, &p, 10);
            mouse_relative(dx, dy, b);
        } else if (act == 'm') {
            int x = (int)strtol(p, &p, 10), y = (int)strtol(p, &p, 10), b = (int)strtol(p, &p, 10);
            mouse_update(x, y, b);
        } else if (act == 'k') {
            int sc = (int)strtol(p, &p, 16);
            kbd_push_scancode((uint8_t)sc);
            kbd_push_scancode((uint8_t)(sc | 0x80));
        } else if (act == 's') {
            while (*p == ' ') p++;
            char path[260]; int n = 0;
            while (*p && *p != ';' && n < 259) path[n++] = *p++;
            path[n] = 0;
            extern void video_save_shot(const char *);
            video_save_shot(path);
        }
        while (*p && *p != ';') p++;
        pos = (int)(p - script);
        last_ms = (host_ticks_us() - t0) / 1000;
        rt_log("script: t=%llu executed '%c'", (unsigned long long)now_ms, act);
    }
}


void video_save_shot(const char *path) {
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(pixels, last_w, last_h, 32, last_w * 4,
                                                        SDL_PIXELFORMAT_ARGB8888);
    if (s) { SDL_SaveBMP(s, path); SDL_FreeSurface(s); }
}

/* Mouse capture: DOS programs read relative motion (mickeys), so the host
 * mouse is captured (relative mode) whenever the window is active and released
 * when it is not.  Ctrl+F10 toggles it while active (e.g. to move the window);
 * a click captures again.  Scripted test runs never capture automatically. */
static int mouse_auto(void) { return !getenv("TFX_CMDFILE") && !getenv("TFX_SCRIPT"); }
static void set_capture(int on) {
    SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
    SDL_SetWindowTitle(win, on ? "TFX  (Ctrl+F10 frees the mouse)" : "TFX  (click to capture the mouse)");
}

void video_pump(void) {
    SDL_Event ev;
    run_script();
    while (SDL_PollEvent(&ev)) {
        { extern int joy_event(const SDL_Event *); joy_event(&ev); }
        switch (ev.type) {
        case SDL_QUIT:
            rt_log("window closed");
            exit(0);
        case SDL_KEYDOWN: case SDL_KEYUP: {
            if (ev.key.repeat) break;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_F10 && (ev.key.keysym.mod & KMOD_CTRL)) {
                set_capture(!SDL_GetRelativeMouseMode());
                break;
            }
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_RETURN &&
                (ev.key.keysym.mod & KMOD_ALT)) {
                Uint32 f = SDL_GetWindowFlags(win);
                SDL_SetWindowFullscreen(win, (f & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                break;
            }
            int ext, sc = sdl_to_xt(ev.key.keysym.scancode, &ext);
            if (!sc) break;
            if (ext) kbd_push_scancode(0xe0);
            kbd_push_scancode((uint8_t)(sc | (ev.type == SDL_KEYUP ? 0x80 : 0)));
            break;
        }
        case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
            static double fx, fy;
            int w, h;
            SDL_GetWindowSize(win, &w, &h);
            if (!SDL_GetRelativeMouseMode()) {
                if (ev.type == SDL_MOUSEBUTTONDOWN) set_capture(1);
                break;
            }
            Uint32 b = SDL_GetRelativeMouseState(NULL, NULL);
            b = SDL_GetMouseState(NULL, NULL);
            int btn = ((b & SDL_BUTTON_LMASK) ? 1 : 0) | ((b & SDL_BUTTON_RMASK) ? 2 : 0) |
                      ((b & SDL_BUTTON_MMASK) ? 4 : 0);
            if (ev.type == SDL_MOUSEMOTION && w && h) {
                /* one window width = 320 mickeys */
                fx += ev.motion.xrel * 320.0 / w;
                fy += ev.motion.yrel * 200.0 / h;
            }
            int dx = (int)fx, dy = (int)fy;
            fx -= dx; fy -= dy;
            mouse_relative(dx, dy, btn);
            break;
        }
        case SDL_WINDOWEVENT:
            if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) set_capture(0);
            else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED && mouse_auto()) set_capture(1);
            break;
        }
    }
    joy_poll();
}
