/* Native renderer, stage 1: capture the 3D scene each frame.
 *
 * TFX's 3D bytecode VM transforms each vertex into a 20h-byte record
 * (TFX_CODE.md 9.11): +0/+4/+8 view-space X/Y/Z, +0Ch near-plane flag,
 * +0Eh/+10h the projected screen point (x = 256*X/Z + cx, y = 256*Y/Z + cy,
 * done by 0x448990), +14h.. rotated local coordinates.  Polygon primitives
 * get ESI = list of record offsets (base 0x46814C) and ECX = vertex count.
 * The recompiler calls scene_prim() at the entry of each primitive routine
 * (tools/recomp.py ENTRY_HOOKS); polygons are stored per frame, and the frame
 * ends at the game's page flip (vga.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "art.h"
#include "scene.h"

#define MAX_PRIMS 16384
#define MAX_VERTS 131072
#define VREC_BASE 0x46814Cu        /* record of vertex offset i: VREC_BASE + i */
#define MAX_HUD 4096
#define MAX_HUD_BITS 65536

static struct scene_frame frames[2];
static int cur;
static uint32_t frame_no;
int scene_bg_draw, scene_line_draw, scene_point_draw, scene_hud_draw, scene_line2_draw;
/* a captured disc is being filled: 1 + index of its fill style in
 * disc_fills[] (TFX_CODE.md 9.18); its pixels and the disc areas are counted
 * per style (cmdfile S) to measure each style's coverage */
int scene_disc_draw;
/* sprites (TFX_CODE.md 9.19): the 8-bit sprite blit 0x450454(y, x, index,
 * bank) is captured by the pixels it writes (vga.c calls scene_sprite_pixel
 * while scene_sprite_draw is set).  When the next primitive starts or the
 * frame ends, a sprite in one colour (HUD brackets, markers, the heading box)
 * becomes a 1-bit HUD bitmap, drawn sharp like the others; any other sprite
 * (cockpit art, the MFD pages, shaded panel digits) gives its pixels back to
 * the UI */
int scene_sprite_draw;
int scene_asset_dump;
uint32_t scene_sprite_seq;
void vga_prov_resolve(int p, uint16_t off, uint32_t pending, uint32_t tag);
extern char asset_dir[260];
void vga_save_indexed(const char *path, int w, int h, const uint16_t *idx);
#define MAX_SPRITE_PX 4096
static struct { uint8_t plane, val; uint16_t off; } spx[MAX_SPRITE_PX];
static int nspx, spx_over;
static uint32_t sprite_ret, sprite_args[4], sprite_page;
static int sprite_log;                 /* cmdfile S: log the sprites of the next frame */
void vga_owner_to_ui(int plane, uint16_t off);
unsigned scene_fill_px[10];
static double fill_area[10];
static const uint32_t disc_fills[9] = {0x472560, 0x4726ce, 0x4727ff, 0x47292f, 0x472c36, 0x472f60, 0x473acd, 0x47415a, 0x47424c};
static uint32_t near_list = ~0u;    /* list of the last near-plane primitive (see scene_prim) */
static int near_index = -1;

/* attribute-mapping evidence (scene_check) */
#define UV_MODES 4
#define SHADE_MODES 3
static unsigned uv_hits[UV_MODES], uv_tests, shade_hits[SHADE_MODES], shade_tests;
static int uv_mode = 1, shade_mode;   /* uv 1: row = high word (span emitter 0x46B6EC packs it first) */
static uint32_t fills_seen[2][8];
static unsigned fills_count[2][8];

static void ensure(struct scene_frame *f) {
    if (!f->prims) {
        f->prims = calloc(MAX_PRIMS, sizeof *f->prims);
        f->verts = calloc(MAX_VERTS, sizeof *f->verts);
        f->hud = calloc(MAX_HUD, sizeof *f->hud);
        f->hud_bits = calloc(MAX_HUD_BITS, 1);
    }
}

static int kind_of(uint32_t entry) {
    switch (entry) {
    case 0x44c904: return PRIM_FLAT;
    case 0x44ceb8: return PRIM_GOURAUD;
    case 0x44d194: return PRIM_TEXTURED;
    case 0x44cbe8: return PRIM_EFFECT;
    case 0x466ef4: return PRIM_FLAT_NEAR;
    case 0x4675d0: return PRIM_GOURAUD_NEAR;
    case 0x467b84: return PRIM_TEXTURED_NEAR;
    case 0x44f26a: return PRIM_DISC;
    case 0x44ea99: return PRIM_LINE;
    }
    return PRIM_FLAT;
}

/* filled disc 0x44F26A(x, y, r) (cdecl words; r in game pixels, spans from
 * the circle table at 0x465A84, colour [0x447CA8]).  The sprite handler
 * 0x44B48B calls it (return 0x44B4C2) with EBX = the vertex record and the
 * world radius just read from the bytecode: r = (radius << 7) / (Z >> 16). */
