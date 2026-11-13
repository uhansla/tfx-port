/* NVIDIA DLSS Super Resolution and Frame Generation through Streamline
 * 2.14.16 (RENDERER.md, stage 7).
 *
 * Streamline is used in its "manual hooking" mode: the port keeps its own
 * native D3D12 device; only the DXGI factory (for the swap chain) and the
 * device used to create the command queue are upgraded to Streamline proxies,
 * so Streamline sees Present and the queue as its guide requires.  The
 * interposer DLL is loaded at run time from the exe's folder after checking
 * NVIDIA's signature (sl::security::verifyEmbeddedSignature); its C entry
 * points are fetched with GetProcAddress.  slGetFeatureFunction is defined
 * here as a forwarder so the inline helpers of sl_dlss.h work.
 *
 * Inputs come from the native renderer's upscaler slot (d3d12.c): colour at
 * render resolution (gamma-encoded LDR), depth (0 near .. 1 far), motion in
 * render pixels from the current pixel to its previous position (mvecScale =
 * 1 / render size), sub-pixel jitter in pixels (applied in clip space as
 * (2 jx / W, -2 jy / H)), reset on camera cuts.
 *
 * Frame Generation (DLSS-G, frame_generation=on) also loads Reflex (which
 * DLSS-G requires) and uses PCL.  Each present is one Streamline frame: one
 * token for the Reflex sleep, the PCL markers, the common constants (set
 * once) and the tags.  DLSS-G gets depth and motion (render resolution) and
 * the HUD-less colour and UI alpha (backbuffer size), all valid until
 * Present; it adds generated frames in Streamline's swap chain. */
#ifdef NO_DLSS
/* Built without the Streamline SDK's headers (tools/build.py found no
 * toolchain/streamline-*): upscaler=dlss falls back to taa and frame
 * generation is off. */
#include <cstdint>
#include "dlss.h"
extern "C" {
void rt_log(const char *fmt, ...);
int dlss_preinit(int, int) { rt_log("dlss: not built in (no Streamline SDK at build time)"); return -1; }
void *dlss_upgrade(void *iface) { return iface; }
int dlss_set_device(void *, const void *, uint32_t) { return -1; }
int dlss_sr_ok(void) { return 0; }
int dlss_fg_ok(void) { return 0; }
int dlss_render_size(int, int ow, int oh, int *rw, int *rh) { *rw = ow; *rh = oh; return -1; }
void dlss_frame_begin(void) {}
void dlss_marker(int) {}
void dlss_constants(int, int, int, int, float, float, int, int) {}
int dlss_evaluate(void *, void *, uint32_t, void *, uint32_t, void *, uint32_t, void *, uint32_t, int, int, int, int) { return -1; }
int dlss_fg_set(int) { return 0; }
int dlss_fg_on(void) { return 0; }
int dlss_fg_tag(void *, void *, uint32_t, void *, uint32_t, void *, uint32_t, void *, uint32_t, int, int, int, int) { return -1; }
void dlss_fg_untag(void *) {}
void dlss_fg_poll(void) {}
void dlss_shutdown(void) {}
const char *dlss_mode_name(void) { return "not built in"; }
}
#else
#include <windows.h>
#include <d3d12.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "sl.h"
#include "sl_consts.h"
#include "sl_dlss.h"
#include "sl_dlss_g.h"
#include "sl_reflex.h"
#include "sl_pcl.h"
#include "sl_security.h"
#include "sl_helpers.h"
#include "dlss.h"

extern "C" void rt_log(const char *fmt, ...);

