/* Direct3D 12 presenter (RENDERER.md, stage 2).
 *
 * SDL keeps the window, input and audio; this module owns the swap chain on
 * the window's HWND.  Each present uploads the game's frame (BGRA) to a
 * texture and draws it into the destination rectangle with a full-screen
 * triangle: point or linear sampling, or "sharp" (integer nearest upscale to
 * an intermediate target, then linear).  Overlay lines (the native
 * renderer's debug view) use a second pipeline.  The native scene renderer
 * (stages 3-4) draws its triangles between the game's picture and the
 * masked UI pass; Gouraud and textured triangles look up the game's own
 * texture windows, remap tables and palette (SRV slots 5-7).
 */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <SDL.h>
#include <SDL_syswm.h>
#include "rt.h"
#include "settings.h"
#include "present.h"
#include "art.h"
#include "native.h"
#include "fsr.h"
#include "dlss.h"
#include "hud.h"

#define NBUF 3
#define MAX_LINE_VERTS 262144

static ID3D12Device *dev;
static ID3D12CommandQueue *queue;
static IDXGISwapChain3 *swap;
static ID3D12DescriptorHeap *rtv_heap, *srv_heap;
static UINT rtv_inc, srv_inc;
static ID3D12Resource *backbuf[NBUF];
static ID3D12CommandAllocator *alloc[NBUF];
static ID3D12GraphicsCommandList *cl;
static ID3D12Fence *fence;
static UINT64 fence_val[NBUF], fence_next = 1;
static HANDLE fence_event;
static ID3D12RootSignature *rootsig;
static ID3D12PipelineState *pso_blit, *pso_blit_rt, *pso_lines, *pso_scene, *pso_masked;
static ID3D12DescriptorHeap *dsv_heap;
static ID3D12Resource *depth;                    /* scene depth (output size) */
static ID3D12Resource *motion;                   /* scene motion vectors, R16G16_FLOAT, output pixels (stage 5) */
/* upscaler slot (stage 7): scene targets at render resolution, history at output resolution */
static ID3D12Resource *up_color, *up_depth, *up_motion, *hist[2];
static int up_rw, up_rh, hist_w, hist_h, hist_cur, hist_valid;
static unsigned up_frame;
static ID3D12PipelineState *pso_taa, *pso_comp;
static ID3D12Resource *fsr_out;                  /* FSR output (UAV), output resolution */
static int fsr_state;                            /* 0 untried, 1 ready, -1 unavailable (taa used) */
static int fsr_w, fsr_h;
static int dlss_state;                           /* 1 Streamline + DLSS ready, 0 not used, -1 failed (taa used) */
static int sl_active;                            /* Streamline is initialised (DLSS and/or frame generation) */
static int fg_state;                             /* DLSS Frame Generation: 1 ready, 0 not asked for, -1 unavailable */
static int fg_hold;                              /* a resize waits one present with frame generation off */
static ID3D12Resource *hudless, *ui_alpha;       /* its inputs at the backbuffer size (RTV NBUF+6 = ui_alpha) */
static int fg_w, fg_h;
static ID3D12PipelineState *pso_masked_ui;       /* the UI pass that also writes the UI alpha */
static ID3D12PipelineState *pso_mmpx;            /* ui_filter=mmpx: one 2x pass */
static ID3D12Resource *mmpx_rt[2];                /* 2x and 4x of the game frame (premultiplied) */
static int mmpx_w, mmpx_h;
/* ui_filter=remacri (art.h): per game pixel the art asset and position it
 * shows (R32_UINT, from vga.c), the atlas and the asset table */
static const uint32_t *prov_data;
static int prov_dw, prov_dh;
static ID3D12Resource *prov_tex, *prov_up[NBUF], *art_tex, *art_up, *tab_tex, *tab_up;
static int prov_w, prov_h, art_ver, art_h;
static UINT prov_pitch;
static void ensure_fg(int w, int h);
#define RD_ST (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
static LUID adapter_luid;
static int depth_w, depth_h;
static ID3D12Resource *mask_tex, *mask_up[NBUF];  /* 3D-pixel mask of the game picture */
static UINT mask_pitch;
static ID3D12Resource *tri_vb[NBUF];             /* native triangles, per frame */
static UINT64 tri_cap[NBUF];
/* native shading data: texture windows (array), remap tables, palette */
static ID3D12Resource *scene_tex, *scene_tab, *scene_pal, *res_up[NBUF];
#define RES_TAB_OFF ((UINT64)NATIVE_MAX_TEX * 65536)
#define RES_PAL_OFF (RES_TAB_OFF + (UINT64)NATIVE_MAX_TABLES * 256)
#define RES_SIZE (RES_PAL_OFF + 1024)
static HWND hwnd;
static int out_w, out_h, tearing;
static char adapter[128];

/* game frame texture + per-frame upload buffers */
static ID3D12Resource *frame_tex, *upload[NBUF];
static int tex_w, tex_h;
static UINT upload_pitch;
/* "sharp" intermediate target */
static ID3D12Resource *sharp_rt;
static int sharp_w, sharp_h;
/* overlay lines: per-frame upload vertex buffers */
static ID3D12Resource *line_vb[NBUF];
/* sharp HUD overlay (hud.c): vertices and atlas for this present, its
 * pipelines (the second also writes frame generation's UI alpha) and
 * resources; SRV 24/26 = atlas, 25 = the game's pixel mask */
static const struct hud_vertex *hud_v;
static int hud_n;
static const uint8_t *hud_at;
static ID3D12PipelineState *pso_hud, *pso_hud_ui;
static ID3D12Resource *hud_atlas, *hud_at_up[NBUF], *hud_vb[NBUF];
static UINT64 hud_vb_cap[NBUF];
/* window capture */
static ID3D12Resource *readback;
static UINT64 readback_size;

