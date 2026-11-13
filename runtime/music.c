/* Music: run START.EXE's original resident music code.
 *
 * START.EXE (Borland C++, real mode) normally installs OSL's "LMEDIT Track
 * Driver" and one device driver (AdLib for MUSIC=1 "SB/CD", LAPC-1/MT-32 for
 * MUSIC=2, Roland SCC-1 for MUSIC=4) and then spawns TFX.DAT.  TFX.DAT finds
 * the track driver through the real-mode vector IRQBASE (TFX.CFG, default 135
 * = 87h) whose handler is preceded by "DID!", and calls it with DPMI 0300h.
 *
 * Here START.EXE is loaded from the game folder into DOS memory (with its MZ
 * relocations) and its own install routine 0549h:0041h (relative to the
 * image) is run on the real-mode interpreter (rm86.c), exactly as START's
 * main() does, followed by its "start" call (AH=17h).  From then on:
 *   - TFX.DAT's DPMI 0300h calls to IRQBASE run the real handler (dpmi.c);
 *   - the track driver's timer handler (it hooks int 8 and speeds up the PIT)
 *     runs on IRQ0 and chains to the BIOS tick (dpmi.c, rm86.c);
 *   - the device drivers drive an OPL2 at 388h (opl.c) or an MPU-401 at 330h
 *     (mpu.c), as on the original hardware.
 * See TFX_CODE.md 9.10. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

int dos_alloc(uint16_t paras, uint16_t *seg, uint16_t *maxavail);
void rm86_init(void);
void rm86_far_call(uint16_t cs_, uint16_t ip_, uint16_t *regs8, uint16_t *segs4, const uint16_t *args, int nargs);

static int mus_vec = 0x87, cfg_music, cfg_soundfx;
static int installed;

static void cfg_read(void) {
    extern char game_dir[512];
    char path[600], line[256];
    snprintf(path, sizeof path, "%s\\TFX.CFG", game_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        if (!_strnicmp(line, "IRQBASE=", 8) && atoi(line + 8) > 0) mus_vec = atoi(line + 8) & 0xff;
        if (!_strnicmp(line, "MUSIC=", 6)) cfg_music = atoi(line + 6);
        if (!_strnicmp(line, "SOUNDFX=", 8)) cfg_soundfx = atoi(line + 8);
    }
    fclose(f);
}

/* load an MZ executable image (without its PSP) at a fresh DOS block */
static int load_mz(const char *path, uint16_t *load_seg) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return 0; }
    fclose(f);
    if (n < 0x40 || d[0] != 'M' || d[1] != 'Z') { free(d); return 0; }
    uint16_t last = d[2] | d[3] << 8, pages = d[4] | d[5] << 8, nrel = d[6] | d[7] << 8, hdr = d[8] | d[9] << 8;
    uint16_t minalloc = d[10] | d[11] << 8, relo = d[0x18] | d[0x19] << 8;
    long size = (long)(pages - (last ? 1 : 0)) * 512 + last - hdr * 16;
    if (size <= 0 || hdr * 16 + size > n) { free(d); return 0; }
    uint16_t paras = (uint16_t)((size + 15) / 16 + minalloc + 1), seg, maxav;
    if (!dos_alloc(paras, &seg, &maxav)) { free(d); return 0; }
    uint32_t base = (uint32_t)seg << 4;
    memset(MEM + base, 0, (size_t)paras * 16);
    memcpy(MEM + base, d + hdr * 16, (size_t)size);
    for (int i = 0; i < nrel; i++) {
        const uint8_t *r = d + relo + 4 * i;
        uint32_t at = base + (uint32_t)((r[2] | r[3] << 8) << 4) + (uint32_t)(r[0] | r[1] << 8);
        WW(at, (uint16_t)(RW(at) + seg));
    }
    free(d);
    *load_seg = seg;
    return 1;
}

void music_init(void) {
    extern char game_dir[512];
    char path[600];
    cfg_read();
    rm86_init();
    /* START.EXE main(): "if (MUSIC == 0 && SOUNDFX == 1) MUSIC = 1" */
    if (cfg_music == 0 && cfg_soundfx == 1) cfg_music = 1;
    snprintf(path, sizeof path, "%s\\START.EXE", game_dir);
    uint16_t L;
    if (!load_mz(path, &L)) { rt_log("music: cannot load %s; no driver music", path); return; }
    uint16_t seg_cfg = (uint16_t)(L + 0x549);        /* START's settings: +14h SOUNDFX, +16h MUSIC */
    WW(((uint32_t)seg_cfg << 4) + 0x14, (uint16_t)cfg_soundfx);
    WW(((uint32_t)seg_cfg << 4) + 0x16, (uint16_t)cfg_music);
    /* the song buffer START allocates (9A28h bytes) */
    uint16_t buf, maxav;
    if (!dos_alloc(0x9a3, &buf, &maxav)) { rt_log("music: no DOS memory for the song buffer"); return; }
    /* install (cdecl far: vector, buffer far pointer) */
    uint16_t args[3] = {(uint16_t)mus_vec, 0, buf};
    uint16_t regs[8] = {0}, segs[4] = {0, 0, 0, (uint16_t)(L + 0x843)};   /* DS = START's DGROUP */
    rm86_far_call(seg_cfg, 0x0041, regs, segs, args, 3);
    /* main(): FUN_087f -> 1549:0020 (AH=15h: hook int 8, the driver's timer
     * with its tick counter), then 1549:002a (AH=17h: PIT to 54.6 Hz) */
    memset(regs, 0, sizeof regs);
    rm86_far_call(seg_cfg, 0x0020, regs, segs, NULL, 0);
    memset(regs, 0, sizeof regs);
    rm86_far_call(seg_cfg, 0x002a, regs, segs, NULL, 0);
    installed = 1;
    rt_log("music: int 08h -> %04x:%04x, PIT reprogrammed by the driver", RW(0x22), RW(0x20));
    uint32_t v = RW((uint32_t)mus_vec * 4) + ((uint32_t)RW((uint32_t)mus_vec * 4 + 2) << 4);
    rt_log("music: START.EXE drivers installed at %04x (MUSIC=%d), int %02xh -> %05x %s", L, cfg_music, mus_vec, v,
           v >= 4 && !memcmp(MEM + v - 4, "DID!", 4) ? "(DID! ok)" : "(no DID! signature)");
}