static void scene_disc(struct scene_frame *f) {
    uint32_t sp = ss_base + esp;
    int16_t x = (int16_t)RW(sp + 4), y = (int16_t)RW(sp + 6), r = (int16_t)RW(sp + 8);
    /* the routine itself draws nothing for r < 1 or r >= [0x466EA0] (100):
     * cloud puffs close to the camera are skipped (TFX_CODE.md 9.15) */
    if (r < 1 || r >= (int16_t)RW(0x466ea0) || f->nprims >= MAX_PRIMS || f->nverts + 1 > MAX_VERTS) return;
    {
        uint32_t fill = RD(0x447cea); int k;
        for (k = 0; k < 9 && disc_fills[k] != fill; k++) ;
        scene_disc_draw = 1 + k;
        if (x - r >= (int16_t)RW(0x44c9b9) && x + r <= (int16_t)RW(0x44c9c8) &&
            y - r >= (int16_t)RW(0x44c99b) && y + r <= (int16_t)RW(0x44c9aa)) fill_area[k] += 3.14159265 * r * r;
        else scene_disc_draw = 0;                  /* clipped: counted as 3D below, but not measured */
        scene_disc_draw = scene_disc_draw ? scene_disc_draw : 1 + k + 16;
    }
    struct scene_prim *p = &f->prims[f->nprims++];
    memset(p, 0, sizeof *p);
    p->kind = PRIM_DISC;
    p->key = (ds_base + esi) | 0x80000000u;
    p->color = RB(0x447ca8);
    p->fill = RD(0x447cea);                        /* its span fill (clouds use their own) */
    p->count = 1;
    p->first = (uint32_t)f->nverts;
    p->cx = (int16_t)RW(0x4489d8); p->cy = (int16_t)RW(0x4489ed);
    p->top = (int16_t)RW(0x44c99b); p->bottom = (int16_t)RW(0x44c9aa);
    p->left = (int16_t)RW(0x44c9b9); p->right = (int16_t)RW(0x44c9c8);
    f->per_kind[PRIM_DISC]++;
    struct scene_vert *v = &f->verts[f->nverts++];
    memset(v, 0, sizeof *v);
    v->sx = x; v->sy = y; v->a = (uint16_t)r;
    if (RD(sp) == 0x44b4c2u) {                     /* from the sprite handler: depth and exact size */
        uint32_t rec = VREC_BASE + (ebx & 0xffff);
        v->x = (int32_t)RD(rec); v->y = (int32_t)RD(rec + 4); v->z = (int32_t)RD(rec + 8);
        int16_t rad = (int16_t)RW(ds_base + esi - 2);
        double zh = (double)v->z / 65536.0;
        if (zh > 0) {
            double exact = rad * 128.0 / zh;
            if (exact > 0 && exact < 255) v->b = (uint16_t)(exact * 256.0);
        }
    } else v->near = 1;                            /* screen-space only */
}

/* a point: 0x4759A8 plots one pixel, 0x4759D4 a 2x2 block (x = EBX, y = ESI,
 * colour CH), called by the VM handlers 0x44BA9C (1x1) and 0x44BAF4 (2x2):
 * colour byte (remapped), vertex offset word, skipped when that vertex is
 * near-clipped or off screen.  The handler pushed its bytecode pointer, just
 * past the vertex offset (TFX_CODE.md 9.16).  Lights at night, among others */
static void scene_point(struct scene_frame *f, int size) {
    if (f->nprims >= MAX_PRIMS || f->nverts + 1 > MAX_VERTS) return;
    struct scene_prim *p = &f->prims[f->nprims++];
    memset(p, 0, sizeof *p);
    p->kind = PRIM_POINT;
    p->color = (uint8_t)(ecx >> 8);
    uint32_t bc = ds_base + RD(ss_base + esp + 4);  /* the handler's saved ESI */
    p->key = (bc - 2) | 0x40000000u;
    p->count = 1;
    p->first = (uint32_t)f->nverts;
    p->cx = (int16_t)RW(0x4489d8); p->cy = (int16_t)RW(0x4489ed);
    p->top = (int16_t)RW(0x44c99b); p->bottom = (int16_t)RW(0x44c9aa);
    p->left = (int16_t)RW(0x44c9b9); p->right = (int16_t)RW(0x44c9c8);
    f->per_kind[PRIM_POINT]++;
    struct scene_vert *v = &f->verts[f->nverts++];
    memset(v, 0, sizeof *v);
    v->sx = (int16_t)ebx; v->sy = (int16_t)esi; v->a = (uint16_t)size;
    uint32_t rec = VREC_BASE + RW(bc - 2);
    v->x = (int32_t)RD(rec); v->y = (int32_t)RD(rec + 4); v->z = (int32_t)RD(rec + 8);
    v->near = v->z <= 0;
}

/* a distant object drawn as a dot: 0x44A57D (2x2) / 0x44A5F0 (1x1) project
 * object EDI/4 from its view-space position, X [0x45212E + EDI], Y
 * [0x45269E + EDI], Z [0x452C0E + EDI] (x = (X >> 8) / (Z >> 16) + centre,
 * the projection the native renderer uses), skip it when Z >> 16 <= 16 or
 * off screen, and call the plotter (TFX_CODE.md 9.18) */
static void scene_object_point(struct scene_frame *f, int size) {
    if (f->nprims >= MAX_PRIMS || f->nverts + 1 > MAX_VERTS) return;
    struct scene_prim *p = &f->prims[f->nprims++];
    memset(p, 0, sizeof *p);
    p->kind = PRIM_POINT;
    p->color = (uint8_t)(ecx >> 8);
    p->key = (edi & 0xffff) | 0x50000000u;
    p->count = 1;
    p->first = (uint32_t)f->nverts;
    p->cx = (int16_t)RW(0x4489d8); p->cy = (int16_t)RW(0x4489ed);
    p->top = (int16_t)RW(0x44c99b); p->bottom = (int16_t)RW(0x44c9aa);
    p->left = (int16_t)RW(0x44c9b9); p->right = (int16_t)RW(0x44c9c8);
    f->per_kind[PRIM_POINT]++;
    struct scene_vert *v = &f->verts[f->nverts++];
    memset(v, 0, sizeof *v);
    v->sx = (int16_t)ebx; v->sy = (int16_t)esi; v->a = (uint16_t)size;
    v->x = (int32_t)RD(0x45212e + edi); v->y = (int32_t)RD(0x45269e + edi); v->z = (int32_t)RD(0x452c0e + edi);
    v->near = v->z <= 0;
}