static HMODULE mod;
static PFun_slInit *p_slInit;
static PFun_slShutdown *p_slShutdown;
static PFun_slSetD3DDevice *p_slSetD3DDevice;
static PFun_slIsFeatureSupported *p_slIsFeatureSupported;
static PFun_slUpgradeInterface *p_slUpgradeInterface;
static PFun_slGetFeatureFunction *p_slGetFeatureFunction;
static PFun_slGetNewFrameToken *p_slGetNewFrameToken;
static PFun_slSetTagForFrame *p_slSetTagForFrame;
static PFun_slSetConstants *p_slSetConstants;
static PFun_slEvaluateFeature *p_slEvaluateFeature;
static bool inited, device_ok;
static bool want_sr, want_fg, fg_ok;   /* asked for at slInit; DLSS-G + Reflex supported */
static sl::FrameToken *tok;            /* this present's frame */
static bool consts_set;
static int fg_cur = -1, fg_max = 1;    /* generated frames now set (0 = off); the GPU's maximum */
static bool fg_tagged, fg_queried;
static int info_more;                  /* let this many more info messages through */
static sl::DLSSOptions options;
static int cur_pct = -1, cur_ow, cur_oh, cur_rw, cur_rh;
static const char *mode_name = "off";
static uint32_t frame_index;

/* the sl_dlss.h helpers call this (normally provided by sl.interposer.lib) */
extern "C" sl::Result slGetFeatureFunction(sl::Feature feature, const char *functionName, void *&function) {
    if (!p_slGetFeatureFunction) return sl::Result::eErrorNotInitialized;
    return p_slGetFeatureFunction(feature, functionName, function);
}

static void sl_log(sl::LogType type, const char *msg) {
    /* warnings and errors only, at most 20 of each kind of message (Streamline
     * can repeat one every frame); info only before the first frame */
    static int infos, warns;
    if (info_more) { infos = 60 - info_more; info_more = 0; }     /* more after frame generation starts */
    if (type == sl::LogType::eInfo && ++infos > 60) return;
    if (type != sl::LogType::eInfo && ++warns > 40) return;
    char s[512];
    snprintf(s, sizeof s, "%s", msg ? msg : "");
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
    rt_log("dlss: %s%s", type == sl::LogType::eError ? "error: " : type == sl::LogType::eWarn ? "warning: " : "", s);
}

template <typename T> static bool load(T *&p, const char *name) {
    p = reinterpret_cast<T *>(reinterpret_cast<void *>(GetProcAddress(mod, name)));
    return p != nullptr;
}

static const char *res_str(sl::Result r) { return sl::getResultAsStr(r); }

extern "C" int dlss_preinit(int sr, int fg) {
    if (inited) return 0;
    want_sr = sr != 0; want_fg = fg != 0;
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    while (n && path[n - 1] != L'\\') n--;
    if (n + 32 >= MAX_PATH) return -1;
    wcscpy(path + n, L"sl.interposer.dll");
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        rt_log("dlss: sl.interposer.dll not found next to the exe");
        return -1;
    }
    if (!sl::security::verifyEmbeddedSignature(path)) {
        rt_log("dlss: sl.interposer.dll is not signed by NVIDIA; not loading it");
        return -1;
    }
    mod = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!mod) { rt_log("dlss: cannot load sl.interposer.dll (error %lu)", GetLastError()); return -1; }
    if (!load(p_slInit, "slInit") || !load(p_slShutdown, "slShutdown") || !load(p_slSetD3DDevice, "slSetD3DDevice") ||
        !load(p_slIsFeatureSupported, "slIsFeatureSupported") || !load(p_slUpgradeInterface, "slUpgradeInterface") ||
        !load(p_slGetFeatureFunction, "slGetFeatureFunction") || !load(p_slGetNewFrameToken, "slGetNewFrameToken") ||
        !load(p_slSetTagForFrame, "slSetTagForFrame") || !load(p_slSetConstants, "slSetConstants") ||
        !load(p_slEvaluateFeature, "slEvaluateFeature")) {
        rt_log("dlss: sl.interposer.dll lacks expected entry points");
        FreeLibrary(mod); mod = nullptr;
        return -1;
    }
    /* PCL is always loaded; DLSS-G needs Reflex */
    static sl::Feature features[3];
    uint32_t nf = 0;
    if (want_sr) features[nf++] = sl::kFeatureDLSS;
    if (want_fg) { features[nf++] = sl::kFeatureDLSS_G; features[nf++] = sl::kFeatureReflex; }
    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = sl::LogLevel::eDefault;
    pref.logMessageCallback = sl_log;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = nf;
    /* no NVIDIA application id: custom engine + project GUID (sl.h allows this) */
    pref.applicationId = 0;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "tfx-port";
    pref.projectId = "5f0d6c1e-2b7a-4e39-9c84-1a3f6d2b8e71";
    pref.renderAPI = sl::RenderAPI::eD3D12;
    /* manual hooking; the host restores command-list state; no over-the-air downloads */
    pref.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eDisableCLStateTracking |
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging;      /* slSetTagForFrame */
    sl::Result r = p_slInit(pref, sl::kSDKVersion);
    if (r != sl::Result::eOk) {
        rt_log("dlss: slInit failed (%d)", (int)r);
        FreeLibrary(mod); mod = nullptr;
        return -1;
    }
    inited = true;
    rt_log("dlss: Streamline initialised (manual hooking;%s%s)", want_sr ? " DLSS" : "", want_fg ? " DLSS-G Reflex" : "");
    return 0;
}

