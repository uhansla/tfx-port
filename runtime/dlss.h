/* NVIDIA DLSS Super Resolution and Frame Generation through Streamline
 * (manual hooking), see dlss.cpp and RENDERER.md, stage 7.  C interface for
 * d3d12.c. */
#ifndef DLSS_H
#define DLSS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* before the D3D12 device/swap chain exist: verify and load
 * sl.interposer.dll next to the exe, slInit (manual hooking) with DLSS (sr)
 * and/or DLSS-G + Reflex (fg).  0 = ok */
int dlss_preinit(int sr, int fg);
/* slUpgradeInterface: the proxy for a DXGI factory / D3D12 device (or the
 * interface itself when Streamline isn't active) */
void *dlss_upgrade(void *iface);
/* slSetD3DDevice with the native device, then check DLSS / DLSS-G + Reflex
 * support on this adapter (LUID bytes).  0 = device set; then ask
 * dlss_sr_ok / dlss_fg_ok */
int dlss_set_device(void *native_device, const void *luid, uint32_t luid_size);
int dlss_sr_ok(void);
int dlss_fg_ok(void);
/* render size for render_scale (percent) at this output size: picks the
 * DLSS mode (100 DLAA, >=67 Quality, >=58 Balanced, else Performance) and
 * asks DLSS for its optimal render resolution.  0 = ok */
int dlss_render_size(int scale_pct, int ow, int oh, int *rw, int *rh);

/* one Streamline frame per present (all of the below use its token) */
void dlss_frame_begin(void);                  /* token, Reflex sleep, simulation markers */
enum { DLSS_MARK_SUBMIT_START = 2, DLSS_MARK_SUBMIT_END = 3, DLSS_MARK_PRESENT_START = 4, DLSS_MARK_PRESENT_END = 5 };
void dlss_marker(int marker);                 /* PCL marker (with DLSS-G) */
/* common constants, once per frame: jitter in render pixels, motion in
 * render pixels (previous - current), moving = 0 for a re-presented scene */
void dlss_constants(int rw, int rh, int ow, int oh, float jx, float jy, int moving, int reset);
/* tag the inputs and evaluate DLSS into out (command list cl), after
 * dlss_constants.  States are D3D12_RESOURCE_STATES at the evaluation. */
int dlss_evaluate(void *cl, void *color, uint32_t color_state, void *depth, uint32_t depth_state,
                  void *motion, uint32_t motion_state, void *out, uint32_t out_state,
                  int rw, int rh, int ow, int oh);

/* DLSS Frame Generation: generated frames per presented frame (0 = off),
 * from the next Present; clamped to the GPU's maximum */
int dlss_fg_set(int gen);
int dlss_fg_on(void);
/* its inputs for this frame, in the states they will have at Present:
 * depth and motion at render size, HUD-less colour and UI alpha at the
 * backbuffer size */
int dlss_fg_tag(void *cl, void *depth, uint32_t depth_state, void *motion, uint32_t motion_state,
                void *hudless, uint32_t hudless_state, void *ui, uint32_t ui_state,
                int rw, int rh, int ow, int oh);
void dlss_fg_untag(void *cl);                 /* inputs not valid (menus) */
void dlss_fg_poll(void);                      /* log its status now and then */

void dlss_shutdown(void);
const char *dlss_mode_name(void);

#ifdef __cplusplus
}
#endif
#endif