/* 2D overlay (TFX_CODE.md 9.17).  HUD line rasteriser 0x454212: globals
 * colour, x0, y0, x1, y1 at 0x454208..0x454210, an already clipped
 * Bresenham line, end points included.  (0x454336 is the same with ESI ->
 * those words; it draws model lines, see scene_prim.)
 * Pixel plot 0x45658C(y, x, colour) (ret 0Ch): a 1x1 bitmap.  Bitmap blit 0x45CFA4(bitmap, y, x, colour) (returns with ret 10h): the
 * bitmap is {width byte, height byte, rows of ceil(w/8) bytes, MSB first},
 * set bits drawn in the colour, clipped to the rectangle at 0x44FB78 (x min,
 * x max, y min, y max words) */
static void scene_hud(struct scene_frame *f, uint32_t entry) {
    if (f->nhud >= MAX_HUD) return;
    struct scene_hud *h = &f->hud[f->nhud];
    memset(h, 0, sizeof *h);
    if (entry == 0x45cfa4) {
        uint32_t sp = ss_base + esp;
        uint32_t bm = ds_base + RD(sp + 4);
        int w = RB(bm), ht = RB(bm + 1), stride = (w + 7) / 8;
        if (w < 1 || ht < 1 || f->nbits + stride * ht > MAX_HUD_BITS) return;
        h->kind = HUD_BITMAP; h->w = (uint8_t)w; h->h = (uint8_t)ht;
        h->y0 = (int16_t)RW(sp + 8); h->x0 = (int16_t)RW(sp + 12); h->color = RB(sp + 16);   /* (bitmap, y, x, colour) */
        h->cl = (int16_t)RW(0x44fb78); h->cr = (int16_t)RW(0x44fb7a);
        h->ct = (int16_t)RW(0x44fb7c); h->cb = (int16_t)RW(0x44fb7e);
        h->bits = (uint32_t)f->nbits;
        for (int i = 0; i < stride * ht; i++) f->hud_bits[f->nbits++] = RB(bm + 2 + (uint32_t)i);
    } else if (entry == 0x45658c) {
        uint32_t sp = ss_base + esp;
        if (f->nbits + 1 > MAX_HUD_BITS) return;
        h->kind = HUD_BITMAP; h->w = h->h = 1;
        h->y0 = (int16_t)RW(sp + 4); h->x0 = (int16_t)RW(sp + 8); h->color = RB(sp + 12);
        h->cl = 0; h->cr = 320; h->ct = 0; h->cb = 200;
        h->bits = (uint32_t)f->nbits;
        f->hud_bits[f->nbits++] = 0x80;
    } else {
        uint32_t r = 0x454208u;
        h->kind = HUD_LINE; h->color = RB(r);
        h->x0 = (int16_t)RW(r + 2); h->y0 = (int16_t)RW(r + 4); h->x1 = (int16_t)RW(r + 6); h->y1 = (int16_t)RW(r + 8);
    }
    f->nhud++;
    scene_hud_draw = 1;
}

/* a model line: 0x44EA99 draws the 2D segment at [ESI] (x0, y0, x1, y1
 * words) in colour [0x447BFC].  From the VM's line handler (return 0x44B05D)
 * EDX/EBX still hold the two vertex records; they are matched to the points
 * by their projections (a near-clipped end keeps only its 2D point). */
static void scene_line(struct scene_frame *f) {
    if (f->nprims >= MAX_PRIMS || f->nverts + 2 > MAX_VERTS) return;
    uint32_t pts = ds_base + esi;
    int16_t x[2] = {(int16_t)RW(pts), (int16_t)RW(pts + 4)}, y[2] = {(int16_t)RW(pts + 2), (int16_t)RW(pts + 6)};
    struct scene_prim *p = &f->prims[f->nprims++];
    memset(p, 0, sizeof *p);
    p->kind = PRIM_LINE;
    p->key = pts;
    p->color = RB(0x447bfc);
    p->count = 2;
    p->first = (uint32_t)f->nverts;
    p->cx = (int16_t)RW(0x4489d8); p->cy = (int16_t)RW(0x4489ed);
    p->top = (int16_t)RW(0x44c99b); p->bottom = (int16_t)RW(0x44c9aa);
    p->left = (int16_t)RW(0x44c9b9); p->right = (int16_t)RW(0x44c9c8);
    f->per_kind[PRIM_LINE]++;
    uint32_t recs[2] = {VREC_BASE + (edx & 0xffff), VREC_BASE + (ebx & 0xffff)};
    for (int k = 0; k < 2; k++) {
        struct scene_vert *v = &f->verts[f->nverts++];
        memset(v, 0, sizeof *v);
        v->sx = x[k]; v->sy = y[k]; v->near = 1;
        for (int r = 0; r < 2; r++) {
            uint32_t rec = recs[r];
            if (RW(rec + 0x0c) || (int16_t)RW(rec + 0x0e) != x[k] || (int16_t)RW(rec + 0x10) != y[k]) continue;
            v->x = (int32_t)RD(rec); v->y = (int32_t)RD(rec + 4); v->z = (int32_t)RD(rec + 8);
            v->near = 0;
            break;
        }
    }
}

int scene_sprite_pixel(int plane, uint16_t off, uint8_t val) {
    if (spx_over || nspx >= MAX_SPRITE_PX) { spx_over = 1; return 0; }
    spx[nspx].plane = (uint8_t)plane; spx[nspx].off = off; spx[nspx].val = val; nspx++;
    return 1;
}

