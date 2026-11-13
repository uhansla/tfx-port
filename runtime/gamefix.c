/* Fixes for game code that assumes at least 2 clock ticks per frame.
 *
 * With framerate=high (tfx.ini) the frame limiter lets a frame take 1 tick
 * of the 54.6 Hz game clock instead of 2 (TFX_CODE.md, frame limiter).  Most
 * of the game scales by the tick count [0x4A3476], but integer arithmetic
 * that divides a small multiple of it rounds to 0 for 1 tick. */
#include "rt.h"
#include "cpu.h"

/* The chase view (F5), 0x4203C4: while the camera distance [0x4987B8] is
 * below 28h it grows by (ticks << 4) / 30 a frame; from 28h on the function
 * calls 0x41F410 instead, which also handles the view keys.  For 1 tick that
 * is 16 / 30 = 0: the camera stayed at its start (distance 5, on top of the
 * aircraft) and no view key worked any more (DEBUG_LOG 111).  Hook at the
 * function's entry: on every second 1-tick frame add the 1 that a 2-tick
 * frame adds (32 / 30). */
void rt_fix_chase_ease(void) {
    static int half;
    if ((int16_t)RW(0x4a357e) > 0) { half = 0; return; }    /* its start delay is still running */
    int16_t v = (int16_t)RW(0x4987b8);
    if (v >= 0x28 || RW(0x4a3476) != 1) return;
    if (half) WW(0x4987b8, (uint16_t)(v + 1));
    half ^= 1;
}

/* Looking around with Right Shift + the arrow keys: 0x41F41C (cockpit views,
 * the virtual cockpit) and 0x41F4B4 (outside views) turn the view by 400h
 * (5.6 degrees) a frame whatever the frame took, so with 1-tick frames it
 * turned twice as fast as at the original's 2 ticks.  Hook at their entry:
 * on 1-tick frames with a look key down, every second call is skipped. */
int rt_fix_pan_rate(void) {
    static int skip;
    if (RW(0x4a3476) != 1 || !(RB(0x4a358a) & 15)) { skip = 0; return 0; }
    skip ^= 1;
    return skip;
}

/* Widescreen: the game's object visibility test 0x44915C (TFX_CODE.md 9.21).
 * In: AX = radius, ESI/EDI/EBP = view-space X/Y/Z (the high words are used).
 * Out: carry set = not visible.  It keeps an object when its centre less its
 * radius projects within the viewport's half size (immediates the game
 * patches per viewport: x at 0x4491AE, 160 for the full view; y at 0x4491D6;
 * the shifts at 0x4491A4 / 0x4491CC), after rejecting |x| - r >= z, i.e.
 * more than 45 degrees to the side: 256 pixels from the centre.
 * A 16:9 window shows 213 pixels each side, 32:9 shows 427: objects and
 * terrain pieces out there were never drawn.  With the wide view on, this
 * replaces the test for the full-width viewport: the same, with the x limit
 * widened by the extension and without the 45 degree cap. */
float video_wide_ext(void);
static unsigned wide_extra, wide_tests, queue_max;

int rt_wide_cull(void) {
    float ext = video_wide_ext();
    int xlim = (int16_t)RW(0x4491ae);
    if (ext <= 0 || xlim != 0xa0) return 0;
    int ylim = (int16_t)RW(0x4491d6), shx = RB(0x4491a4) & 31, shy = RB(0x4491cc) & 31;
    int r = (int16_t)eax; if (r < 0) r = -r;
    int z = (int16_t)(ebp >> 16), zz = z;
    wide_tests++;
    { unsigned q = RW(0x450b68); if (q > queue_max) queue_max = q; }
    if (z < 0) {
        if (z == -32768) goto reject;
        zz = -z;
        if (zz - r > 0x10) goto reject;
    }
    if (zz > 0x4000) goto reject;
    int x = (int16_t)(esi >> 16); if (x < 0) x = -x;
    x -= r;
    int beyond = 0;
    if (x >= 0) {
        if (zz <= 0) goto reject;
        long long q = ((long long)x << shx) / zz;
        if (q > xlim + (int)(ext + 8.5f)) goto reject;
        beyond = x >= zz || q > xlim;                  /* the original test would have rejected it */
    }
    int y = (int16_t)(edi >> 16); if (y < 0) y = -y;
    y -= r;
    if (y >= 0) {
        if (y >= zz) goto reject;
        if ((((long long)y << shy) / zz) > ylim) goto reject;
    }
    wide_extra += (unsigned)beyond;
    rt_set_cf(0);
    return 1;
reject:
    rt_set_cf(1);
    return 1;
}

/* cmdfile S: objects tested and let through beyond the original limits, and
 * the fullest the draw queue (348 entries) got, since the last S */
void rt_wide_cull_log(void) {
    if (wide_tests)
        rt_log("wide cull: %u objects tested, %u beyond the 4:3 limit kept, draw queue up to %u of 348", wide_tests, wide_extra, queue_max);
    wide_tests = wide_extra = queue_max = 0;
}

/* Projection overflow.  The vertex projection divides x and y by z into 16
 * bits (0x4489D3, 0x4489E8); a vertex far to the side of a point just beyond
 * the near plane overflows.  The game expects it: its handler for the divide
 * exception continues at 0x4488DF, which abandons the object being drawn.
 * The generated code jumps there instead of stopping (recomp.py,
 * DIV_RECOVER); this hook at 0x4488DF only counts it. */
static unsigned div_recovered;
void rt_div_recovered(void) {
    if (!div_recovered++) rt_log("projection overflow: an object was dropped, as the game's own divide handler does");
}
void rt_div_recovered_log(void) {
    if (div_recovered) rt_log("projection overflows recovered: %u", div_recovered);
}