static const char *shader_src =
    "cbuffer C : register(b0) { float use_linear; float3 pad; float ui_xbr; float3 pad2; };\n"
    "Texture2D t : register(t0);\n"
    "Texture2D m : register(t1);\n"
    "Texture2D<uint> pv : register(t3);\n"
    "Texture2D at : register(t4);\n"
    "Texture2D<float4> tb : register(t5);\n"
    "SamplerState sp : register(s0);\n"
    "SamplerState sl : register(s1);\n"
    "struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VO vs_blit(uint id : SV_VertexID) {\n"
    "    VO o; float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    o.uv = uv; o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "float4 ps_blit(VO i) : SV_Target {\n"
    "    return use_linear > 0.5 ? t.Sample(sl, i.uv) : t.Sample(sp, i.uv); }\n"
    "struct LI { float2 pos : POSITION; float4 col : COLOR; };\n"
    "struct LO { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "LO vs_lines(LI i) { LO o; o.pos = float4(i.pos, 0, 1); o.col = i.col; return o; }\n"
    "float4 ps_lines(LO i) : SV_Target { return i.col; }\n"
    /* pad.x/pad.y: uv.x scale/offset, so a viewport wider than the picture
     * repeats its edge columns (clamped sampling): widescreen cockpit sides */
    "/* the game's UI layer upscaled with xBR (Hyllian's xBR level 2, single\n"
    " * pass, any scale), mask-aware: the game's 3D pixels are transparent (they\n"
    " * are drawn natively), so the cockpit's own edge against the world is\n"
    " * smoothed too and comes out as coverage.  Premultiplied colour */\n"
    "float4 ui_px(int2 p, int2 sz) {\n"
    "    p = clamp(p, int2(0, 0), sz - 1);\n"
    "    if (m.Load(int3(p, 0)).r > 0.25) return float4(0, 0, 0, 0);\n"
    "    return float4(t.Load(int3(p, 0)).rgb, 1);\n"
    "}\n"
    "float ui_y(float4 c) { return c.a > 0.5 ? dot(c.rgb, float3(14.352, 28.176, 5.472)) : -64.0; }\n"
    "float4 wdist(float4 a, float4 b, float4 c, float4 d, float4 e, float4 f, float4 g, float4 h) {\n"
    "    return abs(a - b) + abs(a - c) + abs(d - e) + abs(d - f) + 4.0 * abs(g - h);\n"
    "}\n"
    "float4 neq4(float4 a, float4 b) { return abs(a - b) >= 15.0 ? 1.0 : 0.0; }\n"
    "float4 eq4(float4 a, float4 b) { return abs(a - b) < 15.0 ? 1.0 : 0.0; }\n"
    "float4 ne4(float4 a, float4 b) { return a != b ? 1.0 : 0.0; }\n"
    "float4 xbr_ui(float2 uv, float du) {\n"
    "    uint W, H; t.GetDimensions(W, H);\n"
    "    int2 sz = int2(W, H);\n"
    "    float2 pos = uv * float2(W, H);\n"
    "    int2 q = int2(floor(pos));\n"
    "    float2 fp = pos - floor(pos);\n"
    "    float s = max(1.0, 1.0 / max(du * W, 1e-4));\n"
    "    float4 A1 = ui_px(q + int2(-1, -2), sz), B1 = ui_px(q + int2(0, -2), sz), C1 = ui_px(q + int2(1, -2), sz);\n"
    "    float4 A0 = ui_px(q + int2(-2, -1), sz), A = ui_px(q + int2(-1, -1), sz), B = ui_px(q + int2(0, -1), sz);\n"
    "    float4 C = ui_px(q + int2(1, -1), sz), C4 = ui_px(q + int2(2, -1), sz);\n"
    "    float4 D0 = ui_px(q + int2(-2, 0), sz), D = ui_px(q + int2(-1, 0), sz), E = ui_px(q, sz);\n"
    "    float4 F = ui_px(q + int2(1, 0), sz), F4 = ui_px(q + int2(2, 0), sz);\n"
    "    float4 G0 = ui_px(q + int2(-2, 1), sz), G = ui_px(q + int2(-1, 1), sz), Hh = ui_px(q + int2(0, 1), sz);\n"
    "    float4 I = ui_px(q + int2(1, 1), sz), I4 = ui_px(q + int2(2, 1), sz);\n"
    "    float4 G5 = ui_px(q + int2(-1, 2), sz), H5 = ui_px(q + int2(0, 2), sz), I5 = ui_px(q + int2(1, 2), sz);\n"
    "    float4 b = float4(ui_y(B), ui_y(D), ui_y(Hh), ui_y(F));\n"
    "    float4 c = float4(ui_y(C), ui_y(A), ui_y(G), ui_y(I));\n"
    "    float4 e = ui_y(E).xxxx;\n"
    "    float4 d = b.yzwx, f = b.wxyz, g = c.zwxy, h = b.zwxy, i = c.wxyz;\n"
    "    float4 i4 = float4(ui_y(I4), ui_y(C1), ui_y(A0), ui_y(G5));\n"
    "    float4 i5 = float4(ui_y(I5), ui_y(C4), ui_y(A1), ui_y(G0));\n"
    "    float4 h5 = float4(ui_y(H5), ui_y(F4), ui_y(B1), ui_y(D0));\n"
    "    float4 f4 = h5.yzwx;\n"
    "    const float4 Ao = float4(1, -1, -1, 1), Bo = float4(1, 1, -1, -1), Co = float4(1.5, 0.5, -0.5, 0.5);\n"
    "    const float4 Ax = float4(1, -1, -1, 1), Bx = float4(0.5, 2, -0.5, -2), Cx = float4(1, 1, -0.5, 0);\n"
    "    const float4 Ay = float4(1, -1, -1, 1), By = float4(2, 0.5, -2, -0.5), Cy = float4(2, 0, -1, 0.5);\n"
    "    const float4 Ci = float4(0.25, 0.25, 0.25, 0.25);\n"
    "    float4 fx = Ao * fp.y + Bo * fp.x, fx_left = Ax * fp.y + Bx * fp.x, fx_up = Ay * fp.y + By * fp.x;\n"
    "    float4 lv0 = ne4(e, f) * ne4(e, h);\n"
    "    float4 lv1 = lv0 * saturate(neq4(f, b) * neq4(h, d) + eq4(e, i) * neq4(f, i4) * neq4(h, i5) + eq4(e, g) + eq4(e, c));\n"
    "    float4 lv2_left = ne4(e, g) * ne4(d, g), lv2_up = ne4(e, c) * ne4(b, c);\n"
    "    float4 delta = (1.0 / s).xxxx;\n"
    "    float4 deltaL = float4(0.5, 1.0, 0.5, 1.0) / s, deltaU = deltaL.yxwz;\n"
    "    float4 fx45i = saturate((fx + delta - Co - Ci) / (2 * delta));\n"
    "    float4 fx45 = saturate((fx + delta - Co) / (2 * delta));\n"
    "    float4 fx30 = saturate((fx_left + deltaL - Cx) / (2 * deltaL));\n"
    "    float4 fx60 = saturate((fx_up + deltaU - Cy) / (2 * deltaU));\n"
    "    float4 wd1 = wdist(e, c, g, i, h5, f4, h, f), wd2 = wdist(h, d, i5, f, i4, b, e, i);\n"
    "    float4 edri = (wd1 <= wd2 ? 1.0 : 0.0) * lv0;\n"
    "    float4 edr = (wd1 < wd2 ? 1.0 : 0.0) * lv1;\n"
    "    float4 dfg = abs(f - g), dhc = abs(h - c);\n"
    "    float4 edr_left = (2.0 * dfg <= dhc ? 1.0 : 0.0) * lv2_left * edr;\n"
    "    float4 edr_up = (dfg >= 2.0 * dhc ? 1.0 : 0.0) * lv2_up * edr;\n"
    "    fx45 = edr * fx45; fx30 = edr_left * fx30; fx60 = edr_up * fx60; fx45i = edri * fx45i;\n"
    "    float4 px = abs(e - f) <= abs(e - h) ? 1.0 : 0.0;\n"
    "    float4 mx = max(max(fx30, fx60), max(fx45, fx45i));\n"
    "    float4 r1 = lerp(E, lerp(Hh, F, px.x), mx.x);\n"
    "    r1 = lerp(r1, lerp(B, D, px.z), mx.z);\n"
    "    float4 r2 = lerp(E, lerp(F, B, px.y), mx.y);\n"
    "    r2 = lerp(r2, lerp(D, Hh, px.w), mx.w);\n"
    "    float d1 = dot(abs(E - r1), 1.0), d2 = dot(abs(E - r2), 1.0);\n"
    "    return d2 >= d1 ? r2 : r1;\n"
    "}\n"
    "/* MMPX (McGuire and Gagiu 2021): 2x pixel-art magnification by exact colour\n"
    " * rules; run twice for the UI layer (ui_filter=mmpx).  Pass 1 reads the\n"
    " * game's frame with the 3D pixels transparent (mask), pass 2 its output.\n"
    " * Transparent stays (0,0,0,0), so the result is premultiplied */\n"
    "float4 mp_px(int2 p, int2 sz) {\n"
    "    p = clamp(p, int2(0, 0), sz - 1);\n"
    "    float4 c = t.Load(int3(p, 0));\n"
    "    if (pad2.x > 0.5) return m.Load(int3(p, 0)).r > 0.25 ? float4(0, 0, 0, 0) : float4(c.rgb, 1);\n"
    "    return c;\n"
    "}\n"
    "bool mp_eq(float4 a, float4 b) { return all(a == b); }\n"
    "float mp_l(float4 c) { return dot(c.rgb, float3(0.299, 0.587, 0.114)); }\n"
    "bool mp_all3(float4 b, float4 a0, float4 a1, float4 a2) { return mp_eq(b, a0) && mp_eq(b, a1) && mp_eq(b, a2); }\n"
    "bool mp_all4(float4 b, float4 a0, float4 a1, float4 a2, float4 a3) { return mp_all3(b, a0, a1, a2) && mp_eq(b, a3); }\n"
    "bool mp_any3(float4 b, float4 a0, float4 a1, float4 a2) { return mp_eq(b, a0) || mp_eq(b, a1) || mp_eq(b, a2); }\n"
    "bool mp_none2(float4 b, float4 a0, float4 a1) { return !mp_eq(b, a0) && !mp_eq(b, a1); }\n"
    "bool mp_none4(float4 b, float4 a0, float4 a1, float4 a2, float4 a3) { return mp_none2(b, a0, a1) && mp_none2(b, a2, a3); }\n"
    "float4 ps_mmpx(VO i) : SV_Target {\n"
    "    uint W, H; t.GetDimensions(W, H);\n"
    "    int2 sz = int2(W, H);\n"
    "    int2 o = int2(i.pos.xy), q = o >> 1, qd = o & 1;\n"
    "    float4 A = mp_px(q + int2(-1, -1), sz), B = mp_px(q + int2(0, -1), sz), C = mp_px(q + int2(1, -1), sz);\n"
    "    float4 D = mp_px(q + int2(-1, 0), sz), E = mp_px(q, sz), F = mp_px(q + int2(1, 0), sz);\n"
    "    float4 G = mp_px(q + int2(-1, 1), sz), Hh = mp_px(q + int2(0, 1), sz), I = mp_px(q + int2(1, 1), sz);\n"
    "    float4 J = E, K = E, L = E, M = E;\n"
    "    if (!(mp_eq(A, E) && mp_eq(B, E) && mp_eq(C, E) && mp_eq(D, E) && mp_eq(F, E) && mp_eq(G, E) && mp_eq(Hh, E) && mp_eq(I, E))) {\n"
    "        float4 P = mp_px(q + int2(0, -2), sz), S = mp_px(q + int2(0, 2), sz), Q = mp_px(q + int2(-2, 0), sz), R = mp_px(q + int2(2, 0), sz);\n"
    "        float Bl = mp_l(B), Dl = mp_l(D), El = mp_l(E), Fl = mp_l(F), Hl = mp_l(Hh);\n"
    "        if (mp_eq(D, B) && !mp_eq(D, Hh) && !mp_eq(D, F) && (El >= Dl || mp_eq(E, A)) && mp_any3(E, A, C, G) && (El < Dl || !mp_eq(A, D) || !mp_eq(E, P) || !mp_eq(E, Q))) J = D;\n"
    "        if (mp_eq(B, F) && !mp_eq(B, D) && !mp_eq(B, Hh) && (El >= Bl || mp_eq(E, C)) && mp_any3(E, A, C, I) && (El < Bl || !mp_eq(C, B) || !mp_eq(E, P) || !mp_eq(E, R))) K = B;\n"
    "        if (mp_eq(Hh, D) && !mp_eq(Hh, F) && !mp_eq(Hh, B) && (El >= Hl || mp_eq(E, G)) && mp_any3(E, A, G, I) && (El < Hl || !mp_eq(G, Hh) || !mp_eq(E, S) || !mp_eq(E, Q))) L = Hh;\n"
    "        if (mp_eq(F, Hh) && !mp_eq(F, B) && !mp_eq(F, D) && (El >= Fl || mp_eq(E, I)) && mp_any3(E, C, G, I) && (El < Fl || !mp_eq(I, Hh) || !mp_eq(E, R) || !mp_eq(E, S))) M = F;\n"
    "        if (!mp_eq(E, F) && mp_all4(E, C, I, D, Q) && mp_eq(F, B) && mp_eq(F, Hh) && !mp_eq(F, mp_px(q + int2(3, 0), sz))) { K = F; M = F; }\n"
    "        if (!mp_eq(E, D) && mp_all4(E, A, G, F, R) && mp_eq(D, B) && mp_eq(D, Hh) && !mp_eq(D, mp_px(q + int2(-3, 0), sz))) { J = D; L = D; }\n"
    "        if (!mp_eq(E, Hh) && mp_all4(E, G, I, B, P) && mp_eq(Hh, D) && mp_eq(Hh, F) && !mp_eq(Hh, mp_px(q + int2(0, 3), sz))) { L = Hh; M = Hh; }\n"
    "        if (!mp_eq(E, B) && mp_all4(E, A, C, Hh, S) && mp_eq(B, D) && mp_eq(B, F) && !mp_eq(B, mp_px(q + int2(0, -3), sz))) { J = B; K = B; }\n"
    "        if (Bl < El && mp_all4(E, G, Hh, I, S) && mp_none4(E, A, D, C, F)) { J = B; K = B; }\n"
    "        if (Hl < El && mp_all4(E, A, B, C, P) && mp_none4(E, G, D, I, F)) { L = Hh; M = Hh; }\n"
    "        if (Fl < El && mp_all4(E, A, D, G, Q) && mp_none4(E, B, C, Hh, I)) { K = F; M = F; }\n"
    "        if (Dl < El && mp_all4(E, C, F, I, R) && mp_none4(E, B, A, Hh, G)) { J = D; L = D; }\n"
    "        if (!mp_eq(Hh, B)) {\n"
    "            if (!mp_eq(Hh, A) && !mp_eq(Hh, E) && !mp_eq(Hh, C)) {\n"
    "                if (mp_all3(Hh, G, F, R) && mp_none2(Hh, D, mp_px(q + int2(2, -1), sz))) L = M;\n"
    "                if (mp_all3(Hh, I, D, Q) && mp_none2(Hh, F, mp_px(q + int2(-2, -1), sz))) M = L;\n"
    "            }\n"
    "            if (!mp_eq(B, I) && !mp_eq(B, G) && !mp_eq(B, E)) {\n"
    "                if (mp_all3(B, A, F, R) && mp_none2(B, D, mp_px(q + int2(2, 1), sz))) J = K;\n"
    "                if (mp_all3(B, C, D, Q) && mp_none2(B, F, mp_px(q + int2(-2, 1), sz))) K = J;\n"
    "            }\n"
    "        }\n"
    "        if (!mp_eq(F, D)) {\n"
    "            if (!mp_eq(D, I) && !mp_eq(D, E) && !mp_eq(D, C)) {\n"
    "                if (mp_all3(D, A, Hh, S) && mp_none2(D, B, mp_px(q + int2(1, 2), sz))) J = L;\n"
    "                if (mp_all3(D, G, B, P) && mp_none2(D, Hh, mp_px(q + int2(1, -2), sz))) L = J;\n"
    "            }\n"
    "            if (!mp_eq(F, E) && !mp_eq(F, A) && !mp_eq(F, G)) {\n"
    "                if (mp_all3(F, C, Hh, S) && mp_none2(F, B, mp_px(q + int2(-1, 2), sz))) K = M;\n"
    "                if (mp_all3(F, I, B, P) && mp_none2(F, Hh, mp_px(q + int2(-1, -2), sz))) M = K;\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    return qd.y ? (qd.x ? M : L) : (qd.x ? K : J);\n"
    "}\n"
    "/* ui_filter=remacri: the MMPX picture, with the art pack's image where the\n"
    " * game pixel came from a known asset (tag: asset + 1 << 24, y << 12, x) */\n"
    "float4 art_ui(float2 uv) {\n"
    "    float4 r = t.Sample(sl, uv);\n"
    "    uint W, H; pv.GetDimensions(W, H);\n"
    "    float2 pos = uv * float2(W, H);\n"
    "    int2 q = clamp(int2(floor(pos)), int2(0, 0), int2(W, H) - 1);\n"
    "    float2 fq = pos - floor(pos);\n"
    "    uint tag = pv.Load(int3(q, 0)), a = tag >> 24;\n"
    "    if (a > 0) {\n"
    "        float4 e = tb.Load(int3(2 * (a - 1), 0, 0)), g = tb.Load(int3(2 * (a - 1) + 1, 0, 0));\n"
    "        float2 lp = float2(tag & 0xfff, (tag >> 12) & 0xfff);\n"
    "        if (lp.x < g.x && lp.y < g.y) {\n"
    "            uint AW, AH; at.GetDimensions(AW, AH);\n"
    "            float2 hp = e.xy + (lp + fq) * e.zw;\n"
    "            float4 c = at.SampleLevel(sl, hp / float2(AW, AH), 0);\n"
    "            r = float4(c.rgb * c.a, c.a) + r * (1 - c.a);\n"
    "        }\n"
    "    }\n"
    "    return r;\n"
    "}\n"
    "float4 ps_masked(VO i) : SV_Target {\n"
    "    float2 uv = float2(i.uv.x * pad.x + pad.y, i.uv.y);\n"
    "    float du = abs(ddx(uv.x));\n"
    "    if (ui_xbr > 2.5 && uv.x >= 0 && uv.x <= 1) {\n"
    "        float4 r = art_ui(uv);\n"
    "        if (r.a < 0.004) discard;\n"
    "        return float4(r.rgb / r.a, r.a); }\n"
    "    if (ui_xbr > 1.5 && uv.x >= 0 && uv.x <= 1) {\n"
    "        float4 r = t.Sample(sl, uv);\n"
    "        if (r.a < 0.004) discard;\n"
    "        return float4(r.rgb / r.a, r.a); }\n"
    "    if (ui_xbr > 0.5 && uv.x >= 0 && uv.x <= 1) {\n"
    "        float4 r = xbr_ui(uv, du);\n"
    "        if (r.a < 0.004) discard;\n"
    "        return float4(r.rgb / r.a, r.a); }\n"
    "    if (m.Sample(sp, uv).r > 0.25) discard;\n"
    /* beyond the picture: continue a row only where the UI is solid for 4
     * columns in from the edge (cockpit panels), not a line crossing it */
    "    if (uv.x < 0 || uv.x > 1) {\n"
    "        if (pad.z <= 0) discard;\n"     /* widescreen_sides=world: the 3D world only */
    "        float e = uv.x < 0 ? 0.5 * pad.z : 1 - 0.5 * pad.z, d = uv.x < 0 ? pad.z : -pad.z;\n"
    "        [unroll] for (int k = 1; k < 4; k++) if (m.Sample(sp, float2(e + d * k, uv.y)).r > 0.25) discard;\n"
    "        float4 c0 = t.Sample(sp, float2(e, uv.y));\n"
    "        [unroll] for (int j = 1; j < 4; j++) if (any(abs(t.Sample(sp, float2(e + d * j, uv.y)) - c0) > 0.2)) discard;\n"
    "    }\n"
    "    return float4((use_linear > 0.5 ? t.Sample(sl, uv) : t.Sample(sp, uv)).rgb, 1); }\n"
    /* the same, plus its coverage into the UI alpha target (DLSS Frame Generation) */
    "struct MO { float4 c : SV_Target0; float a : SV_Target1; };\n"
    "MO ps_masked_ui(VO i) { MO o; o.c = ps_masked(i); o.a = o.c.a; return o; }\n"
    "";

/* upscaler slot, the port's temporal upscaler (taa): per output pixel, the
 * jittered render samples of the 3x3 neighbourhood are weighted by their
 * distance (Gaussian, sigma ~0.47 render px) into the current colour; the
 * history is fetched where the motion vector says this point was, clamped to
 * the neighbourhood's range (ghosting) and blended.  Premultiplied alpha:
 * coverage of the native scene accumulates with the colour. */
static const char *taa_src =
    "cbuffer U : register(b0) { float2 outsize; float2 rsize; float2 jit; float mvscale; float reset; float4 pad; };\n"
    "Texture2D<float4> cur : register(t0);\n"
    "Texture2D<float2> mot : register(t1);\n"
    "Texture2D<float4> hist : register(t2);\n"
    "SamplerState sp : register(s0);\n"
    "SamplerState sl : register(s1);\n"
    "struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "float4 ps_taa(VO i) : SV_Target {\n"
    "    float2 q = i.uv * rsize;\n"
    "    int2 k = int2(floor(q + jit));\n"
    "    int2 lim = int2(rsize) - 1;\n"
    "    float4 c = 0, mn = 1e5, mx = -1e5; float ws = 0, wc = 0;\n"
    "    [unroll] for (int y = -1; y <= 1; y++) [unroll] for (int x = -1; x <= 1; x++) {\n"
    "        int2 t = clamp(k + int2(x, y), 0, lim);\n"
    "        float4 s = cur.Load(int3(t, 0));\n"
    "        float2 d = q - (float2(t) + 0.5 - jit);\n"
    "        float w = exp(-2.29 * dot(d, d));\n"
    "        c += s * w; ws += w; mn = min(mn, s); mx = max(mx, s);\n"
    "        if (x == 0 && y == 0) wc = w; }\n"
    "    c /= max(ws, 1e-5);\n"
    "    float2 mv = mot.Load(int3(clamp(k, 0, lim), 0)) * mvscale;\n"
    "    float2 huv = i.uv + mv / rsize;\n"
    "    if (reset > 0.5 || any(huv < 0) || any(huv > 1)) return c;\n"
    "    float4 h = clamp(hist.SampleLevel(sl, huv, 0), mn, mx);\n"
    "    float a = max(0.04, 0.12 * wc);\n"
    "    return lerp(h, c, a); }\n";
static const char *comp_src =
    "Texture2D<float4> src : register(t0);\n"

    "SamplerState sp : register(s0);\n"
    "struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "float4 ps_comp(VO i) : SV_Target { return src.Sample(sp, i.uv); }\n";

/* sharp HUD (hud.c): lines with anti-aliased sides and ends, bitmaps from
 * the R8 atlas thresholded smoothly; nothing where the game's final pixel is
 * UI (mask < 0.25), so later cockpit art still covers it */
static const char *hud_src =
    "Texture2D<float> atl : register(t0);\n"
    "Texture2D<float> msk : register(t1);\n"
    "SamplerState sp : register(s0);\n"
    "SamplerState sl : register(s1);\n"
    "cbuffer H : register(b0) { float4 view; float2 gsize; float2 asize; };\n"
    "struct HI { float2 pos : POSITION; float2 uv : TEXCOORD0; float2 ab : TEXCOORD1; float4 col : COLOR; float mode : TEXCOORD2; };\n"
    "struct HO { float4 pos : SV_Position; float2 uv : TEXCOORD0; float2 ab : TEXCOORD1; float4 col : COLOR;\n"
    "            nointerpolation float mode : TEXCOORD2; };\n"
    "HO vs_hud(HI i) { HO o; o.pos = float4(i.pos, 0, 1); o.uv = i.uv; o.ab = i.ab; o.col = i.col; o.mode = i.mode; return o; }\n"
    "bool on(int2 c) { return atl.Load(int3(c, 0)) > 0.5; }\n"
    "float cover(HO i) {\n"
    "    float a;\n"
    "    if (i.mode < 0.5) {\n"
    "        float across = abs(i.uv.x), along = max(max(-i.uv.y, i.uv.y - i.ab.y), 0);\n"
    "        a = saturate(i.ab.x + 0.5 - across) * saturate(i.ab.x + 0.5 - along);\n"
    "    } else {\n"
    /* the bitmap as strokes: each set pixel a point joined to its set
     * neighbours (a diagonal only where neither shared side neighbour is
     * set), half a game pixel either side; a corner of four set pixels is
     * solid.  Pixel-art lines and glyphs become smooth strokes of the same
     * weight */
    "        float2 p = i.uv; int2 c0 = int2(floor(p)), k = int2(round(p));\n"
    "        float d = 9;\n"
    "        if (on(k - 1) && on(k - int2(0, 1)) && on(k - int2(1, 0)) && on(k)) d = 0;\n"
    "        [loop] for (int y = -1; y <= 1; y++) [loop] for (int x = -1; x <= 1; x++) {\n"
    "            int2 c = c0 + int2(x, y);\n"
    "            if (!on(c)) continue;\n"
    "            float2 q = float2(c) + 0.5;\n"
    "            d = min(d, length(p - q));\n"
    "            [unroll] for (int j = 0; j < 9; j++) {\n"
    "                int2 o = int2(j % 3 - 1, j / 3 - 1);\n"
    "                if ((o.x == 0 && o.y == 0) || !on(c + o)) continue;\n"
    "                if (o.x != 0 && o.y != 0 && (on(c + int2(o.x, 0)) || on(c + int2(0, o.y)))) continue;\n"
    "                float2 e = float2(o) * 0.5, w = p - q;\n"
    "                float t = saturate(dot(w, e) / dot(e, e));\n"
    "                d = min(d, length(w - e * t)); }\n"
    "        }\n"
    "        float fw = max(fwidth(p.x), 1e-4);\n"
    "        a = saturate((0.5 - d) / fw + 0.5);\n"
    "    }\n"
    "    int2 g = clamp(int2(floor((i.pos.xy - view.xy) / view.zw)), int2(0, 0), int2(gsize) - 1);\n"
    "    if (msk.Load(int3(g, 0)) < 0.25 || a <= 0.003) discard;\n"
    "    return a; }\n"
    "float4 ps_hud(HO i) : SV_Target { float a = cover(i); return float4(i.col.rgb, a); }\n"
    "struct HO2 { float4 c : SV_Target0; float a : SV_Target1; };\n"
    "HO2 ps_hud_ui(HO i) { HO2 o; float a = cover(i); o.c = float4(i.col.rgb, a); o.a = a; return o; }\n";

/* native scene: flat colour, or the game's lookups (native.h modes) */
static const char *scene_src =
    "Texture2DArray<uint> stex : register(t0);\n"
    "Texture2D<uint> stab : register(t1);\n"
    "Texture2D<float4> spal : register(t2);\n"
    "cbuffer C : register(b0) { float2 outsize; float show_motion; float pad; float2 jit_ndc; float2 jit_px; };\n"
    "struct SI { float4 pos : POSITION; float4 col : COLOR; float2 uv : TEXCOORD0; uint info : TEXCOORD1; float4 clip : TEXCOORD2;\n"
    "            float4 prev : TEXCOORD3; };\n"
    "struct SO { float4 pos : SV_Position; float4 col : COLOR; float2 uv : TEXCOORD0; nointerpolation uint info : TEXCOORD1;\n"
    "            float4 clip : SV_ClipDistance0; float3 prev : TEXCOORD3; };\n"
    "struct PO { float4 col : SV_Target0; float2 mv : SV_Target1; };\n"
    "SO vs_scene(SI i) { SO o; o.pos = i.pos; o.col = i.col; o.uv = i.uv; o.info = i.info; o.clip = i.clip;\n"
    "    o.prev = i.prev.xyz; o.pos.xy += jit_ndc * i.pos.w; return o; }\n"
    "float4 shade(SO i) {\n"
    "    uint mode = i.info & 3, tab = (i.info >> 8) & 255, layer = (i.info >> 16) & 255;\n"
    "    if (mode == 0) return i.col;\n"
    "    uint idx;\n"
    "    if (mode == 1) idx = stab.Load(int3(clamp((int)floor(i.uv.x), 0, 255), tab, 0));\n"
    "    else {\n"
    "        uint t = stex.Load(int4((int)floor(i.uv.x) & 255, (int)floor(i.uv.y) & 255, layer, 0));\n"
    "        idx = stab.Load(int3(t, tab, 0));\n"
    "        if (idx == 0) discard;\n"     /* the game skips index 0 (transparent) */
    "    }\n"
    "    return spal.Load(int3(idx, 0, 0)); }\n"
    /* motion = where this surface point was in the previous scene frame minus
     * where it is now, in output pixels (prev clip x/y/w interpolated
     * perspective-correctly, then divided per pixel) */
    "PO ps_scene(SO i) {\n"
    "    PO o; o.col = shade(i); o.mv = float2(0, 0);\n"
    "    bool ok = (i.info & 0x1000000) != 0 && i.prev.z > 1e-4;\n"
    "    if (ok) {\n"
    "        float2 ndc = i.prev.xy / i.prev.z;\n"
    "        float2 pp = float2((ndc.x * 0.5 + 0.5) * outsize.x, (0.5 - ndc.y * 0.5) * outsize.y);\n"
    "        o.mv = pp - (i.pos.xy - jit_px); }\n"
    "    if (show_motion > 0.5)\n"      /* debug: grey = still, red/green = x/y motion, blue = no history */
    "        o.col = ok ? float4(saturate(0.5 + o.mv.x / 32), saturate(0.5 + o.mv.y / 32), 0.5, 1) : float4(0.1, 0.1, 0.8, 1);\n"
    "    return o; }\n";

static int fail(const char *what, HRESULT hr) {
    rt_log("d3d12: %s failed (hr=%08lx)", what, (unsigned long)hr);
    return -1;
}

static ID3DBlob *compile_src(const char *src, const char *entry, const char *target);
static ID3DBlob *compile(const char *entry, const char *target) { return compile_src(shader_src, entry, target); }
static ID3DBlob *compile_src(const char *src, const char *entry, const char *target) {
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), "tfx", NULL, NULL, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        rt_log("d3d12: shader %s: %s", entry, err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

static void wait_frame(UINT i) {
    if (ID3D12Fence_GetCompletedValue(fence) < fence_val[i]) {
        ID3D12Fence_SetEventOnCompletion(fence, fence_val[i], fence_event);
        WaitForSingleObject(fence_event, INFINITE);
    }
}

static void wait_idle(void) {
    UINT64 v = fence_next++;
    ID3D12CommandQueue_Signal(queue, fence, v);
    if (ID3D12Fence_GetCompletedValue(fence) < v) {
        ID3D12Fence_SetEventOnCompletion(fence, v, fence_event);
        WaitForSingleObject(fence_event, INFINITE);
    }
}

static D3D12_CPU_DESCRIPTOR_HANDLE rtv(int i) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
    h.ptr += (SIZE_T)i * rtv_inc;
    return h;
}
static D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu(int i) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(srv_heap);
    h.ptr += (SIZE_T)i * srv_inc;
    return h;
}
static D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu(int i) {
    D3D12_GPU_DESCRIPTOR_HANDLE h = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(srv_heap);
    h.ptr += (UINT64)i * srv_inc;
    return h;
}

static ID3D12Resource *buffer(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp = {type};
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *r = NULL;
    if (FAILED(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, NULL,
                                                    &IID_ID3D12Resource, (void **)&r))) return NULL;
    return r;
}

