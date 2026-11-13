/* Head tracking for the virtual cockpit (F10): tfx.ini head_tracking=.
 *
 * The source is the FreeTrack interface, which opentrack and other trackers
 * write: a named shared memory block, FT_SharedMem, that starts with
 *   uint32 DataID (counts up with every pose), int32 CamWidth, CamHeight,
 *   float Yaw (radians), Pitch, Roll,
 *   float X, Y, Z (mm), ...
 * (opentrack: freetrackclient/fttypes.h, proto-ft).  Only yaw and pitch are
 * used.  As opentrack writes them, yaw is positive to the left and pitch is
 * positive DOWN (the header's comment says up; a real tracker showed
 * otherwise, DEBUG_LOG 120).
 *
 * The game side (TFX_CODE.md 9.23): the virtual cockpit, view 9, is drawn by
 * 0x420530 from a 3x3 matrix of 1.15 fixed-point words at 0x4987F0, relative
 * to the aircraft.  Right Shift + the arrow keys rotate it in steps, and
 * 0x41F544 holds it within the limits in the words at 0x4987D4 (pitch,
 * 4.2 degrees) and 0x4987D6 (yaw, 26.7 degrees).  The cockpit and the
 * aircraft around it are a complete model, though: with the limits raised the
 * view turns all the way round.
 *
 * rt_head_track() is a hook at the entry of 0x420530.  While poses arrive it
 * raises the two limits and writes the matrix for the head's yaw and pitch,
 * built as the game builds it (identity, its yaw rotation 0x45C5B4, then its
 * pitch rotation 0x45C4A8) but from exact sines instead of the game's
 * 1024-step table.  When the poses stop, the limits go back and the view
 * looks straight ahead again. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <windows.h>
#include "rt.h"
#include "cpu.h"
#include "settings.h"

struct ft_data {
    uint32_t data_id;
    int32_t cam_w, cam_h;
    float yaw, pitch, roll, x, y, z;
};
#define FT_HEAP_SIZE 108                    /* FTHeap: FTData (92 bytes) + GameID, an 8-byte table, GameID2 */

static const volatile struct ft_data *ft;
static uint32_t last_id, changed_ms;
static int tried, seen;
static unsigned frames_tracked;

/* the head's yaw and pitch in radians; 0 when no tracker is writing */
static int head_pose(float *yaw, float *pitch) {
    if (!tried) {
        tried = 1;
        /* created when no tracker has yet, as FreeTrack's own client library
         * does, so one started later finds it */
        HANDLE m = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, FT_HEAP_SIZE, "FT_SharedMem");
        if (m) ft = (const volatile struct ft_data *)MapViewOfFile(m, FILE_MAP_READ, 0, 0, FT_HEAP_SIZE);
        if (!ft) rt_log("head tracking: the FreeTrack shared memory can't be opened (error %lu)", GetLastError());
        else last_id = ft->data_id;
    }
    if (!ft) return 0;
    uint32_t now = GetTickCount(), id = ft->data_id;
    if (id != last_id) {
        last_id = id; changed_ms = now;
        if (!seen) { seen = 1; rt_log("head tracking: a FreeTrack source is sending poses"); }
    }
    if (!seen || now - changed_ms > 1000) return 0;        /* none for a second: the tracker stopped */
    float y = ft->yaw, p = ft->pitch;
    if (!(y == y) || !(p == p)) return 0;                   /* NaN */
    *yaw = y; *pitch = p;
    return 1;
}

#define LIM_PITCH 0x4987d4
#define LIM_YAW 0x4987d6
#define VIEW_MATRIX 0x4987f0

void rt_head_track(void) {
    static int raised;
    static uint16_t saved_pitch, saved_yaw;
    float yaw, pitch;
    if (!settings.head_tracking || !head_pose(&yaw, &pitch)) {
        if (raised) {
            WW(LIM_PITCH, saved_pitch); WW(LIM_YAW, saved_yaw);
            for (int i = 0; i < 9; i++) WW(VIEW_MATRIX + 2 * i, i % 4 == 0 ? 0x7fff : 0);     /* straight ahead */
            raised = 0;
            rt_log("head tracking: no poses, the virtual cockpit is back on the keys");
        }
        return;
    }
    if (!raised) {
        saved_pitch = RW(LIM_PITCH); saved_yaw = RW(LIM_YAW);
        raised = 1;
    }
    WW(LIM_PITCH, 0x3c00);                  /* 84 degrees: above the +-80 written below */
    WW(LIM_YAW, 0x7800);                    /* 169 degrees, above +-160 */
    const float d2r = 3.14159265f / 180.0f;
    if (yaw > 160 * d2r) yaw = 160 * d2r;
    if (yaw < -160 * d2r) yaw = -160 * d2r;
    if (pitch > 80 * d2r) pitch = 80 * d2r;
    if (pitch < -80 * d2r) pitch = -80 * d2r;
    /* The game's rotations turn the view right and down for positive angles
     * (its left arrow key moves the cockpit, not the head); the tracker's yaw
     * is positive to the left and its pitch positive down. */
    float a = -yaw, b = pitch;
    float s = sinf(a), c = cosf(a), sb = sinf(b), cb = cosf(b);
    float m[9] = {c, -s * sb, s * cb,
                  0, cb, sb,
                  -s, -c * sb, c * cb};
    for (int i = 0; i < 9; i++) WW(VIEW_MATRIX + 2 * i, (uint16_t)(int16_t)lrintf(m[i] * 32767.0f));
    frames_tracked++;
}

void rt_head_track_log(void) {
    if (!seen) return;
    rt_log("head tracking: %u frames; yaw %.1f pitch %.1f degrees; view matrix %d %d %d / %d %d %d / %d %d %d", frames_tracked,
           ft->yaw * 57.29578f, ft->pitch * 57.29578f, (int16_t)RW(VIEW_MATRIX), (int16_t)RW(VIEW_MATRIX + 2),
           (int16_t)RW(VIEW_MATRIX + 4), (int16_t)RW(VIEW_MATRIX + 6), (int16_t)RW(VIEW_MATRIX + 8), (int16_t)RW(VIEW_MATRIX + 10),
           (int16_t)RW(VIEW_MATRIX + 12), (int16_t)RW(VIEW_MATRIX + 14), (int16_t)RW(VIEW_MATRIX + 16));
    frames_tracked = 0;
}