extern "C" void *dlss_upgrade(void *iface) {
    if (!inited || !iface) return iface;
    void *p = iface;
    if (p_slUpgradeInterface(&p) != sl::Result::eOk || !p) return iface;
    return p;
}

extern "C" int dlss_set_device(void *native_device, const void *luid, uint32_t luid_size) {
    if (!inited) return -1;
    sl::Result r = p_slSetD3DDevice(native_device);
    if (r != sl::Result::eOk) { rt_log("dlss: slSetD3DDevice failed (%d)", (int)r); return -1; }
    sl::AdapterInfo ai{};
    ai.deviceLUID = (uint8_t *)luid;
    ai.deviceLUIDSizeInBytes = luid_size;
    if (want_sr) {
        r = p_slIsFeatureSupported(sl::kFeatureDLSS, ai);
        if (r != sl::Result::eOk) rt_log("dlss: DLSS is not supported on this GPU/driver (%s)", res_str(r));
        else { device_ok = true; rt_log("dlss: DLSS Super Resolution available"); }
    }
    if (want_fg) {
        r = p_slIsFeatureSupported(sl::kFeatureDLSS_G, ai);
        sl::Result r2 = r == sl::Result::eOk ? p_slIsFeatureSupported(sl::kFeatureReflex, ai) : r;
        if (r != sl::Result::eOk)
            rt_log("dlss-g: Frame Generation is not supported here (%s)%s", res_str(r),
                   r == sl::Result::eErrorOSDisabledHWS ? ": Windows' hardware-accelerated GPU scheduling is off "
                                                          "(Settings > System > Display > Graphics)" : "");
        else if (r2 != sl::Result::eOk) rt_log("dlss-g: Reflex is not supported here (%s)", res_str(r2));
        else {
            sl::ReflexOptions ro{};
            ro.mode = sl::ReflexMode::eLowLatency;       /* DLSS-G requires Reflex */
            r = slReflexSetOptions(ro);
            if (r != sl::Result::eOk) rt_log("dlss-g: slReflexSetOptions failed (%s)", res_str(r));
            else { fg_ok = true; rt_log("dlss-g: Frame Generation available (Reflex low latency on)"); }
        }
    }
    return 0;
}

extern "C" int dlss_sr_ok(void) { return device_ok; }
extern "C" int dlss_fg_ok(void) { return fg_ok; }

/* one Streamline frame per present: token, Reflex sleep, simulation markers */
extern "C" void dlss_frame_begin(void) {
    if (!inited) return;
    uint32_t idx = frame_index++;
    tok = nullptr;
    if (p_slGetNewFrameToken(tok, &idx) != sl::Result::eOk) tok = nullptr;
    consts_set = false;
    if (fg_ok && tok) {
        slReflexSleep(*tok);
        slPCLSetMarker(sl::PCLMarker::eSimulationStart, *tok);
        slPCLSetMarker(sl::PCLMarker::eSimulationEnd, *tok);
    }
}

