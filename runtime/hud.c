/* Sharp HUD: the game's 2D overlay drawn again at the output resolution.
 *
 * scene.c captures every call of the HUD line rasteriser (0x454212), the
 * 1-bit bitmap blit (0x45CFA4: text, the gun ring, ladder, symbols) and the
 * pixel plot (0x45658C) made while a frame is drawn (TFX_CODE.md 9.17).  Their pixels are owned by the "HUD overlay" in vga.c,
 * so the masked UI pass leaves them out and the overlay pass (d3d12.c) draws
 * these instead:
 *  - a line as a quad 0.7 game pixels wide between the centres of its end
 *    pixels, with anti-aliased sides and ends (coverage from the distances
 *    passed in u/v);
 *  - a bitmap (or dot) from an R8 atlas built here each frame (identical
 *    bitmaps shared, 1-texel gaps), turned into strokes by the pixel shader:
 *    each set pixel is a point joined to its set neighbours, drawn half a
 *    game pixel either side, so one-pixel pixel-art lines (the gun ring, the
 *    ladder, glyph strokes) become smooth lines of the same weight.
 * The pass discards where the game's final pixel is UI, so anything the game
 * drew on top later (cockpit art) still covers it. */
#include <math.h>
#include <string.h>
#include "rt.h"
#include "scene.h"
#include "hud.h"

uint32_t vga_palette_argb(uint8_t idx);

#define MAX_HUD_VERTS (6 * 4096)
static struct hud_vertex vb[MAX_HUD_VERTS];
static uint8_t atlas[HUD_ATLAS * HUD_ATLAS];

/* the atlas: shelves left to right, top to bottom */
static int shelf_x, shelf_y, shelf_h;
#define SLOTS 512
static struct { uint32_t hash; uint16_t x, y; uint8_t w, h; } slot[SLOTS];
static int nslots;

static uint32_t bits_hash(const uint8_t *b, int n, int w, int h) {
    uint32_t x = 2166136261u ^ (uint32_t)(w << 8 | h);
    for (int i = 0; i < n; i++) x = (x ^ b[i]) * 16777619u;
    return x;
}

/* place a bitmap in the atlas (or find it there): top-left texel, -1 if full */
static int place(const uint8_t *bits, int w, int h, int *ax, int *ay) {
    int stride = (w + 7) / 8;
    uint32_t hs = bits_hash(bits, stride * h, w, h);
    for (int i = 0; i < nslots; i++)
        if (slot[i].hash == hs && slot[i].w == w && slot[i].h == h) { *ax = slot[i].x; *ay = slot[i].y; return 0; }
    if (nslots >= SLOTS || w + 2 > HUD_ATLAS || h + 2 > HUD_ATLAS) return -1;
    if (shelf_x + w + 2 > HUD_ATLAS) { shelf_x = 0; shelf_y += shelf_h; shelf_h = 0; }
    if (shelf_y + h + 2 > HUD_ATLAS) return -1;
    int x0 = shelf_x + 1, y0 = shelf_y + 1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            atlas[(y0 + y) * HUD_ATLAS + x0 + x] = (bits[y * stride + (x >> 3)] & (0x80 >> (x & 7))) ? 255 : 0;
    slot[nslots].hash = hs; slot[nslots].x = (uint16_t)x0; slot[nslots].y = (uint16_t)y0;
    slot[nslots].w = (uint8_t)w; slot[nslots].h = (uint8_t)h; nslots++;
    shelf_x += w + 2;
    if (h + 2 > shelf_h) shelf_h = h + 2;
    *ax = x0; *ay = y0;
    return 0;
}

