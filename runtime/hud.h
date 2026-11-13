/* Sharp HUD (hud.c): the game's 2D overlay lines and 1-bit bitmaps (HUD,
 * MFD text and symbols), captured by scene.c, drawn again at the output
 * resolution.  See RENDERER.md, "Sharp HUD", and TFX_CODE.md 9.17. */
#ifndef HUD_H
#define HUD_H
#include <stdint.h>
struct scene_frame;

/* clip-space position; mode 0 line: u = distance across the line, v = along
 * it, a = half width, b = length (output pixels); mode 1 bitmap: u/v =
 * atlas texel coordinates */
struct hud_vertex { float x, y, u, v, a, b; uint32_t argb; float mode; };

#define HUD_ATLAS 256          /* R8 atlas, HUD_ATLAS x HUD_ATLAS texels, 0 or 255 */

/* build the overlay for the 4:3 picture at (dx, dy, dw, dh) in an ow x oh
 * output; returns the vertex count (0: nothing), with the atlas */
int hud_build(const struct scene_frame *f, int dx, int dy, int dw, int dh, int ow, int oh,
              const struct hud_vertex **verts, const uint8_t **atlas);
#endif