extern "C" void dlss_marker(int m) {
    if (fg_ok && tok) slPCLSetMarker((sl::PCLMarker)m, *tok);
}

extern "C" int dlss_render_size(int pct, int ow, int oh, int *rw, int *rh) {
    if (!device_ok) return -1;
    if (pct == cur_pct && ow == cur_ow && oh == cur_oh) { *rw = cur_rw; *rh = cur_rh; return 0; }
    sl::DLSSOptions o{};
    if (pct >= 100) { o.mode = sl::DLSSMode::eDLAA; mode_name = "DLAA"; }
    else if (pct >= 67) { o.mode = sl::DLSSMode::eMaxQuality; mode_name = "Quality"; }
    else if (pct >= 58) { o.mode = sl::DLSSMode::eBalanced; mode_name = "Balanced"; }
    else { o.mode = sl::DLSSMode::eMaxPerformance; mode_name = "Performance"; }
    o.outputWidth = (uint32_t)ow; o.outputHeight = (uint32_t)oh;
    o.colorBuffersHDR = sl::Boolean::eFalse;            /* LDR, gamma-encoded */
    o.useAutoExposure = sl::Boolean::eTrue;
    sl::DLSSOptimalSettings s{};
    sl::Result r = slDLSSGetOptimalSettings(o, s);
    if (r != sl::Result::eOk || !s.optimalRenderWidth || !s.optimalRenderHeight) {
        rt_log("dlss: slDLSSGetOptimalSettings failed (%d)", (int)r);
        return -1;
    }
    options = o;
    cur_pct = pct; cur_ow = ow; cur_oh = oh;
    cur_rw = (int)s.optimalRenderWidth; cur_rh = (int)s.optimalRenderHeight;
    rt_log("dlss: mode %s, %dx%d -> %dx%d", mode_name, cur_rw, cur_rh, ow, oh);
    *rw = cur_rw; *rh = cur_rh;
    return 0;
}

extern "C" const char *dlss_mode_name(void) { return mode_name; }

/* row-major view -> clip for the game's camera (vertical half-angle
 * atan(100/256), near 16, far 32768, D3D depth 0..1); row-vector convention */
static void projection(float aspect, sl::float4x4 &m, sl::float4x4 &inv) {
    const float n = 16.0f, f = 32768.0f;
    const float ty = 100.0f / 256.0f, tx = ty * aspect;
    memset(&m, 0, sizeof m); memset(&inv, 0, sizeof inv);
    m[0].x = 1.0f / tx;
    m[1].y = 1.0f / ty;
    m[2].z = f / (f - n); m[2].w = 1.0f;
    m[3].z = -n * f / (f - n);
    inv[0].x = tx;
    inv[1].y = ty;
    inv[2].w = (f - n) / (-n * f);
    inv[3].z = 1.0f; inv[3].w = 1.0f / n;
}

