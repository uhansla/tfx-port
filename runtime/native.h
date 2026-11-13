/* Native renderer: GPU triangles from the captured scene (native.c). */
#ifndef NATIVE_H
#define NATIVE_H
#include <stdint.h>
struct scene_frame;

/* clip space, BGRA colour (flat), attribute u/v and the shading info:
 * bits 0-1 mode (0 flat, 1 Gouraud: u = shade index, 2 textured: u = column,
 * v = row in texels), bits 8-15 remap table, bits 16-23 texture layer */
struct native_vertex { float x, y, z, w; uint32_t argb; float u, v; uint32_t info; float clip[4];
                       float px, py, pw, pad; };
/* px/py/pw: the same point's clip x/y/w in the previous scene frame (stage 5,
 * motion vectors); valid when info bit 24 (NINFO_MOTION) is set */
#define NINFO_MOTION 0x1000000u
/* clip[]: distances to the primitive's own clip rectangle (>= 0 inside), as
 * SV_ClipDistance: each 3D viewport on screen keeps its own rectangle */
enum { NMODE_FLAT, NMODE_GOURAUD, NMODE_TEXTURED };

#define NATIVE_MAX_TEX 64       /* 256x256 texture windows per frame */
#define NATIVE_MAX_TABLES 64    /* 256-byte remap tables per frame */
/* per-frame shading data for the GPU */
struct native_res {
    int ntex, ntables;
    uint8_t (*tex)[65536];      /* ntex windows of 256 rows x 256 texels (palette indices before remap) */
    uint8_t (*tables)[256];     /* ntables remap tables (index -> palette index) */
    uint32_t palette[256];      /* ARGB */
};
struct native_stats { int drawn, culled, skipped_derived, skipped_kind, triangles, orient_agree, orient_disagree;
                      int gouraud, textured, discs, screen, tex_overflow;
                      int matched, unmatched, instanced;   /* motion: primitives found in the previous frame */
                      float mv_mean, mv_max;               /* motion of matched primitives (vertex 0), output pixels */
                      int history_frames;                  /* scene frames between the motion pair (1 normally) */
                      int cam_valid, cam_pairs, cam_inliers, cam_groups; /* camera delta fit (static scenery) */
                      float cam_angle, cam_move, cam_rms;    /* degrees, game units, game units */
                      int mv_hist[2][4];                   /* unique / instanced matches: <2, <10, <50, >=50 px */
                      int from_camera;                     /* unmatched primitives moved with the camera delta */
                      int instance_rejected;               /* instance matches too far from the camera prediction */
                      int lines;                           /* model lines drawn */
                      int lines_454336;                    /* of those, lines the game rasterises with 0x454336 */
                      int points;                          /* VM points (lights) drawn */
                      int new_frame; };                    /* this build started a new scene frame (else a re-present) */

void native_log_rejects(void);
/* ext: widescreen, game pixels added at each side of full-width 3D views (0 = 4:3) */
int native_build(const struct scene_frame *f, int dx, int dy, int dw, int dh, int ow, int oh, float ext,
                 const struct native_vertex **out, const struct native_res **res, struct native_stats *st);
/* the frame has a full-screen 3D view (a primitive clipped to the whole 320 width) */
int native_full_view(const struct scene_frame *f);
#endif