static ID3D12Resource *texture_fmt(int w, int h, int rt, D3D12_RESOURCE_STATES state, DXGI_FORMAT fmt);
static ID3D12Resource *texture(int w, int h, int rt, D3D12_RESOURCE_STATES state) {
    return texture_fmt(w, h, rt, state, DXGI_FORMAT_B8G8R8A8_UNORM);
}
static ID3D12Resource *texture_fmt(int w, int h, int rt, D3D12_RESOURCE_STATES state, DXGI_FORMAT fmt) {
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = (UINT64)w; rd.Height = (UINT)h;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = fmt; rd.SampleDesc.Count = 1;
    rd.Flags = rt == 2 ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : rt ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
    D3D12_CLEAR_VALUE cv = {fmt, {0, 0, 0, 1}};
    if (rt == 2) { cv.DepthStencil.Depth = 1.0f; cv.DepthStencil.Stencil = 0; }
    ID3D12Resource *r = NULL;
    if (FAILED(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, state, rt ? &cv : NULL,
                                                    &IID_ID3D12Resource, (void **)&r))) return NULL;
    return r;
}

static void make_srv_fmt(ID3D12Resource *r, int slot, DXGI_FORMAT fmt);
static void make_srv(ID3D12Resource *r, int slot) { make_srv_fmt(r, slot, DXGI_FORMAT_B8G8R8A8_UNORM); }
static void make_srv_fmt(ID3D12Resource *r, int slot, DXGI_FORMAT fmt) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
    sd.Format = fmt; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sd.Texture2D.MipLevels = 1;
    ID3D12Device_CreateShaderResourceView(dev, r, &sd, srv_cpu(slot));
}