/* the common constants, once per frame (DLSS-G: never twice in a frame) */
extern "C" void dlss_constants(int rw, int rh, int ow, int oh, float jx, float jy, int moving, int reset) {
    if (!inited || !tok || consts_set) return;
    consts_set = true;
    sl::ViewportHandle vp(0u);
    sl::Constants c{};
    projection((float)ow / (float)oh, c.cameraViewToClip, c.clipToCameraView);
    sl::float4x4 id{};
    memset(&id, 0, sizeof id);
    id[0].x = id[1].y = id[2].z = id[3].w = 1.0f;
    c.clipToLensClip = id;
    c.clipToPrevClip = id;          /* motion is fully in the motion-vector buffer */
    c.prevClipToClip = id;
    c.jitterOffset = sl::float2(jx, jy);
    c.mvecScale = moving ? sl::float2(1.0f / (float)rw, 1.0f / (float)rh) : sl::float2(0.0f, 0.0f);
    c.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    c.cameraPos = sl::float3(0.0f, 0.0f, 0.0f);
    c.cameraUp = sl::float3(0.0f, 1.0f, 0.0f);
    c.cameraRight = sl::float3(1.0f, 0.0f, 0.0f);
    c.cameraFwd = sl::float3(0.0f, 0.0f, 1.0f);
    c.cameraNear = 16.0f;
    c.cameraFar = 32768.0f;
    c.cameraFOV = 2.0f * atanf(100.0f / 256.0f);
    c.cameraAspectRatio = (float)ow / (float)oh;
    c.motionVectorsInvalidValue = 0.0f;
    c.depthInverted = sl::Boolean::eFalse;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    c.motionVectorsJittered = sl::Boolean::eFalse;
    sl::Result r = p_slSetConstants(c, *tok, vp);
    if (r != sl::Result::eOk) { static int l; if (!l++) rt_log("dlss: slSetConstants failed (%s)", res_str(r)); }
}

extern "C" int dlss_evaluate(void *cl, void *color, uint32_t color_state, void *depth, uint32_t depth_state,
                             void *motion, uint32_t motion_state, void *out, uint32_t out_state,
                             int rw, int rh, int ow, int oh) {
    if (!device_ok || !tok || !consts_set) return -1;
    sl::FrameToken *token = tok;
    sl::ViewportHandle vp(0u);
    sl::Result r = slDLSSSetOptions(vp, options);
    if (r != sl::Result::eOk) { static int l; if (!l++) rt_log("dlss: slDLSSSetOptions failed (%d)", (int)r); return -1; }

    sl::Resource rc(sl::ResourceType::eTex2d, color, color_state);
    sl::Resource rd(sl::ResourceType::eTex2d, depth, depth_state);
    sl::Resource rm(sl::ResourceType::eTex2d, motion, motion_state);
    sl::Resource ro(sl::ResourceType::eTex2d, out, out_state);
    sl::Extent ein{0, 0, (uint32_t)rw, (uint32_t)rh}, eout{0, 0, (uint32_t)ow, (uint32_t)oh};
    sl::ResourceTag tags[4] = {
        sl::ResourceTag(&rc, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &ein),
        sl::ResourceTag(&rd, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &ein),
        sl::ResourceTag(&rm, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &ein),
        sl::ResourceTag(&ro, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &eout)};
    r = p_slSetTagForFrame(*token, vp, tags, 4, reinterpret_cast<sl::CommandBuffer *>(cl));
    if (r != sl::Result::eOk) { static int l; if (!l++) rt_log("dlss: slSetTagForFrame failed (%d)", (int)r); return -1; }

    const sl::BaseStructure *inputs[] = {&vp};
    r = p_slEvaluateFeature(sl::kFeatureDLSS, *token, inputs, 1, reinterpret_cast<sl::CommandBuffer *>(cl));
    if (r != sl::Result::eOk) { static int l; if (!l++) rt_log("dlss: slEvaluateFeature failed (%d)", (int)r); return -1; }
    return 0;
}

/* DLSS-G: gen = generated frames per presented frame (0 = off); applies
 * at the next Present */
extern "C" int dlss_fg_set(int gen) {
    if (!fg_ok) return -1;
    sl::ViewportHandle vp(0u);
    if (gen && !fg_queried) {              /* first use: this GPU's maximum */
        fg_queried = true;
        info_more = 80;
        sl::DLSSGState st{};
        if (slDLSSGGetState(vp, st, nullptr) == sl::Result::eOk && st.numFramesToGenerateMax)
            fg_max = (int)st.numFramesToGenerateMax;
        rt_log("dlss-g: up to %d generated frame(s) per game frame on this GPU; vsync support %s", fg_max,
               st.bIsVsyncSupportAvailable == sl::Boolean::eTrue ? "yes" : "no");
    }
    if (gen > fg_max) gen = fg_max;
    if (gen < 0) gen = 0;
    if (gen == fg_cur) return 0;
    sl::DLSSGOptions o{};
    o.mode = gen ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    o.numFramesToGenerate = gen ? (uint32_t)gen : 1u;
    o.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;      /* menus: no stutter when it comes back */
    o.enableUserInterfaceRecomposition = sl::Boolean::eTrue; /* HUD-less + UI alpha are tagged */
    sl::Result r = slDLSSGSetOptions(vp, o);
    if (r != sl::Result::eOk) { rt_log("dlss-g: slDLSSGSetOptions failed (%s)", res_str(r)); return -1; }
    if (fg_cur >= 0 || gen) rt_log("dlss-g: %s (%dx)", gen ? "on" : "off", gen + 1);
    fg_cur = gen;
    return 0;
}