int hud_build(const struct scene_frame *f, int dx, int dy, int dw, int dh, int ow, int oh,
              const struct hud_vertex **verts, const uint8_t **out_atlas) {
    *verts = vb; *out_atlas = atlas;
    if (!f || !f->nhud) return 0;
    memset(atlas, 0, sizeof atlas);
    nslots = 0; shelf_x = shelf_y = shelf_h = 0;
    float kx = (float)dw / 320.0f, ky = (float)dh / 200.0f;
    float s = 0.5f * (kx + ky);
    int n = 0;
    static int full_logged;
    for (int i = 0; i < f->nhud && n + 6 <= MAX_HUD_VERTS; i++) {
        const struct scene_hud *h = &f->hud[i];
        uint32_t col = vga_palette_argb(h->color) | 0xff000000u;
        struct hud_vertex q[4];
        memset(q, 0, sizeof q);
        if (h->kind == HUD_LINE) {
            /* end-pixel centres in output pixels */
            float x0 = (float)dx + ((float)h->x0 + 0.5f) * kx, y0 = (float)dy + ((float)h->y0 + 0.5f) * ky;
            float x1 = (float)dx + ((float)h->x1 + 0.5f) * kx, y1 = (float)dy + ((float)h->y1 + 0.5f) * ky;
            float ex = x1 - x0, ey = y1 - y0, len = sqrtf(ex * ex + ey * ey);
            float ux = 1, uy = 0;
            if (len > 1e-3f) { ux = ex / len; uy = ey / len; }
            float nx = -uy, ny = ux;
            float hw = 0.35f * s;                          /* half width: 0.7 game pixels in all */
            if (hw < 0.75f) hw = 0.75f;
            float m = hw + 1.0f;                           /* quad margin for the anti-aliasing */
            float px[4] = {x0 - ux * m + nx * m, x1 + ux * m + nx * m, x1 + ux * m - nx * m, x0 - ux * m - nx * m};
            float py[4] = {y0 - uy * m + ny * m, y1 + uy * m + ny * m, y1 + uy * m - ny * m, y0 - uy * m - ny * m};
            float pu[4] = {m, m, -m, -m}, pv[4] = {-m, len + m, len + m, -m};
            for (int k = 0; k < 4; k++) {
                q[k].x = 2.0f * px[k] / (float)ow - 1.0f; q[k].y = 1.0f - 2.0f * py[k] / (float)oh;
                q[k].u = pu[k]; q[k].v = pv[k]; q[k].a = hw; q[k].b = len; q[k].argb = col; q[k].mode = 0;
            }
        } else {
            int ax, ay;
            if (place(f->hud_bits + h->bits, h->w, h->h, &ax, &ay)) {
                if (!full_logged++) rt_log("hud: bitmap atlas full; some HUD bitmaps not drawn sharp");
                continue;
            }
            /* the bitmap's rectangle, clipped as the blit clips it: x in [cl, cr), y in [ct, cb) */
            float gx0 = h->x0, gy0 = h->y0, gx1 = (float)h->x0 + h->w, gy1 = (float)h->y0 + h->h;
            float cx0 = fmaxf(gx0, (float)h->cl), cx1 = fminf(gx1, (float)h->cr);
            float cy0 = fmaxf(gy0, (float)h->ct), cy1 = fminf(gy1, (float)h->cb);
            if (cx1 <= cx0 || cy1 <= cy0) continue;
            float gx[4] = {cx0, cx1, cx1, cx0}, gy[4] = {cy0, cy0, cy1, cy1};
            for (int k = 0; k < 4; k++) {
                float ox = (float)dx + gx[k] * kx, oy = (float)dy + gy[k] * ky;
                q[k].x = 2.0f * ox / (float)ow - 1.0f; q[k].y = 1.0f - 2.0f * oy / (float)oh;
                q[k].u = (float)ax + (gx[k] - gx0); q[k].v = (float)ay + (gy[k] - gy0);
                q[k].argb = col; q[k].mode = 1;
            }
        }
        vb[n++] = q[0]; vb[n++] = q[1]; vb[n++] = q[2];
        vb[n++] = q[0]; vb[n++] = q[2]; vb[n++] = q[3];
    }
    return n;
}