static int create_backbuffers(void) {
    for (int i = 0; i < NBUF; i++) {
        if (FAILED(IDXGISwapChain3_GetBuffer(swap, (UINT)i, &IID_ID3D12Resource, (void **)&backbuf[i])))
            return fail("GetBuffer", E_FAIL);
        ID3D12Device_CreateRenderTargetView(dev, backbuf[i], NULL, rtv(i));
    }
    return 0;
}

static void client_size(int *w, int *h) {
    RECT r; GetClientRect(hwnd, &r);
    *w = r.right - r.left; *h = r.bottom - r.top;
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

static int pipelines(void) {
    /* root signature: b0 = 4 constants, t0 = descriptor table; s0 point, s1 linear */
    D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 6, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
    D3D12_ROOT_PARAMETER params[2];
    memset(params, 0, sizeof params);
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 12;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;      /* the scene VS reads the jitter */
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC samp[2];
    memset(samp, 0, sizeof samp);
    for (int i = 0; i < 2; i++) {
        samp[i].Filter = i ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
        samp[i].AddressU = samp[i].AddressV = samp[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp[i].MaxLOD = D3D12_FLOAT32_MAX; samp[i].ShaderRegister = (UINT)i;
        samp[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd = {2, params, 2, samp, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ID3DBlob *blob = NULL, *err = NULL;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) return fail("D3D12SerializeRootSignature", hr);
    hr = ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                                          &IID_ID3D12RootSignature, (void **)&rootsig);
    ID3D10Blob_Release(blob);
    if (err) ID3D10Blob_Release(err);
    if (FAILED(hr)) return fail("CreateRootSignature", hr);

    ID3DBlob *vsb = compile("vs_blit", "vs_5_0"), *psb = compile("ps_blit", "ps_5_0");
    ID3DBlob *vsl = compile("vs_lines", "vs_5_0"), *psl = compile("ps_lines", "ps_5_0");
    if (!vsb || !psb || !vsl || !psl) return -1;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd;
    memset(&pd, 0, sizeof pd);
    pd.pRootSignature = rootsig;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vsb); pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vsb);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(psb); pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(psb);
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = 0xffffffffu;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM; pd.SampleDesc.Count = 1;
    hr = ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso_blit);
    if (FAILED(hr)) return fail("CreateGraphicsPipelineState (blit)", hr);
    pso_blit_rt = pso_blit;                       /* same formats for the intermediate target */

    static D3D12_INPUT_ELEMENT_DESC il[2] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    pd.InputLayout.pInputElementDescs = il; pd.InputLayout.NumElements = 2;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vsl); pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vsl);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(psl); pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(psl);
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
    D3D12_RENDER_TARGET_BLEND_DESC *b = &pd.BlendState.RenderTarget[0];
    b->BlendEnable = TRUE; b->SrcBlend = D3D12_BLEND_SRC_ALPHA; b->DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    b->BlendOp = D3D12_BLEND_OP_ADD; b->SrcBlendAlpha = D3D12_BLEND_ONE; b->DestBlendAlpha = D3D12_BLEND_ZERO;
    b->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    hr = ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso_lines);
    if (FAILED(hr)) return fail("CreateGraphicsPipelineState (lines)", hr);
    /* native scene: clip-space float4 + colour, depth written, painter's order (depth test always passes) */
    ID3DBlob *vss = compile_src(scene_src, "vs_scene", "vs_5_0"), *pss = compile_src(scene_src, "ps_scene", "ps_5_0");
    ID3DBlob *psm = compile("ps_masked", "ps_5_0");
    if (!vss || !pss || !psm) return -1;
    static D3D12_INPUT_ELEMENT_DESC il2[6] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32_UINT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    pd.InputLayout.pInputElementDescs = il2; pd.InputLayout.NumElements = 6;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vss); pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vss);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(pss); pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(pss);
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    b->BlendEnable = FALSE;
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd.NumRenderTargets = 2; pd.RTVFormats[1] = DXGI_FORMAT_R16G16_FLOAT;      /* colour + motion */
    pd.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    /* colour blends by the primitive's alpha: 1 for everything except the
     * game's stippled cloud puffs (50 % / 25 %, native.c disc_alpha); alpha
     * itself accumulates as coverage.  Motion is written as it is */
    pd.BlendState.IndependentBlendEnable = TRUE;
    b->BlendEnable = TRUE; b->SrcBlend = D3D12_BLEND_SRC_ALPHA; b->DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    b->BlendOp = D3D12_BLEND_OP_ADD; b->SrcBlendAlpha = D3D12_BLEND_ONE; b->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    b->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[1].BlendEnable = FALSE;
    hr = ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso_scene);
    if (FAILED(hr)) return fail("CreateGraphicsPipelineState (scene)", hr);
    pd.BlendState.IndependentBlendEnable = FALSE;
    b->BlendEnable = FALSE; b->DestBlendAlpha = D3D12_BLEND_ZERO;
    pd.NumRenderTargets = 1; pd.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
    /* masked blit: the game's UI pixels over the native scene */
    memset(&pd.InputLayout, 0, sizeof pd.InputLayout);
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vsb); pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vsb);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(psm); pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(psm);
    memset(&pd.DepthStencilState, 0, sizeof pd.DepthStencilState);
    pd.DSVFormat = DXGI_FORMAT_UNKNOWN;
    /* blended by the UI pixel's alpha: 1, or the xBR filter's coverage at edges */
    b->BlendEnable = TRUE; b->SrcBlend = D3D12_BLEND_SRC_ALPHA; b->DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    b->SrcBlendAlpha = D3D12_BLEND_ONE; b->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    hr = ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso_masked);
    if (FAILED(hr)) return fail("CreateGraphicsPipelineState (masked)", hr);
    {   /* masked + UI alpha (R8) for frame generation */
        ID3DBlob *psu = compile("ps_masked_ui", "ps_5_0");
        if (!psu) return -1;
        pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(psu); pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(psu);
        pd.NumRenderTargets = 2; pd.RTVFormats[1] = DXGI_FORMAT_R8_UNORM;
        pd.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        hr = ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso_masked_ui);
        if (FAILED(hr)) return fail("CreateGraphicsPipelineState (masked ui)", hr);
        pd.NumRenderTargets = 1; pd.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
        pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(psm); pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(psm);
    }
    b->BlendEnable = FALSE; b->DestBlendAlpha = D3D12_BLEND_ZERO;
    {   /* ui_filter=mmpx: a 2x MMPX pass into a B8G8R8A8 target, no blending */
        ID3DBlob *psx = compile("ps_mmpx", "ps_5_0");
        if (!psx) return -1;
        pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(psx); pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(psx);
        hr = ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso_mmpx);
        if (FAILED(hr)) return fail("CreateGraphicsPipelineState (mmpx)", hr);
        pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(psm); pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(psm);
    }
    /* upscaler: taa resolve into the FP16 history, then the composite (premultiplied alpha) */
    {
        ID3DBlob *pst = compile_src(taa_src, "ps_taa", "ps_5_0"), *psc = compile_src(comp_src, "ps_comp", "ps_5_0");
        if (!pst || !psc) return -1;
        pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(pst); pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(pst);
        pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        hr = ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso_taa);
        if (FAILED(hr)) return fail("CreateGraphicsPipelineState (taa)", hr);
        pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(psc); pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(psc);
        pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
        D3D12_RENDER_TARGET_BLEND_DESC *cb = &pd.BlendState.RenderTarget[0];
        cb->BlendEnable = TRUE; cb->SrcBlend = D3D12_BLEND_ONE; cb->DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        cb->BlendOp = D3D12_BLEND_OP_ADD; cb->SrcBlendAlpha = D3D12_BLEND_ONE; cb->DestBlendAlpha = D3D12_BLEND_ZERO;
        cb->BlendOpAlpha = D3D12_BLEND_OP_ADD;
        hr = ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso_comp);
        if (FAILED(hr)) return fail("CreateGraphicsPipelineState (composite)", hr);

        cb->BlendEnable = FALSE;
        ID3D10Blob_Release(pst); ID3D10Blob_Release(psc);
    }
    ID3D10Blob_Release(vsb); ID3D10Blob_Release(psb); ID3D10Blob_Release(vsl); ID3D10Blob_Release(psl);
    {   /* sharp HUD overlay: alpha blended over the picture; a variant that also
         * accumulates coverage into frame generation's UI alpha */
        ID3DBlob *hv = compile_src(hud_src, "vs_hud", "vs_5_0"), *hp = compile_src(hud_src, "ps_hud", "ps_5_0"),
                 *hp2 = compile_src(hud_src, "ps_hud_ui", "ps_5_0");
        if (!hv || !hp || !hp2) return -1;
        static D3D12_INPUT_ELEMENT_DESC il3[5] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 2, DXGI_FORMAT_R32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        pd.InputLayout.pInputElementDescs = il3; pd.InputLayout.NumElements = 5;
        pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(hv); pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(hv);
        pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(hp); pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(hp);
        pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM; pd.NumRenderTargets = 1;
        D3D12_RENDER_TARGET_BLEND_DESC *hb = &pd.BlendState.RenderTarget[0];
        hb->BlendEnable = TRUE; hb->SrcBlend = D3D12_BLEND_SRC_ALPHA; hb->DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        hb->BlendOp = D3D12_BLEND_OP_ADD; hb->SrcBlendAlpha = D3D12_BLEND_ONE; hb->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        hb->BlendOpAlpha = D3D12_BLEND_OP_ADD; hb->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        hr = ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso_hud);
        if (FAILED(hr)) return fail("CreateGraphicsPipelineState (hud)", hr);
        pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(hp2); pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(hp2);
        pd.NumRenderTargets = 2; pd.RTVFormats[1] = DXGI_FORMAT_R8_UNORM;
        pd.BlendState.IndependentBlendEnable = TRUE;
        D3D12_RENDER_TARGET_BLEND_DESC *hb1 = &pd.BlendState.RenderTarget[1];
        hb1->BlendEnable = TRUE; hb1->SrcBlend = D3D12_BLEND_ONE; hb1->DestBlend = D3D12_BLEND_INV_SRC_COLOR;
        hb1->BlendOp = D3D12_BLEND_OP_ADD; hb1->SrcBlendAlpha = D3D12_BLEND_ONE; hb1->DestBlendAlpha = D3D12_BLEND_ZERO;
        hb1->BlendOpAlpha = D3D12_BLEND_OP_ADD; hb1->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        hr = ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso_hud_ui);
        if (FAILED(hr)) return fail("CreateGraphicsPipelineState (hud ui)", hr);
        pd.BlendState.IndependentBlendEnable = FALSE; hb->BlendEnable = FALSE; hb1->BlendEnable = FALSE;
        pd.NumRenderTargets = 1; pd.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
        ID3D10Blob_Release(hv); ID3D10Blob_Release(hp); ID3D10Blob_Release(hp2);
    }
    ID3D10Blob_Release(vss); ID3D10Blob_Release(pss); ID3D10Blob_Release(psm);
    return 0;
}

