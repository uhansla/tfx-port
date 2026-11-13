/* Native renderer: per-frame capture of TFX's 3D polygons (scene.c). */
#ifndef SCENE_H
#define SCENE_H
#include <stdint.h>

enum { PRIM_FLAT, PRIM_GOURAUD, PRIM_TEXTURED, PRIM_EFFECT, PRIM_FLAT_NEAR, PRIM_GOURAUD_NEAR, PRIM_TEXTURED_NEAR,
       PRIM_DISC,     /* filled circle 0x44F26A: one vertex, a = radius in game pixels, b = exact radius x 256 (0 = unknown) */
       PRIM_LINE,     /* model line (VM handler 0x44AFAC -> 0x44EA99): two vertices; near = 1 when only the 2D points are known */
       PRIM_POINT,    /* 1x1 (0x4759A8) or 2x2 (0x4759D4) point: one vertex, a = size in game pixels */
       PRIM_KINDS };

struct scene_vert {
    int32_t x, y, z;          /* view space as the game computes it (fixed point: screen = 256*x/z) */
    int16_t sx, sy;           /* the game's own projected point (valid when !near) */
    uint16_t near;            /* 1 = in front of the near plane (game clips it) */
    uint16_t a, b;            /* attributes: Gouraud shade (a) or texture coordinate words (a low, b high) */
};
struct scene_prim {
    uint8_t kind, color;
    uint8_t derived;          /* not drawn natively: 1 clipped copy made inside a near-plane routine,
                               * 2 screen-space polygon (sky/haze, 0x4493xx: 2D records only) */
    uint8_t cull;             /* the game applies its back-face test to this primitive */
    uint16_t count;
    uint32_t first;           /* index of the first vertex in verts[] */
    uint32_t tex;             /* textured: texture base (linear address, 256-byte rows) */
    uint32_t table;           /* Gouraud/textured: 256-byte remap to palette index (linear address) */
    uint32_t fill;            /* span routine the primitive calls ([0x46810E] textured, [0x468112] Gouraud); 1 on a line the game rasterises with 0x454336 */
    int16_t cx, cy;           /* projection centre in force for this primitive (0x4489D8/0x4489ED) */
    int16_t top, bottom, left, right;   /* its clip rectangle, inclusive (0x44C99B/AA/B9/C8) */
    uint32_t key;             /* identity across frames: linear address of its vertex list in the
                               * model bytecode (discs: bytecode position | 80000000h), see native.c */
};
/* 2D overlay item drawn by the game's line rasterisers or its 1-bit bitmap
 * blit (HUD, MFD text): drawn again natively, sharp (TFX_CODE.md 9.17) */
enum { HUD_LINE, HUD_BITMAP };
struct scene_hud {
    uint8_t kind, color, w, h;    /* bitmap: width, height in pixels */
    int16_t x0, y0, x1, y1;       /* line: end points (inclusive, already clipped); bitmap: x0/y0 top left */
    int16_t cl, ct, cr, cb;       /* bitmap: the blit's clip rectangle (0x44FB78..7E: x min, x max, y min, y max) */
    uint32_t bits;                /* bitmap: offset of its rows in hud_bits (ceil(w/8) bytes a row, MSB first) */
};
struct scene_frame {
    uint32_t number;
    int cx, cy;               /* projection centre of the first primitive (each has its own) */
    int nprims, nverts;
    int mismatches, checked;  /* re-projection check against the game's points */
    int per_kind[PRIM_KINDS];
    struct scene_prim *prims;
    struct scene_vert *verts;
    int nhud, nbits;
    struct scene_hud *hud;
    uint8_t *hud_bits;
};

void scene_end_frame(void);                  /* at the game's page flip */
/* 1 while the game draws a screen-space (background) polygon: its pixels
 * stay the game's own, behind the native scene (vga.c) */
extern int scene_bg_draw;
/* 1 while the game draws a 3D model line (its pixels belong to the native scene) */
extern int scene_line_draw;
/* 1 while the game plots a VM point (light): its pixels belong to the native scene */
extern int scene_point_draw;
extern int scene_asset_dump;       /* cmdfile A: save this frame's sprites (vga.c asset_dir) */
extern int scene_sprite_draw;
extern uint32_t scene_sprite_seq;   /* sprites drawn so far (art pack tags, vga.c) */      /* the sprite blit 0x450454 is drawing (TFX_CODE.md 9.19) */
int scene_sprite_pixel(int plane, uint16_t off, uint8_t val);
extern int scene_disc_draw;      /* a captured disc is being filled: 1 + fill style index (+16 when clipped) */
extern unsigned scene_fill_px[10];
extern int scene_line2_draw;     /* 0x454336 is drawing a captured model line (VM handler 0x44B09C) */
/* 1 while the game draws a captured HUD line or bitmap: its pixels are redrawn natively */
extern int scene_hud_draw;
const struct scene_frame *scene_last(void);  /* last complete frame */
int scene_current(void);                     /* the last page flip had 3D drawn (not a 2D screen such as the map) */
void scene_log_stats(void);
void scene_dump(void);        /* debug: every primitive of the last frame */
/* compare the captured attributes with the game's own pixels on the page
 * just completed (pix reads its palette index) and adopt the attribute
 * mapping that matches; see scene.c */
void scene_check(uint8_t (*pix)(int x, int y));
int scene_uv_mode(void);      /* 0..3, see scene.c */
int scene_shade_mode(void);   /* 0..2, see scene.c */
#endif