static void sprite_finish(struct scene_frame *f) {
    if (!scene_sprite_draw) return;
    scene_sprite_draw = 0;
    int n = nspx, take = n > 0 && !spx_over && f->nprims > 0, x0 = 999, y0 = 999, x1 = -1, y1 = -1, nc = 0;
    uint8_t cols[4] = {0};
    for (int i = 0; i < n; i++) {
        uint16_t rel = (uint16_t)(spx[i].off - sprite_page);
        int x = (rel % 80) * 4 + spx[i].plane, y = rel / 80;
        if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
        int k; for (k = 0; k < nc && cols[k] != spx[i].val; k++) ;
        if (k == nc) { if (nc < 4) cols[nc] = spx[i].val; nc++; }
    }
    int w = x1 - x0 + 1, h = y1 - y0 + 1, stride = (w + 7) / 8;
    /* one colour, up to 254x254 (HUD pieces).  Sprites in several colours stay
     * the game's: the panel digits and labels are shaded pixel art, which
     * split into one stroked bitmap per colour looked worse (DEBUG_LOG 106) */
    if (take && (y1 >= 200 || w > 254 || h > 254 || nc != 1 ||
                 f->nhud + nc > MAX_HUD || f->nbits + nc * stride * h > MAX_HUD_BITS)) take = 0;
    static uint16_t buf[320 * 200];
    uint32_t shash = 0;
    int art = art_active();
    if ((scene_asset_dump || art) && n > 0 && !spx_over && w <= 320 && h <= 200) {
        for (int i = 0; i < w * h; i++) buf[i] = 0x100;
        for (int i = 0; i < n; i++) {
            uint16_t rel = (uint16_t)(spx[i].off - sprite_page);
            buf[(rel / 80 - y0) * w + (rel % 80) * 4 + spx[i].plane - x0] = spx[i].val;
        }
        shash = art_hash(buf, w, h);
        int a = art && nc > 1 ? art_find(ART_SPRITE, shash) : -1;
        if (a >= 0) {                               /* known art: its pixels that still show it get the asset */
            uint32_t pend = 0xfe000000u | (scene_sprite_seq & 0xffffffu);
            for (int i = 0; i < n; i++) {
                uint16_t rel = (uint16_t)(spx[i].off - sprite_page);
                uint32_t lx = (uint32_t)((rel % 80) * 4 + spx[i].plane - x0), ly = (uint32_t)(rel / 80 - y0);
                vga_prov_resolve(spx[i].plane, spx[i].off, pend, (uint32_t)(a + 1) << 24 | ly << 12 | lx);
            }
        }
    }
    if (scene_asset_dump && n > 0 && !spx_over && w <= 320 && h <= 200) {
        char path[300];
        snprintf(path, sizeof path, "%s\\spr_%08x_%03u.bmp", asset_dir, sprite_args[3], sprite_args[2]);
        vga_save_indexed(path, w, h, buf);
        snprintf(path, sizeof path, "%s\\manifest.txt", asset_dir);
        FILE *mf = fopen(path, "a");
        if (mf) {
            fprintf(mf, "spr_%08x_%03u.bmp bank %08x index %u caller %08x at %d,%d size %dx%d colours %d hash %08x\n", sprite_args[3],
                    sprite_args[2], sprite_args[3], sprite_args[2], sprite_ret, x0, y0, w, h, nc, shash);
            fclose(mf);
        }
    }
    if (sprite_log)
        rt_log("sprite: ret %08x index %u bank %08x at %d,%d: %d pixels, %dx%d at %d,%d, %d colour(s) %d %d %d -> %s", sprite_ret, sprite_args[2],
               sprite_args[3], (int16_t)sprite_args[1], (int16_t)sprite_args[0], n, n ? w : 0, n ? h : 0, x0, y0,
               spx_over ? 99 : nc, cols[0], cols[1], cols[2], take ? "HUD" : "UI");
    if (!take) {
        for (int i = 0; i < n; i++) vga_owner_to_ui(spx[i].plane, spx[i].off);
        nspx = 0; spx_over = 0;
        return;
    }
    for (int k = 0; k < nc; k++) {
        struct scene_hud *hd = &f->hud[f->nhud++];
        memset(hd, 0, sizeof *hd);
        hd->kind = HUD_BITMAP; hd->w = (uint8_t)w; hd->h = (uint8_t)h; hd->x0 = (int16_t)x0; hd->y0 = (int16_t)y0; hd->color = cols[k];
        hd->cl = 0; hd->cr = 320; hd->ct = 0; hd->cb = 200;
        hd->bits = (uint32_t)f->nbits;
        memset(f->hud_bits + f->nbits, 0, (size_t)(stride * h));
        for (int i = 0; i < n; i++) {
            if (spx[i].val != cols[k]) continue;
            uint16_t rel = (uint16_t)(spx[i].off - sprite_page);
            int x = (rel % 80) * 4 + spx[i].plane - x0, y = rel / 80 - y0;
            f->hud_bits[f->nbits + y * stride + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));
        }
        f->nbits += stride * h;
    }
    nspx = 0; spx_over = 0;
}