int d3d_init(SDL_Window *win) {
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(win, &wm) || wm.subsystem != SDL_SYSWM_WINDOWS) return fail("SDL_GetWindowWMInfo", E_FAIL);
    hwnd = wm.info.win.window;
    if (getenv("TFX_D3D_DEBUG")) {
        ID3D12Debug *dbg = NULL;
        if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12Debug, (void **)&dbg))) { ID3D12Debug_EnableDebugLayer(dbg); ID3D12Debug_Release(dbg); }
    }
    /* upscaler=dlss: Streamline (manual hooking) must be initialised before
     * the swap chain and command queue are created (dlss.cpp) */
    int want_sr = settings.upscaler == UPSCALER_DLSS, want_fg = settings.frame_gen > 0;
    int sl_on = (want_sr || want_fg) && !dlss_preinit(want_sr, want_fg);
    if (want_sr && !sl_on) { dlss_state = -1; rt_log("dlss: unavailable, using taa"); }
    if (want_fg && !sl_on) { fg_state = -1; rt_log("dlss-g: unavailable, frame generation off"); }
    IDXGIFactory4 *fac = NULL;
    HRESULT hr = CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&fac);
    if (FAILED(hr)) return fail("CreateDXGIFactory2", hr);
    /* first hardware adapter that supports D3D12 */
    IDXGIAdapter1 *ad = NULL;
    for (UINT i = 0; IDXGIFactory4_EnumAdapters1(fac, i, &ad) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc;
        IDXGIAdapter1_GetDesc1(ad, &desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(D3D12CreateDevice((IUnknown *)ad, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev))) {
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, adapter, sizeof adapter, NULL, NULL);
            adapter_luid = desc.AdapterLuid;
            IDXGIAdapter1_Release(ad);
            break;
        }
        IDXGIAdapter1_Release(ad);
    }
    if (!dev) { IDXGIFactory4_Release(fac); return fail("D3D12CreateDevice (no D3D12 GPU)", E_FAIL); }
    {
        IDXGIFactory5 *f5 = NULL; BOOL t = FALSE;
        if (SUCCEEDED(IDXGIFactory4_QueryInterface(fac, &IID_IDXGIFactory5, (void **)&f5))) {
            if (SUCCEEDED(IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &t, sizeof t))) tearing = t;
            IDXGIFactory5_Release(f5);
        }
    }
    /* Streamline needs the device before any hooked call (queue, swap chain) */
    if (sl_on) {
        int dev_ok = !dlss_set_device(dev, &adapter_luid, sizeof adapter_luid);
        if (want_sr) dlss_state = dev_ok && dlss_sr_ok() ? 1 : -1;
        if (want_fg) fg_state = dev_ok && dlss_fg_ok() ? 1 : -1;
        if (dlss_state < 0) rt_log("dlss: unavailable, using taa");
        if (fg_state < 0) rt_log("dlss-g: unavailable, frame generation off");
        sl_active = 1;
    }
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    {   /* with Streamline: queue from its device proxy (a hooked call), native device elsewhere */
        ID3D12Device *qdev = sl_on ? (ID3D12Device *)dlss_upgrade(dev) : dev;
        if (FAILED(hr = ID3D12Device_CreateCommandQueue(qdev, &qd, &IID_ID3D12CommandQueue, (void **)&queue)))
            return fail("CreateCommandQueue", hr);
    }
    if (sl_on) fac = (IDXGIFactory4 *)dlss_upgrade(fac);    /* swap chain through Streamline's factory proxy */
    client_size(&out_w, &out_h);
    DXGI_SWAP_CHAIN_DESC1 sd = {0};
    sd.Width = (UINT)out_w; sd.Height = (UINT)out_h; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = NBUF; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    IDXGISwapChain1 *sc1 = NULL;
    hr = IDXGIFactory4_CreateSwapChainForHwnd(fac, (IUnknown *)queue, hwnd, &sd, NULL, NULL, &sc1);
    if (FAILED(hr)) { IDXGIFactory4_Release(fac); return fail("CreateSwapChainForHwnd", hr); }
    IDXGIFactory4_MakeWindowAssociation(fac, hwnd, DXGI_MWA_NO_ALT_ENTER);   /* SDL handles Alt+Enter */
    IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void **)&swap);
    IDXGISwapChain1_Release(sc1);
    IDXGIFactory4_Release(fac);


    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, NBUF + 10};
    ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap);
    D3D12_DESCRIPTOR_HEAP_DESC sh = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 48, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    ID3D12Device_CreateDescriptorHeap(dev, &sh, &IID_ID3D12DescriptorHeap, (void **)&srv_heap);
    D3D12_DESCRIPTOR_HEAP_DESC dh = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2};
    ID3D12Device_CreateDescriptorHeap(dev, &dh, &IID_ID3D12DescriptorHeap, (void **)&dsv_heap);
    rtv_inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    srv_inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (create_backbuffers()) return -1;
    for (int i = 0; i < NBUF; i++)
        if (FAILED(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc[i])))
            return fail("CreateCommandAllocator", E_FAIL);
    if (FAILED(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0], NULL,
                                              &IID_ID3D12GraphicsCommandList, (void **)&cl)))
        return fail("CreateCommandList", E_FAIL);
    ID3D12GraphicsCommandList_Close(cl);
    ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (pipelines()) return -1;
    for (int i = 0; i < NBUF; i++) line_vb[i] = buffer(D3D12_HEAP_TYPE_UPLOAD, (UINT64)MAX_LINE_VERTS * sizeof(struct overlay_vertex), D3D12_RESOURCE_STATE_GENERIC_READ);
    rt_log("d3d12: presenter on %s (%dx%d, tearing %s)", adapter, out_w, out_h, tearing ? "yes" : "no");
    return 0;
}

const char *d3d_adapter_name(void) { return adapter; }

