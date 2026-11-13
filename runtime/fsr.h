/* AMD FSR through the FidelityFX API (signed DLLs from the FidelityFX SDK,
 * loaded at run time; x64 only).  See fsr.c and RENDERER.md, stage 7. */
#ifndef FSR_H
#define FSR_H
#include <stdint.h>

struct ID3D12Device;
struct ID3D12Resource;

int fsr_init(struct ID3D12Device *dev);          /* load the DLL once: 0 = available */
int fsr_context(int ow, int oh);                 /* (re)create for this output size (GPU idle!): 0 = ready */
void fsr_jitter(unsigned index, int rw, int ow, float *jx, float *jy);   /* FSR's sequence, render pixels */
/* record the upscale into cl: colour (B8G8R8A8, gamma), depth (R32), motion
 * (R16G16_FLOAT, render pixels, previous - current), output (R16G16B16A16
 * UAV) at the output size.  Inputs in PIXEL|NON_PIXEL_SHADER_RESOURCE,
 * output in UNORDERED_ACCESS.  moving = 0 for a re-presented scene frame. */
int fsr_dispatch(void *cl, struct ID3D12Resource *color, struct ID3D12Resource *depth, struct ID3D12Resource *motion,
                 struct ID3D12Resource *out, int rw, int rh, int ow, int oh, float jx, float jy, int moving, int reset);
const char *fsr_version(void);                   /* provider in use (after fsr_context) */
#endif
