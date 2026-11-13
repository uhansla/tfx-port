/* AMD FSR upscaler through the FidelityFX API (RENDERER.md, stage 7).
 *
 * The FidelityFX SDK v2.4.0 ships AMD-signed DLLs with a C API:
 * amd_fidelityfx_loader_dx12.dll exports ffxCreateContext / ffxQuery /
 * ffxDispatch / ffxDestroyContext and picks the upscaler provider from
 * amd_fidelityfx_upscaler_dx12.dll (FSR 4 on Radeon RX 9000, FSR 3.1
 * otherwise).  Both are x64 only; tools/build.py copies them next to tfx.exe and
 * this file loads them at run time, so nothing links against AMD code.
 *
 * Inputs come from the native renderer's upscaler slot: colour at render
 * resolution (gamma-encoded B8G8R8A8: FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE),
 * standard depth (0 near .. 1 far, finite far plane), motion vectors in render
 * pixels from the current pixel to its previous position (FSR's convention,
 * motionVectorScale 1), and the jitter applied as (2 jx / W, -2 jy / H) in
 * clip space, which is what FSR's documentation prescribes. */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#ifdef NO_FSR
/* Built without the FidelityFX SDK's headers (tools/build.py found no
 * toolchain/fidelityfx-*): upscaler=fsr falls back to taa. */
#include "rt.h"
#include "fsr.h"
int fsr_init(struct ID3D12Device *dev) { (void)dev; rt_log("fsr: not built in (no FidelityFX SDK at build time)"); return -1; }
int fsr_context(int ow, int oh) { (void)ow; (void)oh; return -1; }
void fsr_jitter(unsigned index, int rw, int ow, float *jx, float *jy) { (void)index; (void)rw; (void)ow; *jx = *jy = 0; }
int fsr_dispatch(void *cl, struct ID3D12Resource *color, struct ID3D12Resource *depth, struct ID3D12Resource *motion,
                 struct ID3D12Resource *out, int rw, int rh, int ow, int oh, float jx, float jy, int moving, int reset) {
    (void)cl; (void)color; (void)depth; (void)motion; (void)out; (void)rw; (void)rh; (void)ow; (void)oh;
    (void)jx; (void)jy; (void)moving; (void)reset;
    return -1;
}
const char *fsr_version(void) { return "not built in"; }
#else
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>
#include <d3d12.h>
#include "ffx_api.h"
#include "ffx_upscale.h"
#include "dx12/ffx_api_dx12.h"
#include "rt.h"
#include "fsr.h"

static HMODULE dll;
static PfnFfxCreateContext pCreate;
static PfnFfxDestroyContext pDestroy;
static PfnFfxQuery pQuery;
static PfnFfxDispatch pDispatch;
static ID3D12Device *device;
static ffxContext ctx;
static int ctx_w, ctx_h, failed;
static char version[96] = "none";
static LARGE_INTEGER last_t;

static void msg(uint32_t type, const wchar_t *m) {
    char s[512];
    WideCharToMultiByte(CP_UTF8, 0, m, -1, s, sizeof s, NULL, NULL);
    rt_log("fsr: %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", s);
}

int fsr_init(ID3D12Device *dev) {
    if (dll) return 0;
    if (failed) return -1;
    device = dev;
    /* the DLLs next to the exe only (not the DLL search path) */
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    while (n && path[n - 1] != L'\\') n--;
    if (n + 40 >= MAX_PATH) { failed = 1; return -1; }
    wcscpy(path + n, L"amd_fidelityfx_loader_dx12.dll");
    dll = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dll) {
        rt_log("fsr: amd_fidelityfx_loader_dx12.dll not found next to the exe (error %lu)", GetLastError());
        failed = 1;
        return -1;
    }
    pCreate = (PfnFfxCreateContext)(void *)GetProcAddress(dll, "ffxCreateContext");
    pDestroy = (PfnFfxDestroyContext)(void *)GetProcAddress(dll, "ffxDestroyContext");
    pQuery = (PfnFfxQuery)(void *)GetProcAddress(dll, "ffxQuery");
    pDispatch = (PfnFfxDispatch)(void *)GetProcAddress(dll, "ffxDispatch");
    if (!pCreate || !pDestroy || !pQuery || !pDispatch) {
        rt_log("fsr: the loader DLL lacks the FidelityFX API entry points");
        FreeLibrary(dll); dll = NULL; failed = 1;
        return -1;
    }
    rt_log("fsr: FidelityFX loader loaded");
    return 0;
}