void d3d_output_size(int *ow, int *oh) {
    int w, h;
    client_size(&w, &h);
    if (swap && (w != out_w || h != out_h) && fg_state == 1 && dlss_fg_on()) {
        /* frame generation must be off before the swap chain is resized:
         * present once more at the old size with it off, then resize */
        fg_hold = 1;
    } else if (swap && (w != out_w || h != out_h)) {         /* window resized: resize the swap chain */
        wait_idle();
        for (int i = 0; i < NBUF; i++) { ID3D12Resource_Release(backbuf[i]); backbuf[i] = NULL; }
        IDXGISwapChain3_ResizeBuffers(swap, NBUF, (UINT)w, (UINT)h, DXGI_FORMAT_B8G8R8A8_UNORM,
                                      tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
        create_backbuffers();
        out_w = w; out_h = h;
    }
    *ow = out_w; *oh = out_h;
}

static void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b;
    memset(&b, 0, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from; b.Transition.StateAfter = to;
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
}

static void ensure_frame_tex(int w, int h) {
    if (frame_tex && tex_w == w && tex_h == h) return;
    wait_idle();
    if (frame_tex) ID3D12Resource_Release(frame_tex);
    for (int i = 0; i < NBUF; i++) if (upload[i]) { ID3D12Resource_Release(upload[i]); upload[i] = NULL; }
    frame_tex = texture(w, h, 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    make_srv(frame_tex, 0);
    upload_pitch = ((UINT)w * 4 + 255) & ~255u;
    for (int i = 0; i < NBUF; i++) upload[i] = buffer(D3D12_HEAP_TYPE_UPLOAD, (UINT64)upload_pitch * (UINT)h, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (mask_tex) ID3D12Resource_Release(mask_tex);
    for (int i = 0; i < NBUF; i++) if (mask_up[i]) { ID3D12Resource_Release(mask_up[i]); mask_up[i] = NULL; }
    mask_tex = texture_fmt(w, h, 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, DXGI_FORMAT_R8_UNORM);
    make_srv_fmt(mask_tex, 1, DXGI_FORMAT_R8_UNORM);
    make_srv_fmt(mask_tex, 3, DXGI_FORMAT_R8_UNORM);
    make_srv_fmt(mask_tex, 25, DXGI_FORMAT_R8_UNORM);     /* for the HUD overlay */
    mask_pitch = ((UINT)w + 255) & ~255u;
    for (int i = 0; i < NBUF; i++) mask_up[i] = buffer(D3D12_HEAP_TYPE_UPLOAD, (UINT64)mask_pitch * (UINT)h, D3D12_RESOURCE_STATE_GENERIC_READ);
    tex_w = w; tex_h = h;
}

/* ui_filter=mmpx: 2x and 4x targets (SRV 32 and 34, the mask beside each) */
static void ensure_mmpx(int w, int h) {
    if (mmpx_rt[0] && mmpx_w == w && mmpx_h == h) return;
    wait_idle();
    for (int k = 0; k < 2; k++) {
        if (mmpx_rt[k]) ID3D12Resource_Release(mmpx_rt[k]);
        mmpx_rt[k] = texture(w << (k + 1), h << (k + 1), 1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ID3D12Device_CreateRenderTargetView(dev, mmpx_rt[k], NULL, rtv(NBUF + 7 + k));
        make_srv(mmpx_rt[k], 32 + 2 * k);
        make_srv_fmt(mask_tex, 33 + 2 * k, DXGI_FORMAT_R8_UNORM);
    }
    mmpx_w = w; mmpx_h = h;
}

static void copy_to_texture(ID3D12Resource *dst_r, UINT sub, ID3D12Resource *src_r, UINT64 off, DXGI_FORMAT fmt, UINT w, UINT h, UINT pitch);
/* ui_filter=remacri: this frame's tags, and the atlas and table when the pack
 * (re)loads.  SRV 36 mask, 37 tags, 38 atlas, 39 table (bound from 34) */
static int art_ready;
static void upload_art(int w, int h, UINT bi) {
    art_ready = 0;
    if (settings.ui_filter != 3 || !prov_data || prov_dw != w || prov_dh != h || !art_active()) return;
    int aw, ah, ver, n;
    const uint8_t *ap = art_atlas(&aw, &ah, &ver);
    const float *tab = art_table(&n);
    if (!ap || n < 1) return;
    if (!prov_tex || prov_w != w || prov_h != h) {
        wait_idle();
        if (prov_tex) ID3D12Resource_Release(prov_tex);
        prov_tex = texture_fmt(w, h, 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, DXGI_FORMAT_R32_UINT);
        prov_pitch = ((UINT)w * 4 + 255) & ~255u;
        for (int i = 0; i < NBUF; i++) {
            if (prov_up[i]) ID3D12Resource_Release(prov_up[i]);
            prov_up[i] = buffer(D3D12_HEAP_TYPE_UPLOAD, (UINT64)prov_pitch * (UINT)h, D3D12_RESOURCE_STATE_GENERIC_READ);
        }
        make_srv_fmt(prov_tex, 37, DXGI_FORMAT_R32_UINT);
        make_srv_fmt(mask_tex, 36, DXGI_FORMAT_R8_UNORM);
        prov_w = w; prov_h = h;
    }
    if (ver != art_ver || !art_tex) {
        wait_idle();
        if (art_tex) ID3D12Resource_Release(art_tex);
        if (art_up) ID3D12Resource_Release(art_up);
        if (tab_tex) ID3D12Resource_Release(tab_tex);
        if (tab_up) ID3D12Resource_Release(tab_up);
        art_tex = texture_fmt(aw, ah, 0, D3D12_RESOURCE_STATE_COPY_DEST, DXGI_FORMAT_B8G8R8A8_UNORM);
        art_up = buffer(D3D12_HEAP_TYPE_UPLOAD, (UINT64)aw * 4 * (UINT)ah, D3D12_RESOURCE_STATE_GENERIC_READ);
        tab_tex = texture_fmt(2 * ART_MAX, 1, 0, D3D12_RESOURCE_STATE_COPY_DEST, DXGI_FORMAT_R32G32B32A32_FLOAT);
        tab_up = buffer(D3D12_HEAP_TYPE_UPLOAD, 2 * ART_MAX * 16, D3D12_RESOURCE_STATE_GENERIC_READ);
        void *p = NULL;
        D3D12_RANGE none = {0, 0};
        if (!art_tex || !art_up || !tab_tex || !tab_up) return;
        if (SUCCEEDED(ID3D12Resource_Map(art_up, 0, &none, &p))) { memcpy(p, ap, (size_t)aw * 4 * ah); ID3D12Resource_Unmap(art_up, 0, NULL); }
        if (SUCCEEDED(ID3D12Resource_Map(tab_up, 0, &none, &p))) {
            memset(p, 0, 2 * ART_MAX * 16); memcpy(p, tab, (size_t)n * 8 * sizeof(float)); ID3D12Resource_Unmap(tab_up, 0, NULL);
        }
        copy_to_texture(art_tex, 0, art_up, 0, DXGI_FORMAT_B8G8R8A8_UNORM, (UINT)aw, (UINT)ah, (UINT)aw * 4);
        copy_to_texture(tab_tex, 0, tab_up, 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 2 * ART_MAX, 1, 2 * ART_MAX * 16);
        barrier(art_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        barrier(tab_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        make_srv(art_tex, 38);
        make_srv_fmt(tab_tex, 39, DXGI_FORMAT_R32G32B32A32_FLOAT);
        art_ver = ver; art_h = ah;
    }
    void *p = NULL;
    D3D12_RANGE none = {0, 0};
    if (FAILED(ID3D12Resource_Map(prov_up[bi], 0, &none, &p))) return;
    for (int y = 0; y < h; y++) memcpy((uint8_t *)p + (size_t)y * prov_pitch, prov_data + (size_t)y * w, (size_t)w * 4);
    ID3D12Resource_Unmap(prov_up[bi], 0, NULL);
    barrier(prov_tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    copy_to_texture(prov_tex, 0, prov_up[bi], 0, DXGI_FORMAT_R32_UINT, (UINT)w, (UINT)h, prov_pitch);
    barrier(prov_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    art_ready = 1;
}

static void ensure_sharp(int w, int h) {
    if (sharp_rt && sharp_w == w && sharp_h == h) return;
    wait_idle();
    if (sharp_rt) ID3D12Resource_Release(sharp_rt);
    sharp_rt = texture(w, h, 1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ID3D12Device_CreateRenderTargetView(dev, sharp_rt, NULL, rtv(NBUF));
    make_srv(sharp_rt, 2);
    sharp_w = w; sharp_h = h;
}

static void draw_blit(int srv_slot, int linear, D3D12_CPU_DESCRIPTOR_HANDLE target, float x, float y, float w, float h) {
    ID3D12GraphicsCommandList_SetPipelineState(cl, pso_blit);
    float c[4] = {linear ? 1.0f : 0.0f, 0, 0, 0};
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cl, 0, 4, c, 0);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 1, srv_gpu(srv_slot));
    D3D12_VIEWPORT vp = {x, y, w, h, 0, 1};
    D3D12_RECT sc = {(LONG)x, (LONG)y, (LONG)(x + w), (LONG)(y + h)};
    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
    ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &target, FALSE, NULL);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
}

static void ensure_depth(int w, int h) {
    if (depth && depth_w == w && depth_h == h) return;
    wait_idle();
    if (depth) ID3D12Resource_Release(depth);
    depth = texture_fmt(w, h, 2, D3D12_RESOURCE_STATE_DEPTH_WRITE, DXGI_FORMAT_D32_FLOAT);
    ID3D12Device_CreateDepthStencilView(dev, depth, NULL, ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsv_heap));
    if (motion) ID3D12Resource_Release(motion);
    motion = texture_fmt(w, h, 1, D3D12_RESOURCE_STATE_RENDER_TARGET, DXGI_FORMAT_R16G16_FLOAT);
    ID3D12Device_CreateRenderTargetView(dev, motion, NULL, rtv(NBUF + 1));
    depth_w = w; depth_h = h;
}

/* upscaler targets.  SRV slots: 8/9/10 = colour, motion, history 0 and
 * 11/12/13 = colour, motion, history 1 (the taa pass reads the previous
 * history), 14/15 = history 0/1 for the composite.  RTVs NBUF+2.. */
static D3D12_CPU_DESCRIPTOR_HANDLE dsv(int i) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsv_heap);
    h.ptr += (SIZE_T)i * ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    return h;
}
static void ensure_up(int rw, int rh, int ow, int oh) {
    if (up_color && up_rw == rw && up_rh == rh && hist_w == ow && hist_h == oh) return;
    wait_idle();
    if (up_color) { ID3D12Resource_Release(up_color); ID3D12Resource_Release(up_depth); ID3D12Resource_Release(up_motion); }
    for (int i = 0; i < 2; i++) if (hist[i]) { ID3D12Resource_Release(hist[i]); hist[i] = NULL; }
    up_color = texture_fmt(rw, rh, 1, D3D12_RESOURCE_STATE_RENDER_TARGET, DXGI_FORMAT_B8G8R8A8_UNORM);
    up_motion = texture_fmt(rw, rh, 1, D3D12_RESOURCE_STATE_RENDER_TARGET, DXGI_FORMAT_R16G16_FLOAT);
    {   /* R32_TYPELESS so FSR can read it as R32_FLOAT; D32_FLOAT depth view */
        D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = (UINT64)rw; rd.Height = (UINT)rh;
        rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = DXGI_FORMAT_R32_TYPELESS; rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE cv = {DXGI_FORMAT_D32_FLOAT};
        cv.DepthStencil.Depth = 1.0f;
        up_depth = NULL;
        ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                             &IID_ID3D12Resource, (void **)&up_depth);
    }
    ID3D12Device_CreateRenderTargetView(dev, up_color, NULL, rtv(NBUF + 2));
    ID3D12Device_CreateRenderTargetView(dev, up_motion, NULL, rtv(NBUF + 3));
    {
        D3D12_DEPTH_STENCIL_VIEW_DESC dd = {DXGI_FORMAT_D32_FLOAT, D3D12_DSV_DIMENSION_TEXTURE2D};
        ID3D12Device_CreateDepthStencilView(dev, up_depth, &dd, dsv(1));
    }
    /* FSR output (UAV), SRV slots 16 and 18 for the composite */
    if (fsr_out) { ID3D12Resource_Release(fsr_out); fsr_out = NULL; }
    {
        D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = (UINT64)ow; rd.Height = (UINT)oh;
        rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL,
                                             &IID_ID3D12Resource, (void **)&fsr_out);
    }
    for (int i = 0; i < 2; i++)      /* both first: each one's descriptors name the other */
        hist[i] = texture_fmt(ow, oh, 1, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, DXGI_FORMAT_R16G16B16A16_FLOAT);
    for (int i = 0; i < 2; i++) {
        ID3D12Device_CreateRenderTargetView(dev, hist[i], NULL, rtv(NBUF + 4 + i));
        make_srv(up_color, 8 + 3 * i);
        make_srv_fmt(up_motion, 9 + 3 * i, DXGI_FORMAT_R16G16_FLOAT);
        make_srv_fmt(hist[i ^ 1], 10 + 3 * i, DXGI_FORMAT_R16G16B16A16_FLOAT);   /* the previous history */
        make_srv_fmt(hist[i], 14 + i, DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (fsr_out) make_srv_fmt(fsr_out, 16 + 2 * i, DXGI_FORMAT_R16G16B16A16_FLOAT);
        make_srv_fmt(hist[i], 17 + 2 * i, DXGI_FORMAT_R16G16B16A16_FLOAT);
    }
    up_rw = rw; up_rh = rh; hist_w = ow; hist_h = oh;
    hist_valid = 0;
}
static float halton(unsigned i, unsigned b) {
    float f = 1, r = 0;
    for (; i; i /= b) { f /= (float)b; r += f * (float)(i % b); }
    return r;
}

/* texture windows (R8_UINT array), remap tables (R8_UINT, a row each), palette */
static int ensure_scene_res(void) {
    if (scene_tex) return 0;
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = 256; rd.Height = 256;
    rd.DepthOrArraySize = NATIVE_MAX_TEX; rd.MipLevels = 1; rd.Format = DXGI_FORMAT_R8_UINT; rd.SampleDesc.Count = 1;
    if (FAILED(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                    NULL, &IID_ID3D12Resource, (void **)&scene_tex))) return fail("scene textures", E_FAIL);
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {0};
    sd.Format = DXGI_FORMAT_R8_UINT; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2DArray.MipLevels = 1; sd.Texture2DArray.ArraySize = NATIVE_MAX_TEX;
    ID3D12Device_CreateShaderResourceView(dev, scene_tex, &sd, srv_cpu(5));
    scene_tab = texture_fmt(256, NATIVE_MAX_TABLES, 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, DXGI_FORMAT_R8_UINT);
    scene_pal = texture_fmt(256, 1, 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, DXGI_FORMAT_B8G8R8A8_UNORM);
    if (!scene_tab || !scene_pal) return fail("scene tables", E_FAIL);
    make_srv_fmt(scene_tab, 6, DXGI_FORMAT_R8_UINT);
    make_srv(scene_pal, 7);
    for (int i = 0; i < NBUF; i++)
        if (!(res_up[i] = buffer(D3D12_HEAP_TYPE_UPLOAD, RES_SIZE, D3D12_RESOURCE_STATE_GENERIC_READ))) return fail("scene upload", E_FAIL);
    return 0;
}

static void copy_to_texture(ID3D12Resource *dst_r, UINT sub, ID3D12Resource *src_r, UINT64 off, DXGI_FORMAT fmt, UINT w, UINT h, UINT pitch) {
    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    dst.pResource = dst_r; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = sub;
    src.pResource = src_r; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = off;
    src.PlacedFootprint.Footprint.Format = fmt;
    src.PlacedFootprint.Footprint.Width = w; src.PlacedFootprint.Footprint.Height = h;
    src.PlacedFootprint.Footprint.Depth = 1; src.PlacedFootprint.Footprint.RowPitch = pitch;
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
}

static void upload_buffer(ID3D12Resource **b, UINT64 *cap, const void *data, UINT64 size) {
    if (!*b || *cap < size) {
        if (*b) ID3D12Resource_Release(*b);
        *cap = size * 2;
        *b = buffer(D3D12_HEAP_TYPE_UPLOAD, *cap, D3D12_RESOURCE_STATE_GENERIC_READ);
    }
    void *p = NULL;
    D3D12_RANGE none = {0, 0};
    if (*b && SUCCEEDED(ID3D12Resource_Map(*b, 0, &none, &p))) { memcpy(p, data, (size_t)size); ID3D12Resource_Unmap(*b, 0, NULL); }
}

static void save_bmp(const char *path, int w, int h, UINT pitch) {
    void *p = NULL;
    D3D12_RANGE r = {0, (SIZE_T)readback_size};
    if (FAILED(ID3D12Resource_Map(readback, 0, &r, &p))) return;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
    if (s) {
        for (int y = 0; y < h; y++) memcpy((uint8_t *)s->pixels + y * s->pitch, (uint8_t *)p + (size_t)y * pitch, (size_t)w * 4);
        SDL_SaveBMP(s, path);
        SDL_FreeSurface(s);
    }
    D3D12_RANGE none = {0, 0};
    ID3D12Resource_Unmap(readback, 0, &none);
}

void d3d_present(const uint32_t *pixels, int w, int h, int scaling, int dx, int dy, int dw, int dh,
                 const struct overlay_vertex *lines, int nline_verts, const char *shot_bmp,
                 const struct native_vertex *tris, int ntri_verts, const uint8_t *mask,
                 const struct native_res *res, int wx, int ww, const struct upscale_params *up) {
    if (!swap) return;
    int ow, oh;
    d3d_output_size(&ow, &oh);
    /* DLSS Frame Generation in flight: present only the game's own frames
     * (DLSS generates the ones in between); a re-presented scene frame is
     * skipped, except after 100 ms without a new one (a paused game) or
     * when a window capture is pending (the caller drops the request after
     * this call, so a skipped present lost it: DEBUG_LOG 104) */
    int fg = fg_state == 1 && settings.frame_gen > 0 && !fg_hold && tris && ntri_verts >= 3 && mask && res &&
             up && up->enabled;
    fg_hold = 0;
    {
        static ULONGLONG last_fg;
        ULONGLONG now = GetTickCount64();
        if (fg && !up->new_scene && now - last_fg < 100 && !(shot_bmp && shot_bmp[0])) return;
        if (fg) last_fg = now;
    }
    if (sl_active) dlss_frame_begin();      /* Streamline frame: Reflex sleep, PCL markers */
    UINT bi = IDXGISwapChain3_GetCurrentBackBufferIndex(swap);
    wait_frame(bi);
    ensure_frame_tex(w, h);
    /* upload the frame */
    {
        void *p = NULL;
        D3D12_RANGE none = {0, 0};
        if (SUCCEEDED(ID3D12Resource_Map(upload[bi], 0, &none, &p))) {
            for (int y = 0; y < h; y++) memcpy((uint8_t *)p + (size_t)y * upload_pitch, pixels + (size_t)y * w, (size_t)w * 4);
            ID3D12Resource_Unmap(upload[bi], 0, NULL);
        }
        if (mask && SUCCEEDED(ID3D12Resource_Map(mask_up[bi], 0, &none, &p))) {
            for (int y = 0; y < h; y++) memcpy((uint8_t *)p + (size_t)y * mask_pitch, mask + (size_t)y * w, (size_t)w);
            ID3D12Resource_Unmap(mask_up[bi], 0, NULL);
        }
    }
    int native = tris && ntri_verts >= 3 && mask && res && !ensure_scene_res();
    int upscale = native && up && up->enabled;
    int rw = ow, rh = oh;
    if (upscale) {
        float sc = up->scale < 0.5f ? 0.5f : up->scale > 1.0f ? 1.0f : up->scale;
        rw = (int)((float)ow * sc + 0.5f); rh = (int)((float)oh * sc + 0.5f);
        /* DLSS: its mode for render_scale, at its optimal render size */
        if (up->dlss && dlss_state == 1 && dlss_render_size(settings.render_scale, ow, oh, &rw, &rh)) {
            dlss_state = -1; rt_log("dlss: no render size, using taa");
        }
        if (rw < 16) rw = 16;
        if (rh < 16) rh = 16;
        ensure_up(rw, rh, ow, oh);
        /* upscaler=fsr: AMD's DLL, once; its context follows the output size */
        if (up->fsr && fsr_state >= 0) {
            if (fsr_init(dev)) { fsr_state = -1; rt_log("fsr: unavailable, using taa"); }
            else if (fsr_state == 0 || fsr_w != ow || fsr_h != oh) {
                wait_idle();
                if (fsr_context(ow, oh)) { fsr_state = -1; rt_log("fsr: no context, using taa"); }
                else { fsr_state = 1; fsr_w = ow; fsr_h = oh; hist_valid = 0; }
            }
        }
    } else hist_valid = 0;
    int use_fsr = upscale && up->fsr && fsr_state == 1 && fsr_out;
    int use_dlss = upscale && up->dlss && dlss_state == 1 && fsr_out;
    fg = fg && upscale;
    if (fg) ensure_fg(ow, oh);
    if (native) {
        ensure_depth(ow, oh);
        upload_buffer(&tri_vb[bi], &tri_cap[bi], tris, (UINT64)ntri_verts * sizeof(struct native_vertex));
        void *p = NULL;
        D3D12_RANGE none = {0, 0};
        if (SUCCEEDED(ID3D12Resource_Map(res_up[bi], 0, &none, &p))) {
            uint8_t *u = p;
            if (res->ntex) memcpy(u, res->tex, (size_t)res->ntex * 65536);
            if (res->ntables) memcpy(u + RES_TAB_OFF, res->tables, (size_t)res->ntables * 256);
            memcpy(u + RES_PAL_OFF, res->palette, 1024);
            ID3D12Resource_Unmap(res_up[bi], 0, NULL);
        }
    }
    int k = 1;
    if (scaling == SCALE_SHARP) {
        k = (dw + w - 1) / w;
        int k2 = (dh + h - 1) / h;
        if (k2 > k) k = k2;
        if (k > 8) k = 8;
        if (k < 1) k = 1;
        ensure_sharp(w * k, h * k);
    }
    dlss_marker(DLSS_MARK_SUBMIT_START);
    ID3D12CommandAllocator_Reset(alloc[bi]);
    ID3D12GraphicsCommandList_Reset(cl, alloc[bi], NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, rootsig);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 1, &srv_heap);
    barrier(frame_tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    {
        D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
        dst.pResource = frame_tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = upload[bi]; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        src.PlacedFootprint.Footprint.Width = (UINT)w; src.PlacedFootprint.Footprint.Height = (UINT)h;
        src.PlacedFootprint.Footprint.Depth = 1; src.PlacedFootprint.Footprint.RowPitch = upload_pitch;
        ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
    }
    barrier(frame_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (native) {
        barrier(mask_tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
        dst.pResource = mask_tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = mask_up[bi]; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UNORM;
        src.PlacedFootprint.Footprint.Width = (UINT)w; src.PlacedFootprint.Footprint.Height = (UINT)h;
        src.PlacedFootprint.Footprint.Depth = 1; src.PlacedFootprint.Footprint.RowPitch = mask_pitch;
        ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
        barrier(mask_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        upload_art(w, h, bi);
        /* this frame's texture windows, tables and palette */
        barrier(scene_tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        barrier(scene_tab, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        barrier(scene_pal, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        for (int i = 0; i < res->ntex; i++)
            copy_to_texture(scene_tex, (UINT)i, res_up[bi], (UINT64)i * 65536, DXGI_FORMAT_R8_UINT, 256, 256, 256);
        if (res->ntables)
            copy_to_texture(scene_tab, 0, res_up[bi], RES_TAB_OFF, DXGI_FORMAT_R8_UINT, 256, (UINT)res->ntables, 256);
        copy_to_texture(scene_pal, 0, res_up[bi], RES_PAL_OFF, DXGI_FORMAT_B8G8R8A8_UNORM, 256, 1, 1024);
        barrier(scene_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        barrier(scene_tab, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        barrier(scene_pal, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    int src_slot = 0, linear = scaling == SCALE_SMOOTH;
    if (scaling == SCALE_SHARP) {                       /* integer nearest upscale first */
        barrier(sharp_rt, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        draw_blit(0, 0, rtv(NBUF), 0, 0, (float)sharp_w, (float)sharp_h);
        barrier(sharp_rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        src_slot = 2; linear = 1;
    }
    barrier(backbuf[bi], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    {
        D3D12_CPU_DESCRIPTOR_HANDLE t = rtv((int)bi);
        float black[4] = {0, 0, 0, 1};
        ID3D12GraphicsCommandList_ClearRenderTargetView(cl, t, black, 0, NULL);
        draw_blit(src_slot, linear, t, (float)dx, (float)dy, (float)dw, (float)dh);
        if (native) {
            /* 1. the world, natively: straight over the game's picture (which
             * stays as the fallback), or with an upscaler into its own HUD-less
             * targets at render resolution, jittered */
            extern int native_debug;
            float zero[4] = {0, 0, 0, 0};
            float jx = 0, jy = 0;
            D3D12_CPU_DESCRIPTOR_HANDLE ds, mrt[2];
            if (upscale) {
                if (use_fsr) fsr_jitter(up_frame++, rw, ow, &jx, &jy);      /* FSR's own sequence */
                else if (use_dlss) {
                    /* DLSS: Halton (2,3) with 8 x (output/render)^2 phases (NVIDIA's guidance) */
                    float rr = (float)ow / (float)rw;
                    unsigned phases = (unsigned)(8.0f * rr * rr + 0.5f);
                    if (phases < 8) phases = 8;
                    unsigned ph = (up_frame++ % phases) + 1;
                    jx = halton(ph, 2) - 0.5f; jy = halton(ph, 3) - 0.5f;
                }
                else {
                    unsigned ph = (up_frame++ % 8) + 1;  /* Halton (2,3), 8 phases, render pixels */
                    jx = halton(ph, 2) - 0.5f; jy = halton(ph, 3) - 0.5f;
                }
                /* Streamline's common constants, once per frame (DLSS and frame generation) */
                if (sl_active) dlss_constants(rw, rh, ow, oh, jx, jy, up->new_scene, !hist_valid || up->reset);
                ds = dsv(1); mrt[0] = rtv(NBUF + 2); mrt[1] = rtv(NBUF + 3);
                ID3D12GraphicsCommandList_ClearRenderTargetView(cl, mrt[0], zero, 0, NULL);
                /* backdrop: the game's picture under the scene, so the
                 * upscaler gets an opaque image and model edges are
                 * anti-aliased against the real background (FSR has no
                 * alpha); the cockpit/HUD pass puts the crisp UI back on top */
                {
                    float ksb = (float)rw / (float)ow;
                    ID3D12GraphicsCommandList_SetPipelineState(cl, pso_blit);
                    float cb[4] = {linear ? 1.0f : 0.0f, 0, 0, 0};
                    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cl, 0, 4, cb, 0);
                    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 1, srv_gpu(src_slot));
                    D3D12_VIEWPORT bv = {(float)dx * ksb, (float)dy * ksb, (float)dw * ksb, (float)dh * ksb, 0, 1};
                    D3D12_RECT bs = {(LONG)((float)dx * ksb), (LONG)((float)dy * ksb), (LONG)((float)(dx + dw) * ksb + 0.999f),
                                     (LONG)((float)(dy + dh) * ksb + 0.999f)};
                    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &bv);
                    ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &bs);
                    ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &mrt[0], FALSE, NULL);
                    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
                }
            } else {
                ds = dsv(0); mrt[0] = t; mrt[1] = rtv(NBUF + 1);
            }
            ID3D12GraphicsCommandList_ClearDepthStencilView(cl, ds, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, NULL);
            ID3D12GraphicsCommandList_ClearRenderTargetView(cl, mrt[1], zero, 0, NULL);
            ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 2, mrt, FALSE, &ds);
            ID3D12GraphicsCommandList_SetPipelineState(cl, pso_scene);
            {
                float c[8] = {(float)rw, (float)rh, (native_debug & 32) ? 1.0f : 0.0f, 0,
                              2.0f * jx / (float)rw, -2.0f * jy / (float)rh, jx, jy};
                ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cl, 0, 8, c, 0);
            }
            ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 1, srv_gpu(5));
            float ks = (float)rw / (float)ow;
            D3D12_VIEWPORT vp = {0, 0, (float)rw, (float)rh, 0, 1};
            D3D12_RECT sc = {(LONG)((float)wx * ks), (LONG)((float)dy * ks), (LONG)((float)(wx + ww) * ks + 0.999f),
                             (LONG)((float)(dy + dh) * ks + 0.999f)};
            ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);
            ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
            ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            D3D12_VERTEX_BUFFER_VIEW vb = {ID3D12Resource_GetGPUVirtualAddress(tri_vb[bi]),
                                           (UINT)((UINT64)ntri_verts * sizeof(struct native_vertex)), sizeof(struct native_vertex)};
            ID3D12GraphicsCommandList_IASetVertexBuffers(cl, 0, 1, &vb);
            ID3D12GraphicsCommandList_DrawInstanced(cl, (UINT)ntri_verts, 1, 0, 0);
            if (upscale) {
                /* 1b. the upscaler: current samples + motion + previous history
                 * -> this frame's history (output resolution) */
                int prev = hist_cur, curh = hist_cur ^ 1;
                const D3D12_RESOURCE_STATES rd_st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                barrier(up_color, D3D12_RESOURCE_STATE_RENDER_TARGET, rd_st);
                barrier(up_motion, D3D12_RESOURCE_STATE_RENDER_TARGET, rd_st);
                barrier(hist[curh], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                D3D12_CPU_DESCRIPTOR_HANDLE ht = rtv(NBUF + 4 + curh);
                ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &ht, FALSE, NULL);
                ID3D12GraphicsCommandList_SetPipelineState(cl, pso_taa);
                int reset = !hist_valid || up->reset;
                float c[8] = {(float)ow, (float)oh, (float)rw, (float)rh, jx, jy, up->new_scene ? 1.0f : 0.0f, reset ? 1.0f : 0.0f};
                ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cl, 0, 8, c, 0);
                ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 1, srv_gpu(8 + 3 * curh));   /* reads hist[prev] */
                D3D12_VIEWPORT hv = {0, 0, (float)ow, (float)oh, 0, 1};
                D3D12_RECT hs = {0, 0, ow, oh};
                ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &hv);
                ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &hs);
                ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
                barrier(hist[curh], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                int fsr_done = 0;
                if (use_fsr) {
                    /* 1b'. AMD FSR into fsr_out (compute, recorded into this list) */
                    barrier(up_depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, rd_st);
                    fsr_done = !fsr_dispatch(cl, up_color, up_depth, up_motion, fsr_out, rw, rh, ow, oh, jx, jy,
                                             up->new_scene, reset);
                    barrier(up_depth, rd_st, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                    /* FSR binds its own root signature and heaps */
                    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, rootsig);
                    ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 1, &srv_heap);
                    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &hv);
                    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                }
                if (use_dlss) {
                    /* 1b''. NVIDIA DLSS (Streamline) into fsr_out */
                    barrier(up_depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, rd_st);
                    fsr_done = !dlss_evaluate(cl, up_color, (uint32_t)rd_st, up_depth, (uint32_t)rd_st, up_motion, (uint32_t)rd_st,
                                              fsr_out, (uint32_t)D3D12_RESOURCE_STATE_UNORDERED_ACCESS, rw, rh, ow, oh);
                    barrier(up_depth, rd_st, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                    /* the host restores command-list state after slEvaluateFeature */
                    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, rootsig);
                    ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 1, &srv_heap);
                    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &hv);
                    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                }
                barrier(up_color, rd_st, D3D12_RESOURCE_STATE_RENDER_TARGET);
                barrier(up_motion, rd_st, D3D12_RESOURCE_STATE_RENDER_TARGET);
                (void)prev;
                hist_cur = curh; hist_valid = 1;
                /* 1c. composite the upscaled scene over the game's picture */
                ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &t, FALSE, NULL);
                D3D12_RECT cs = {wx, dy, wx + ww, dy + dh};
                ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &cs);
                if (fsr_done && !(native_debug & 128)) {   /* 128: FSR runs, taa is shown (debug) */
                    barrier(fsr_out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    /* the backdrop made FSR's input opaque: copy its output as it is */
                    ID3D12GraphicsCommandList_SetPipelineState(cl, pso_blit);
                    float cf[4] = {0, 0, 0, 0};
                    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cl, 0, 4, cf, 0);
                    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 1, srv_gpu(16 + 2 * curh));
                    ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
                    barrier(fsr_out, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                } else {
                    ID3D12GraphicsCommandList_SetPipelineState(cl, pso_comp);
                    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 1, srv_gpu(14 + curh));
                    ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
                }
                if (fg) {
                    /* 1d. frame generation's HUD-less colour: the picture so far */
                    barrier(backbuf[bi], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    barrier(hudless, RD_ST, D3D12_RESOURCE_STATE_COPY_DEST);
                    ID3D12GraphicsCommandList_CopyResource(cl, hudless, backbuf[bi]);
                    barrier(hudless, D3D12_RESOURCE_STATE_COPY_DEST, RD_ST);
                    barrier(backbuf[bi], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    barrier(ui_alpha, RD_ST, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    ID3D12GraphicsCommandList_ClearRenderTargetView(cl, rtv(NBUF + 6), zero, 0, NULL);
                }
            }
            /* 2. the game's own cockpit/HUD pixels on top (its 3D pixels discarded) */
            ID3D12GraphicsCommandList_SetPipelineState(cl, pso_masked);
            /* over [wx, wx+ww]: uv.x = (x - dx) / dw, edge columns repeated outside */
            /* ui_filter=xbr: the game's own 320x200 frame (slot 0, its mask in 1),
             * upscaled in the shader; mmpx: two 2x MMPX passes into targets
             * first (the first with the 3D pixels transparent), then sampled */
            int xbr = settings.ui_filter == 1, art = settings.ui_filter == 3 && art_ready, mmpx = settings.ui_filter == 2 || settings.ui_filter == 3;
            if (mmpx) {
                ensure_mmpx(w, h);
                for (int k = 0; k < 2; k++) {
                    int ow2 = w << (k + 1), oh2 = h << (k + 1);
                    barrier(mmpx_rt[k], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    D3D12_CPU_DESCRIPTOR_HANDLE mt = rtv(NBUF + 7 + k);
                    ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &mt, FALSE, NULL);
                    D3D12_RECT msc = {0, 0, ow2, oh2};
                    D3D12_VIEWPORT mvp = {0, 0, (float)ow2, (float)oh2, 0, 1};
                    ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &msc);
                    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &mvp);
                    ID3D12GraphicsCommandList_SetPipelineState(cl, pso_mmpx);
                    float mc[8] = {0, 0, 0, 0, 0, k == 0 ? 1.0f : 0.0f, 0, 0};
                    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cl, 0, 8, mc, 0);
                    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 1, srv_gpu(k == 0 ? 0 : 32));
                    ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
                    barrier(mmpx_rt[k], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                }
                ID3D12GraphicsCommandList_SetPipelineState(cl, pso_masked);
            }
            float c[8] = {(linear && !xbr && !mmpx) ? 1.0f : 0.0f, (float)ww / (float)dw, (float)(wx - dx) / (float)dw,
                          settings.widescreen_cockpit ? 1.0f / (float)w : 0.0f, xbr ? 1.0f : art ? 3.0f : mmpx ? 2.0f : 0.0f, 0, 0, 0};
            ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cl, 0, 8, c, 0);
            ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 1, srv_gpu(mmpx ? 34 : xbr ? 0 : src_slot));
            D3D12_RECT sc2 = {wx, dy, wx + ww, dy + dh};
            ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc2);
            D3D12_VIEWPORT vp2 = {(float)wx, (float)dy, (float)ww, (float)dh, 0, 1};
            ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp2);
            if (fg) {       /* the UI pixels also into the UI alpha (1 where the game's UI is drawn) */
                D3D12_CPU_DESCRIPTOR_HANDLE ut[2] = {t, rtv(NBUF + 6)};
                ID3D12GraphicsCommandList_SetPipelineState(cl, pso_masked_ui);
                ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 2, ut, FALSE, NULL);
            } else
                ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &t, FALSE, NULL);
            ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
            /* 3. the game's HUD lines and bitmaps, sharp (hud.c) */
            if (hud_n > 0 && hud_v && hud_at) {
                if (!hud_atlas) {
                    hud_atlas = texture_fmt(HUD_ATLAS, HUD_ATLAS, 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, DXGI_FORMAT_R8_UNORM);
                    make_srv_fmt(hud_atlas, 24, DXGI_FORMAT_R8_UNORM);
                    make_srv_fmt(hud_atlas, 26, DXGI_FORMAT_R8_UNORM);
                }
                if (!hud_at_up[bi]) hud_at_up[bi] = buffer(D3D12_HEAP_TYPE_UPLOAD, (UINT64)HUD_ATLAS * HUD_ATLAS, D3D12_RESOURCE_STATE_GENERIC_READ);
                void *hp = NULL;
                D3D12_RANGE hnone = {0, 0};
                if (hud_atlas && hud_at_up[bi] && SUCCEEDED(ID3D12Resource_Map(hud_at_up[bi], 0, &hnone, &hp))) {
                    memcpy(hp, hud_at, (size_t)HUD_ATLAS * HUD_ATLAS);
                    ID3D12Resource_Unmap(hud_at_up[bi], 0, NULL);
                    barrier(hud_atlas, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                    copy_to_texture(hud_atlas, 0, hud_at_up[bi], 0, DXGI_FORMAT_R8_UNORM, HUD_ATLAS, HUD_ATLAS, HUD_ATLAS);
                    barrier(hud_atlas, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    upload_buffer(&hud_vb[bi], &hud_vb_cap[bi], hud_v, (UINT64)hud_n * sizeof(struct hud_vertex));
                    ID3D12GraphicsCommandList_SetPipelineState(cl, fg ? pso_hud_ui : pso_hud);
                    float hc[8] = {(float)dx, (float)dy, (float)dw / (float)w, (float)dh / (float)h,
                                   (float)w, (float)h, (float)HUD_ATLAS, (float)HUD_ATLAS};
                    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cl, 0, 8, hc, 0);
                    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 1, srv_gpu(24));
                    D3D12_VIEWPORT hvp = {0, 0, (float)ow, (float)oh, 0, 1};
                    D3D12_RECT hsc = {dx, dy, dx + dw, dy + dh};
                    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &hvp);
                    ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &hsc);
                    if (fg) {
                        D3D12_CPU_DESCRIPTOR_HANDLE ut2[2] = {t, rtv(NBUF + 6)};
                        ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 2, ut2, FALSE, NULL);
                    } else ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &t, FALSE, NULL);
                    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    D3D12_VERTEX_BUFFER_VIEW hvb = {ID3D12Resource_GetGPUVirtualAddress(hud_vb[bi]),
                                                    (UINT)((UINT64)hud_n * sizeof(struct hud_vertex)), sizeof(struct hud_vertex)};
                    ID3D12GraphicsCommandList_IASetVertexBuffers(cl, 0, 1, &hvb);
                    ID3D12GraphicsCommandList_DrawInstanced(cl, (UINT)hud_n, 1, 0, 0);
                }
            }
            if (fg) barrier(ui_alpha, D3D12_RESOURCE_STATE_RENDER_TARGET, RD_ST);
        }
        if (lines && nline_verts >= 2) {
            int n = nline_verts > MAX_LINE_VERTS ? MAX_LINE_VERTS : nline_verts & ~1;
            void *p = NULL;
            D3D12_RANGE none = {0, 0};
            if (SUCCEEDED(ID3D12Resource_Map(line_vb[bi], 0, &none, &p))) {
                struct overlay_vertex *v = p;
                for (int i = 0; i < n; i++) {      /* pixels -> clip space */
                    v[i].x = lines[i].x * 2.0f / (float)ow - 1.0f;
                    v[i].y = 1.0f - lines[i].y * 2.0f / (float)oh;
                    v[i].argb = lines[i].argb;
                }
                ID3D12Resource_Unmap(line_vb[bi], 0, NULL);
                D3D12_VERTEX_BUFFER_VIEW vb = {ID3D12Resource_GetGPUVirtualAddress(line_vb[bi]),
                                               (UINT)(n * sizeof(struct overlay_vertex)), sizeof(struct overlay_vertex)};
                ID3D12GraphicsCommandList_SetPipelineState(cl, pso_lines);
                D3D12_VIEWPORT vp = {0, 0, (float)ow, (float)oh, 0, 1};
                D3D12_RECT sc = {0, 0, ow, oh};
                ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);
                ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
                ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_LINELIST);
                ID3D12GraphicsCommandList_IASetVertexBuffers(cl, 0, 1, &vb);
                ID3D12GraphicsCommandList_DrawInstanced(cl, (UINT)n, 1, 0, 0);
            }
        }
    }
    UINT shot_pitch = ((UINT)ow * 4 + 255) & ~255u;
    if (shot_bmp && shot_bmp[0]) {                      /* copy the back buffer for a window capture */
        UINT64 need = (UINT64)shot_pitch * (UINT)oh;
        if (!readback || readback_size < need) {
            if (readback) ID3D12Resource_Release(readback);
            readback = buffer(D3D12_HEAP_TYPE_READBACK, need, D3D12_RESOURCE_STATE_COPY_DEST);
            readback_size = need;
        }
        barrier(backbuf[bi], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
        src.pResource = backbuf[bi]; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = readback; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width = (UINT)ow; dst.PlacedFootprint.Footprint.Height = (UINT)oh;
        dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = shot_pitch;
        ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
        barrier(backbuf[bi], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    } else {
        barrier(backbuf[bi], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    }
    if (sl_active) {
        /* constants every frame (menus too); frame generation's inputs, valid
         * until Present, in the states they have then; on or off for this present */
        dlss_constants(ow, oh, ow, oh, 0, 0, 0, 1);
        if (fg) dlss_fg_tag(cl, up_depth, (uint32_t)D3D12_RESOURCE_STATE_DEPTH_WRITE, up_motion,
                            (uint32_t)D3D12_RESOURCE_STATE_RENDER_TARGET, hudless, (uint32_t)RD_ST, ui_alpha, (uint32_t)RD_ST,
                            rw, rh, ow, oh);
        else dlss_fg_untag(cl);
        if (fg_state == 1) dlss_fg_set(fg ? settings.frame_gen - 1 : 0);
    }
    ID3D12GraphicsCommandList_Close(cl);
    ID3D12CommandList *lists[1] = {(ID3D12CommandList *)cl};
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    dlss_marker(DLSS_MARK_SUBMIT_END);
    if (shot_bmp && shot_bmp[0]) { wait_idle(); save_bmp(shot_bmp, ow, oh, shot_pitch); }
    dlss_marker(DLSS_MARK_PRESENT_START);
    IDXGISwapChain3_Present(swap, 0, tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    dlss_marker(DLSS_MARK_PRESENT_END);
    dlss_fg_poll();
    if (fg_state == 1) {    /* every 10 s: our presents vs the swap chain's (with generated frames) */
        static ULONGLONG t0; static UINT pc0; static unsigned np;
        ULONGLONG now = GetTickCount64();
        np++;
        if (now - t0 >= 10000) {
            DXGI_FRAME_STATISTICS fs;
            HRESULT fh = IDXGISwapChain3_GetFrameStatistics(swap, &fs);
            if (SUCCEEDED(fh) && t0)
                rt_log("present: %u frames from the port, %u presented by the swap chain in %llu ms", np,
                       fs.PresentCount - pc0, (unsigned long long)(now - t0));
            else if (FAILED(fh) && t0) rt_log("present: %u frames in %llu ms (no frame statistics: 0x%08lx)", np,
                                              (unsigned long long)(now - t0), (unsigned long)fh);
            if (SUCCEEDED(fh)) pc0 = fs.PresentCount;
            t0 = now; np = 0;
        }
    }
    fence_val[bi] = fence_next++;
    ID3D12CommandQueue_Signal(queue, fence, fence_val[bi]);
}

/* the sharp HUD for the next present (hud.c); n = 0: none */
void d3d_set_hud(const struct hud_vertex *v, int n, const uint8_t *atlas) { hud_v = v; hud_n = n; hud_at = atlas; }
void d3d_set_prov(const uint32_t *p, int w, int h) { prov_data = p; prov_dw = w; prov_dh = h; }

void d3d_shutdown(void) {
    if (!dev) return;
    wait_idle();
    if (sl_active) dlss_shutdown();         /* before the device goes away */
}

/* frame generation is generating frames (hw.c then presents on each page flip) */
int d3d_fg_active(void) { return fg_state == 1 && dlss_fg_on(); }

/* frame generation's HUD-less colour and UI alpha, backbuffer size */
static void ensure_fg(int w, int h) {
    if (hudless && fg_w == w && fg_h == h) return;
    wait_idle();
    if (hudless) { ID3D12Resource_Release(hudless); ID3D12Resource_Release(ui_alpha); }
    hudless = texture_fmt(w, h, 0, RD_ST, DXGI_FORMAT_B8G8R8A8_UNORM);
    ui_alpha = texture_fmt(w, h, 1, RD_ST, DXGI_FORMAT_R8_UNORM);
    ID3D12Device_CreateRenderTargetView(dev, ui_alpha, NULL, rtv(NBUF + 6));
    fg_w = w; fg_h = h;
}