void scene_prim(uint32_t entry) {
    /* A page flip is noticed lazily (vga.c: at the next retrace poll or
     * present).  A frame that took longer than a tick doesn't wait before the
     * next one, so the flip was noticed some hundred primitives into it, and
     * the captured frame held the end of one game frame and the start of the
     * next: its background bands then covered the world (DEBUG_LOG 122).
     * So: before a primitive is recorded, a pending flip ends the frame. */
    { extern void vga_flip_check(void); vga_flip_check(); }
    struct scene_frame *f = &frames[cur];
    ensure(f);
    sprite_finish(f);
    if (entry == 0x450454) {                       /* 8-bit sprite blit (y, x, index, bank), ret 10h */
        uint32_t sp = ss_base + esp;
        sprite_ret = RD(sp);
        for (int i = 0; i < 4; i++) sprite_args[i] = RD(sp + 4 + 4 * i);
        sprite_page = (uint32_t)RW(0x455907) << 4;
        nspx = 0; spx_over = 0;
        scene_sprite_seq++;
        scene_sprite_draw = 1;
        return;
    }
    scene_bg_draw = 0;
    scene_line_draw = 0;
    scene_point_draw = 0;
    scene_hud_draw = 0;
    scene_line2_draw = 0;
    scene_disc_draw = 0;
    if (entry == 0x454336) {                       /* second line rasteriser (TFX_CODE.md 9.14) */
        /* from the VM's line handler 0x44B09C (return 0x44B0AC): a model line
         * scene_line() captured a moment ago through 0x44EA99; its pixels
         * are the native line's.  Other callers (0x46053F: polygon outlines,
         * only seen on 2D screens) stay the game's. */
        if (RD(ss_base + esp) == 0x44b0acu) {
            scene_line2_draw = 1;
            if (f->prims && f->nprims && f->prims[f->nprims - 1].kind == PRIM_LINE) f->prims[f->nprims - 1].fill = 1;
        }
        return;
    }
    if (entry == 0x454212 || entry == 0x45cfa4 || entry == 0x45658c) { if (f->prims) scene_hud(f, entry); return; }
    if (entry == 0x44ea99) {                       /* also used for maps and HUD: only the VM's lines */
        if (RD(ss_base + esp) == 0x44b05du) { scene_line_draw = 1; if (f->prims) scene_line(f); }
        return;
    }
    if (entry == 0x44f26a) { if (f->prims) scene_disc(f); return; }
    if (entry == 0x4759a8 || entry == 0x4759d4) {     /* only the VM's 3D points (handlers 0x44BA9C / 0x44BAF4) */
        uint32_t ret = RD(ss_base + esp);
        if (ret == 0x44bae6u || ret == 0x44bb3eu) { scene_point_draw = 1; if (f->prims) scene_point(f, entry == 0x4759d4 ? 2 : 1); }
        else if (ret == 0x44a5efu || ret == 0x44a662u) {   /* a distant object as a dot (0x44A57D / 0x44A5F0) */
            scene_point_draw = 1; if (f->prims) scene_object_point(f, entry == 0x4759d4 ? 2 : 1);
        }
        return;
    }
    uint32_t n = ecx & 0xffff;
    if (!f->prims || n < 2 || n > 64 || f->nprims >= MAX_PRIMS || f->nverts + (int)n > MAX_VERTS) return;
    if (!f->nprims) {                              /* projection centre (patched immediates) */
        f->cx = (int16_t)RW(0x4489d8);
        f->cy = (int16_t)RW(0x4489ed);
    }
    int kind = kind_of(entry);
    /* a near-plane routine that finds every vertex projected jumps to the
     * plain primitive with the same list: keep one copy (the plain one) */
    if (kind <= PRIM_TEXTURED && near_index >= 0 && near_index == f->nprims - 1 && near_list == esi &&
        f->prims[near_index].count == n && f->prims[near_index].kind == kind + PRIM_FLAT_NEAR) {
        f->per_kind[f->prims[near_index].kind]--;
        f->nverts = (int)f->prims[near_index].first;
        f->nprims--;
    }
    near_index = -1;
    if (kind >= PRIM_FLAT_NEAR) { near_index = f->nprims; near_list = esi; }
    struct scene_prim *p = &f->prims[f->nprims++];
    p->kind = (uint8_t)kind;
    p->tex = p->table = p->fill = 0;
    p->derived = 0;
    p->key = ds_base + esi;
    {   /* the sky/haze routine around 0x449300 builds 2D-only vertex
         * records (screen x/y; X/Y/Z are stale) and calls the primitives
         * directly; everything else comes through the VM's call [0x447CE6] */
        uint32_t ret = RD(ss_base + esp);
        if (ret == 0x449332u || ret == 0x4493a5u || ret == 0x449418u || ret == 0x44948eu) {
            p->derived = 2;
            scene_bg_draw = 1;
        }
    }
    /* screens such as the arming screen draw several 3D viewports a frame
     * (the aircraft, each weapon in the list): the projection centre and
     * clip rectangle are patched per viewport, so take them per primitive */
    p->cx = (int16_t)RW(0x4489d8); p->cy = (int16_t)RW(0x4489ed);
    p->top = (int16_t)RW(0x44c99b); p->bottom = (int16_t)RW(0x44c9aa);
    p->left = (int16_t)RW(0x44c9b9); p->right = (int16_t)RW(0x44c9c8);
    if (kind == PRIM_GOURAUD || kind == PRIM_GOURAUD_NEAR) {
        p->table = ds_base + RD(0x44787e);
        p->fill = RD(0x468112);
    } else if (kind == PRIM_TEXTURED || kind == PRIM_TEXTURED_NEAR) {
        p->table = ds_base + RD(0x44787e);
        p->tex = ds_base + RD(0x44787a);
        p->fill = RD(0x46810e);
    }
    if (p->fill) {
        int t = kind == PRIM_GOURAUD || kind == PRIM_GOURAUD_NEAR ? 1 : 0;
        for (int k = 0; k < 8; k++) {
            if (fills_seen[t][k] == p->fill) { fills_count[t][k]++; break; }
            if (!fills_seen[t][k]) { fills_seen[t][k] = p->fill; fills_count[t][k] = 1; break; }
        }
    }
    p->color = RB(0x447ca8);
    /* the back-face test is a js the game can patch out per model (as it
     * does for 0x44CBE8 below); near-plane copies re-enter the plain routine
     * before it.  flat 0x44CA16, Gouraud 0x44CFD6, textured 0x44D2B2 */
    {
        static const uint32_t test_at[3] = {0x44ca16, 0x44cfd6, 0x44d2b2};
        int base = kind >= PRIM_FLAT_NEAR ? kind - PRIM_FLAT_NEAR : kind;
        p->cull = base <= PRIM_TEXTURED ? (RB(test_at[base]) == 0x0f && RB(test_at[base] + 1) == 0x88) : 1;
    }
    if (entry == 0x44cbe8) {
        /* called by the VM handler 0x44BBBC (return 0x44BCEA): a genuine
         * primitive whose back-face test that handler may patch out (EB 4A at
         * 0x44CCA2); called from the near-plane routines: a clipped copy */
        if (!p->derived) p->derived = RD(ss_base + esp) != 0x44bceau;
        p->cull = RB(0x44cca2) != 0xeb;
    }
    p->count = (uint16_t)n;
    p->first = (uint32_t)f->nverts;
    f->per_kind[p->kind]++;
    uint32_t list = ds_base + esi;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t rec = VREC_BASE + RW(list + 2 * i);
        struct scene_vert *v = &f->verts[f->nverts++];
        v->x = (int32_t)RD(rec); v->y = (int32_t)RD(rec + 4); v->z = (int32_t)RD(rec + 8);
        v->near = RW(rec + 0x0c) != 0;
        v->sx = (int16_t)RW(rec + 0x0e); v->sy = (int16_t)RW(rec + 0x10);
        /* per-vertex attributes, in list order, written by the VM handler
         * before the call (TFX_CODE.md 9.12): shade words at 0x447FAC + 4i,
         * texture coordinate dwords at 0x4481AC + 4i */
        v->a = v->b = 0;
        if (p->table && !p->tex) v->a = RW(0x447fac + 4 * i);
        else if (p->tex) { uint32_t uv = RD(0x4481ac + 4 * i); v->a = (uint16_t)uv; v->b = (uint16_t)(uv >> 16); }
        if (!v->near) {
            /* the game's exact projection: (X >> 8) / (Z >> 16) + centre */
            int16_t zh = (int16_t)(v->z >> 16);
            if (zh > 0) {
                int16_t px = (int16_t)((int16_t)((v->x >> 8) / zh) + p->cx);
                int16_t py = (int16_t)((int16_t)((v->y >> 8) / zh) + p->cy);
                f->checked++;
                if (px != v->sx || py != v->sy) f->mismatches++;
            }
        }
    }
}