int fsr_context(int ow, int oh) {
    if (!dll) return -1;
    if (ctx && ctx_w == ow && ctx_h == oh) return 0;
    if (ctx) { pDestroy(&ctx, NULL); ctx = NULL; }
    struct ffxCreateBackendDX12Desc be;
    memset(&be, 0, sizeof be);
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    be.device = device;
    struct ffxCreateContextDescUpscale cd;
    memset(&cd, 0, sizeof cd);
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    cd.header.pNext = &be.header;
    cd.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE | FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;
    if (getenv("TFX_FSR_DEBUG")) cd.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;   /* API checks, logged via msg() */
    cd.maxRenderSize.width = (uint32_t)ow; cd.maxRenderSize.height = (uint32_t)oh;
    cd.maxUpscaleSize.width = (uint32_t)ow; cd.maxUpscaleSize.height = (uint32_t)oh;
    cd.fpMessage = msg;
    ffxReturnCode_t r = pCreate(&ctx, &cd.header, NULL);
    if (r != FFX_API_RETURN_OK) {
        rt_log("fsr: ffxCreateContext failed (%u)", r);
        ctx = NULL;
        return -1;
    }
    ctx_w = ow; ctx_h = oh;
    struct ffxQueryGetProviderVersion pv;
    memset(&pv, 0, sizeof pv);
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (pQuery(&ctx, &pv.header) == FFX_API_RETURN_OK && pv.versionName)
        snprintf(version, sizeof version, "%s", pv.versionName);
    rt_log("fsr: upscaler context %dx%d, provider %s", ow, oh, version);
    QueryPerformanceCounter(&last_t);
    return 0;
}

const char *fsr_version(void) { return version; }

void fsr_jitter(unsigned index, int rw, int ow, float *jx, float *jy) {
    *jx = *jy = 0;
    if (!ctx) return;
    int32_t phases = 8;
    struct ffxQueryDescUpscaleGetJitterPhaseCount pc;
    memset(&pc, 0, sizeof pc);
    pc.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
    pc.renderWidth = (uint32_t)rw; pc.displayWidth = (uint32_t)ow; pc.pOutPhaseCount = &phases;
    pQuery(&ctx, &pc.header);
    if (phases < 1) phases = 8;
    struct ffxQueryDescUpscaleGetJitterOffset jo;
    memset(&jo, 0, sizeof jo);
    jo.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
    jo.index = (int32_t)(index % (unsigned)phases); jo.phaseCount = phases; jo.pOutX = jx; jo.pOutY = jy;
    pQuery(&ctx, &jo.header);
}

static struct FfxApiResource res(ID3D12Resource *r, uint32_t fmt, int w, int h, uint32_t state, uint32_t usage) {
    struct FfxApiResource x;
    memset(&x, 0, sizeof x);
    x.resource = r;
    x.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    x.description.format = fmt;
    x.description.width = (uint32_t)w; x.description.height = (uint32_t)h; x.description.depth = 1;
    x.description.mipCount = 1;
    x.description.usage = usage;
    x.state = state;
    return x;
}

int fsr_dispatch(void *cl, ID3D12Resource *color, ID3D12Resource *depth, ID3D12Resource *motion,
                 ID3D12Resource *out, int rw, int rh, int ow, int oh, float jx, float jy, int moving, int reset) {
    if (!ctx) return -1;
    LARGE_INTEGER now, fq;
    QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
    float dt = (float)((double)(now.QuadPart - last_t.QuadPart) * 1000.0 / (double)fq.QuadPart);
    last_t = now;
    if (dt <= 0 || dt > 200) dt = 16.7f;
    struct ffxDispatchDescUpscale d;
    memset(&d, 0, sizeof d);
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cl;
    d.color = res(color, FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM, rw, rh, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ, FFX_API_RESOURCE_USAGE_READ_ONLY);
    d.depth = res(depth, FFX_API_SURFACE_FORMAT_R32_FLOAT, rw, rh, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ, FFX_API_RESOURCE_USAGE_DEPTHTARGET);
    d.motionVectors = res(motion, FFX_API_SURFACE_FORMAT_R16G16_FLOAT, rw, rh, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ, FFX_API_RESOURCE_USAGE_READ_ONLY);
    d.output = res(out, FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, ow, oh, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);
    d.jitterOffset.x = jx; d.jitterOffset.y = jy;
    /* a re-presented scene frame hasn't moved: no motion */
    d.motionVectorScale.x = moving ? 1.0f : 0.0f; d.motionVectorScale.y = moving ? 1.0f : 0.0f;
    d.renderSize.width = (uint32_t)rw; d.renderSize.height = (uint32_t)rh;
    d.upscaleSize.width = (uint32_t)ow; d.upscaleSize.height = (uint32_t)oh;
    d.enableSharpening = false;
    d.sharpness = 0;
    d.frameTimeDelta = dt;
    d.preExposure = 1.0f;
    d.reset = reset != 0;
    /* the game's camera: near plane Z 16, far 32768 (the native depth
     * mapping), vertical half-angle atan(100 / 256) */
    d.cameraNear = 16.0f;
    d.cameraFar = 32768.0f;
    d.cameraFovAngleVertical = 2.0f * atanf(100.0f / 256.0f);
    d.viewSpaceToMetersFactor = 6.0f;           /* ~6 m per game unit (TFX_CODE.md 9.13) */
    d.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    ffxReturnCode_t r = pDispatch(&ctx, &d.header);
    if (r != FFX_API_RETURN_OK) {
        static int logged;
        if (!logged++) rt_log("fsr: ffxDispatch failed (%u)", r);
        return -1;
    }
    return 0;
}
#endif  /* NO_FSR */
