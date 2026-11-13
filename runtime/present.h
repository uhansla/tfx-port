/* Presenters: Direct3D 12 (d3d12.c, the default) and the SDL renderer
 * fallback (vga.c).  See RENDERER.md, stage 2. */
#ifndef PRESENT_H
#define PRESENT_H
#include <stdint.h>

struct SDL_Window;
struct native_vertex;
struct native_res;

/* the upscaler slot (RENDERER.md, stage 7): the native scene is rendered at
 * scale x the output into its own HUD-less colour/depth/motion targets with
 * a sub-pixel jitter, then upscaled/accumulated to the output resolution and
 * composited under the game's cockpit/HUD.  The port's own temporal
 * upscaler (taa) fills it; FSR 2/3 and DLSS (Streamline) take the same
 * inputs: colour, depth, motion (render pixels, previous - current, no
 * jitter), jitter offset, render/output size, reset. */
struct upscale_params {
    int enabled;              /* upscaler on (renderer=native, upscaler != off) */
    float scale;              /* render resolution / output resolution (0.5..1) */
    int new_scene;            /* a new scene frame (else a re-present: motion 0, keep accumulating) */
    int reset;                /* camera cut: discard the history */
    int fsr;                  /* upscaler=fsr: AMD FSR (fsr.c) when its DLL loads, else taa */
    int dlss;                 /* upscaler=dlss: NVIDIA DLSS (dlss.cpp) when Streamline is ready, else taa */
};

/* overlay line vertex in output pixels (lines are drawn as pairs) */
struct overlay_vertex { float x, y; uint32_t argb; };

int d3d_init(struct SDL_Window *win);                /* 0 = ready */
void d3d_output_size(int *ow, int *oh);
/* draw the w x h ARGB frame into dest (output pixels) with the given
 * scaling mode, then the overlay lines; optionally save the result as BMP */
void d3d_present(const uint32_t *pixels, int w, int h, int scaling,
                 int dx, int dy, int dw, int dh,
                 const struct overlay_vertex *lines, int nline_verts, const char *shot_bmp,
                 const struct native_vertex *tris, int ntri_verts, const uint8_t *mask,
                 const struct native_res *res, int wx, int ww,   /* wx/ww: the native scene's horizontal extent */
                 const struct upscale_params *up);
void d3d_shutdown(void);
struct hud_vertex;
/* the sharp HUD overlay for the next d3d_present (hud.c); n = 0: none */
void d3d_set_hud(const struct hud_vertex *v, int n, const uint8_t *atlas);
/* ui_filter=remacri: per game pixel of the frame, the art asset and position it shows (0 = none) */
void d3d_set_prov(const uint32_t *p, int w, int h);
const char *d3d_adapter_name(void);
#endif