static int idle_flips;
int scene_current(void) { return idle_flips < 2; }

void scene_end_frame(void) {
    struct scene_frame *f = &frames[cur];
    sprite_finish(f);
    sprite_log = 0;
    if (scene_asset_dump && f->nprims) scene_asset_dump = 0;
    if (!f->nprims) {                              /* no 3D drawn (menus, map): keep the last scene */
        f->nhud = f->nbits = 0;
        if (idle_flips < 1000) idle_flips++;
        return;
    }
    idle_flips = 0;
    f->number = ++frame_no;
    cur ^= 1;
    struct scene_frame *n = &frames[cur];
    n->nprims = n->nverts = n->mismatches = n->checked = 0;
    n->nhud = n->nbits = 0;
    memset(n->per_kind, 0, sizeof n->per_kind);
}

const struct scene_frame *scene_last(void) {
    const struct scene_frame *f = &frames[cur ^ 1];
    return f->number ? f : NULL;
}

/* Texel and shade lookups as each hypothesis reads them.
 * Textured (span fill 0x475334): the texel is [tex + row*256 + col], then
 * remapped through the table, 0 = transparent; the span setup turns an edge
 * word w into 16.16 with texel = w >> 7, the first interpolant forming the
 * row.  uv modes: 0 row=a>>7 col=b>>7, 1 row=b>>7 col=a>>7, 2/3 the same >>8.
 * Gouraud (span fill 0x4756CC): index = table[shade]; shade modes:
 * 0 a>>8, 1 (a>>7)&255, 2 a>>8 used directly as the palette index. */
static uint8_t tex_index(const struct scene_prim *p, double a, double b, int mode) {
    int sh = mode >= 2 ? 8 : 7;
    int ia = ((int)a >> sh) & 255, ib = ((int)b >> sh) & 255;
    int row = (mode & 1) ? ib : ia, col = (mode & 1) ? ia : ib;
    uint32_t t = p->tex + (uint32_t)row * 256u + (uint32_t)col;
    if (t >= MEM_SIZE || p->table + 256 > MEM_SIZE) return 0;
    return MEM[p->table + MEM[t]];
}
static uint8_t shade_index(const struct scene_prim *p, double a, int mode) {
    int s = (int)a;
    if (s < 0) s = 0;
    if (p->table + 256 > MEM_SIZE) return 0;
    if (mode == 0) return MEM[p->table + ((s >> 8) & 255)];
    if (mode == 1) return MEM[p->table + ((s >> 7) & 255)];
    return (uint8_t)(s >> 8);
}

static uint8_t (*last_pix)(int x, int y);
static long long area2(const struct scene_vert *v) {   /* the game's test: keep when >= 0 */
    return (long long)(v[2].sy - v[0].sy) * (v[1].sx - v[0].sx) - (long long)(v[2].sx - v[0].sx) * (v[1].sy - v[0].sy);
}