extern "C" int dlss_fg_on(void) { return fg_cur > 0; }

/* DLSS-G inputs for this frame, in the states they will have at Present */
extern "C" int dlss_fg_tag(void *cl, void *depth, uint32_t depth_state, void *motion, uint32_t motion_state,
                           void *hudless, uint32_t hudless_state, void *ui, uint32_t ui_state,
                           int rw, int rh, int ow, int oh) {
    if (!fg_ok || !tok) return -1;
    sl::ViewportHandle vp(0u);
    sl::Resource rd(sl::ResourceType::eTex2d, depth, depth_state);
    sl::Resource rm(sl::ResourceType::eTex2d, motion, motion_state);
    sl::Resource rhl(sl::ResourceType::eTex2d, hudless, hudless_state);
    sl::Resource ru(sl::ResourceType::eTex2d, ui, ui_state);
    sl::Extent ein{0, 0, (uint32_t)rw, (uint32_t)rh}, eout{0, 0, (uint32_t)ow, (uint32_t)oh};
    sl::ResourceTag tags[4] = {
        sl::ResourceTag(&rd, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &ein),
        sl::ResourceTag(&rm, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &ein),
        sl::ResourceTag(&rhl, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &eout),
        sl::ResourceTag(&ru, sl::kBufferTypeUIAlpha, sl::ResourceLifecycle::eValidUntilPresent, &eout)};
    sl::Result r = p_slSetTagForFrame(*tok, vp, tags, 4, reinterpret_cast<sl::CommandBuffer *>(cl));
    if (r != sl::Result::eOk) { static int l; if (!l++) rt_log("dlss-g: slSetTagForFrame failed (%s)", res_str(r)); return -1; }
    fg_tagged = true;
    return 0;
}

/* inputs not valid (menus, 2D screens): null tags */
extern "C" void dlss_fg_untag(void *cl) {
    if (!fg_ok || !tok || !fg_tagged) return;
    sl::ViewportHandle vp(0u);
    sl::ResourceTag tags[4] = {
        sl::ResourceTag(nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(nullptr, sl::kBufferTypeUIAlpha, sl::ResourceLifecycle::eValidUntilPresent)};
    p_slSetTagForFrame(*tok, vp, tags, 4, reinterpret_cast<sl::CommandBuffer *>(cl));
    fg_tagged = false;
}

/* now and then: DLSS-G's status and the frames shown per presented frame */
extern "C" void dlss_fg_poll(void) {
    if (!fg_ok || fg_cur <= 0) return;
    static uint32_t n, last_status = 0xffffffffu;
    sl::DLSSGState st{};
    if (slDLSSGGetState(sl::ViewportHandle(0u), st, nullptr) != sl::Result::eOk) return;
    if ((uint32_t)st.status != last_status || ++n % 150 == 0) {
        last_status = (uint32_t)st.status;
        rt_log("dlss-g: status %s0x%x, frames shown per game frame %u", st.status == sl::DLSSGStatus::eOk ? "ok " : "",
               (unsigned)st.status, st.numFramesActuallyPresented);
    }
}

extern "C" void dlss_shutdown(void) {
    if (!inited) return;
    p_slShutdown();
    inited = device_ok = false;
}
#endif  /* NO_DLSS */