void scene_check(uint8_t (*pix)(int x, int y)) {
    static uint32_t checked;
    last_pix = pix;
    const struct scene_frame *f = scene_last();
    if (!f || f->number == checked) return;          /* each frame once */
    checked = f->number;
    for (int i = 0; i < f->nprims; i++) {
        const struct scene_prim *p = &f->prims[i];
        if ((p->kind != PRIM_TEXTURED && p->kind != PRIM_GOURAUD) || p->count < 3 || !p->table) continue;
        const struct scene_vert *v = &f->verts[p->first];
        if (v[0].near || v[1].near || v[2].near) continue;
        if (area2(v) < 0) continue;                         /* back face: not drawn */
        /* the pixel nearest the centroid of the first triangle */
        double x0 = v[0].sx, y0 = v[0].sy, x1 = v[1].sx, y1 = v[1].sy, x2 = v[2].sx, y2 = v[2].sy;
        double det = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        if (det > -40 && det < 40) continue;                  /* too small to sample reliably */
        int px = (int)((x0 + x1 + x2) / 3.0), py = (int)((y0 + y1 + y2) / 3.0);
        if (px < 0 || px >= 320 || py < 0 || py >= 200) continue;
        double l1 = ((px - x0) * (y2 - y0) - (x2 - x0) * (py - y0)) / det;
        double l2 = ((x1 - x0) * (py - y0) - (px - x0) * (y1 - y0)) / det;
        double l0 = 1 - l1 - l2;
        double a = l0 * v[0].a + l1 * v[1].a + l2 * v[2].a, b = l0 * v[0].b + l1 * v[1].b + l2 * v[2].b;
        uint8_t got = pix(px, py);
        if (p->kind == PRIM_TEXTURED) {
            uv_tests++;
            for (int m = 0; m < UV_MODES; m++) if (tex_index(p, a, b, m) == got) uv_hits[m]++;
        } else {
            shade_tests++;
            for (int m = 0; m < SHADE_MODES; m++) if (shade_index(p, a, m) == got) shade_hits[m]++;
        }
    }
    /* adopt a shade mapping once it clearly wins.  The uv mapping is fixed
     * (mode 1, confirmed by tracing the span fill's texel addresses): small,
     * partly transparent textured polygons make the centroid test unreliable */
    for (int m = 0; m < SHADE_MODES; m++) if (shade_tests >= 200 && shade_hits[m] > 2 * shade_hits[shade_mode] + 20) shade_mode = m;
}
/* debug (cmdfile D): every primitive of the last frame in drawing order,
 * with its screen bounds, depth and the original page's pixel at the
 * centre of its bounds */
void scene_dump(void) {
    const struct scene_frame *f = scene_last();
    if (!f) { rt_log("dump: nothing captured"); return; }
    static const char *kn[] = {"flat", "gour", "tex", "eff", "flatN", "gourN", "texN", "disc", "line", "point"};
    rt_log("dump: frame %u, %d primitives, %d HUD items", f->number, f->nprims, f->nhud);
    for (int i = 0; i < f->nhud; i++) {
        const struct scene_hud *h = &f->hud[i];
        if (h->kind == HUD_LINE) rt_log("dump: hud #%d line c%d (%d,%d)-(%d,%d)", i, h->color, h->x0, h->y0, h->x1, h->y1);
        else rt_log("dump: hud #%d bitmap c%d at %d,%d %dx%d clip %d..%d x %d..%d", i, h->color, h->x0, h->y0, h->w, h->h,
                    h->cl, h->cr, h->ct, h->cb);
    }
    for (int i = 0; i < f->nprims; i++) {
        const struct scene_prim *p = &f->prims[i];
        const struct scene_vert *v = &f->verts[p->first];
        int x0 = 32767, y0 = 32767, x1 = -32768, y1 = -32768, zmin = 0x7fffffff, nn = 0;
        for (int k = 0; k < p->count; k++) {
            if (v[k].near) { nn++; continue; }
            if (v[k].sx < x0) x0 = v[k].sx;
            if (v[k].sx > x1) x1 = v[k].sx;
            if (v[k].sy < y0) y0 = v[k].sy;
            if (v[k].sy > y1) y1 = v[k].sy;
            if (p->derived != 2 && (v[k].z >> 16) < zmin) zmin = v[k].z >> 16;
        }
        int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, pix = -1;
        if (last_pix && cx >= 0 && cx < 320 && cy >= 0 && cy < 200) pix = last_pix(cx, cy);
        rt_log("dump: #%d %s%s c%d n%d near%d x %d..%d y %d..%d z %d clip %d,%d-%d,%d tab %x page %d r %d fill %x",
               i, kn[p->kind < PRIM_KINDS ? p->kind : 0], p->derived == 2 ? " SCR" : p->derived ? " der" : "", p->color,
               p->count, nn, x0, x1, y0, y1, zmin == 0x7fffffff ? -1 : zmin, p->left, p->top, p->right, p->bottom,
               p->table, pix, p->kind == PRIM_DISC ? v[0].a : 0, p->fill);
        if (p->derived == 2)
            for (int k = 0; k < p->count && k < 8; k++) rt_log("dump:    (%d,%d)", v[k].sx, v[k].sy);
    }
}
int scene_uv_mode(void) { return uv_mode; }
int scene_shade_mode(void) { return shade_mode; }

void scene_log_stats(void) {
    sprite_log = 1;
    for (int k = 0; k < 10; k++)
        if (fill_area[k] > 0) rt_log("scene: disc fill %06x: %u pixels over %.0f of unclipped disc area = %.3f", k < 9 ? disc_fills[k] : 0,
                                     scene_fill_px[k], fill_area[k], scene_fill_px[k] / fill_area[k]);
    const struct scene_frame *f = scene_last();
    if (!f) { rt_log("scene: nothing captured yet"); return; }
    rt_log("scene: frame %u: %d polygons, %d vertices, centre %d,%d; re-projection %d/%d mismatches",
           f->number, f->nprims, f->nverts, f->cx, f->cy, f->mismatches, f->checked);
    rt_log("scene:   flat %d gouraud %d textured %d effect %d | near: flat %d gouraud %d textured %d | discs %d lines %d",
           f->per_kind[0], f->per_kind[1], f->per_kind[2], f->per_kind[3], f->per_kind[4], f->per_kind[5], f->per_kind[6],
           f->per_kind[PRIM_DISC], f->per_kind[PRIM_LINE]);
    rt_log("scene: textured mapping tests %u, hits per uv mode %u %u %u %u -> mode %d; Gouraud tests %u, hits %u %u %u -> mode %d",
           uv_tests, uv_hits[0], uv_hits[1], uv_hits[2], uv_hits[3], uv_mode, shade_tests, shade_hits[0], shade_hits[1], shade_hits[2], shade_mode);
    for (int t = 0; t < 2; t++)
        for (int k = 0; k < 8 && fills_seen[t][k]; k++)
            rt_log("scene: %s span fill %#x used %u times", t ? "Gouraud" : "textured", fills_seen[t][k], fills_count[t][k]);
    for (int i = 0, shown = 0; i < f->nprims && shown < 6; i++) {
        const struct scene_prim *p = &f->prims[i];
        if (!p->table) continue;
        const struct scene_vert *v = &f->verts[p->first];
        rt_log("scene:   kind %d tex %#x table %#x fill %#x, attrs %04x/%04x %04x/%04x %04x/%04x", p->kind, p->tex, p->table, p->fill,
               v[0].a, v[0].b, v[1].a, v[1].b, p->count > 2 ? v[2].a : 0, p->count > 2 ? v[2].b : 0);
        shown++;
    }
    /* widescreen survey: how far beyond the screen sides the submitted
     * geometry reaches (|256 X / Z| in game pixels from the centre), and the
     * screen-space polygons' corners */
    {
        int hist[8] = {0}, wholly_off = 0;
        double far_x = 0;
        for (int i = 0; i < f->nprims; i++) {
            const struct scene_prim *p = &f->prims[i];
            if (p->derived || p->kind == PRIM_DISC) continue;
            const struct scene_vert *v = &f->verts[p->first];
            double mn = 1e9, mx = 0;
            int ok = 1;
            for (int k = 0; k < p->count; k++) {
                if (v[k].z <= (16 << 16)) { ok = 0; break; }
                double ax = 256.0 * (double)v[k].x / (double)v[k].z;
                double a = ax < 0 ? -ax : ax;
                if (a < mn) mn = a;
                if (a > mx) mx = a;
                if (a > far_x) far_x = a;
            }
            if (!ok) continue;
            if (mn > 160) { wholly_off++; int b = (int)((mn - 160) / 40); hist[b > 7 ? 7 : b]++; }
        }
        rt_log("scene: wide: %d polygons wholly beyond the screen sides; nearest edge 160+ [0-40) %d [40-80) %d [80-120) %d [120-160) %d [160-200) %d [200-240) %d [240+) %d; farthest vertex %.0f px",
               wholly_off, hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6] + hist[7], far_x);
        for (int i = 0, shown = 0; i < f->nprims && shown < 40; i++) {
            const struct scene_prim *p = &f->prims[i];
            if (p->derived != 2) continue;
            const struct scene_vert *v = &f->verts[p->first];
            char buf[256]; int o = 0;
            for (int k = 0; k < p->count && k < 8; k++) o += snprintf(buf + o, sizeof buf - (size_t)o, " (%d,%d)", v[k].sx, v[k].sy);
            rt_log("scene: screen-space #%d kind %d colour %d clip %d..%d:%s", i, p->kind, p->color, p->left, p->right, buf);
            shown++;
        }
    }
    /* textured detail: what the game drew at the centroid against each mapping */
    for (int i = 0, shown = 0; i < f->nprims && shown < 6; i++) {
        const struct scene_prim *p = &f->prims[i];
        if (p->kind != PRIM_TEXTURED || p->count < 3) continue;
        const struct scene_vert *v = &f->verts[p->first];
        shown++;
        rt_log("scene: textured #%d: %d verts, centre %d,%d clip %d..%d x %d..%d, area %lld, tex %#x table %#x",
               i, p->count, p->cx, p->cy, p->left, p->right, p->top, p->bottom, area2(v), p->tex, p->table);
        for (int k = 0; k < p->count && k < 6; k++)
            rt_log("scene:     v%d screen %d,%d near %d attrs %04x %04x", k, v[k].sx, v[k].sy, v[k].near, v[k].a, v[k].b);
        if (p->table + 16 <= MEM_SIZE && p->tex + 16 <= MEM_SIZE) {
            const uint8_t *t = MEM + p->table, *x = MEM + p->tex;
            rt_log("scene:     table %02x %02x %02x %02x %02x %02x %02x %02x | tex %02x %02x %02x %02x %02x %02x %02x %02x",
                   t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7]);
        }
        if (last_pix && !v[0].near && !v[1].near && !v[2].near) {
            int px = (v[0].sx + v[1].sx + v[2].sx) / 3, py = (v[0].sy + v[1].sy + v[2].sy) / 3;
            if (px >= 0 && px < 320 && py >= 0 && py < 200) {
                double x0 = v[0].sx, y0 = v[0].sy, x1 = v[1].sx, y1 = v[1].sy, x2 = v[2].sx, y2 = v[2].sy;
                double det = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
                if (det != 0) {
                    double l1 = ((px - x0) * (y2 - y0) - (x2 - x0) * (py - y0)) / det;
                    double l2 = ((x1 - x0) * (py - y0) - (px - x0) * (y1 - y0)) / det, l0 = 1 - l1 - l2;
                    double a = l0 * v[0].a + l1 * v[1].a + l2 * v[2].a, b = l0 * v[0].b + l1 * v[1].b + l2 * v[2].b;
                    rt_log("scene:     pixel %d,%d = %02x (displayed page); predicted %02x %02x %02x %02x (a %.0f b %.0f)",
                           px, py, last_pix(px, py), tex_index(p, a, b, 0), tex_index(p, a, b, 1), tex_index(p, a, b, 2),
                           tex_index(p, a, b, 3), a, b);
                }
            }
        }
    }
}
