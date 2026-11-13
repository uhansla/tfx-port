/* Native renderer, stages 3-5: turn the captured scene into GPU triangles.
 *
 * Positions are given in clip space with w = view-space Z, so the GPU does
 * perspective-correct depth and clips at the near plane in 3D (the game clips
 * in 2D after its own near-plane handling).  The mapping reproduces the
 * game's projection, x = 256*X/Z + cx and y = 256*Y/Z + cy in 320x200
 * game pixels, placed in the destination rectangle of the output.
 * Back faces are rejected as the game does: its screen-space test on the
 * game's own projected points where all vertices are projected, otherwise a
 * 3D orientation test whose sign is learned from the projected polygons.
 * Triangles keep the game's painter's order.
 * Stage 4: Gouraud and textured polygons carry their attributes (shade, or
 * texel column/row) and the remap table / texture window they use; the
 * windows (64 KB each) and tables are copied from guest memory every frame
 * and looked up by the pixel shader exactly as the game's span fills do,
 * but interpolated per pixel at the output resolution.
 * Stage 5: motion vectors.  A primitive's vertex list lives at a fixed place
 * in the model bytecode, so (list address, occurrence) identifies the same
 * polygon in consecutive frames and vertex k is the same model point.  Each
 * vertex carries its clip position in the previous scene frame; the pixel
 * shader writes previous minus current position.  This covers camera motion,
 * moving objects and moving parts alike without reconstructing matrices.
 * Instanced models (one list drawn several times) pick the previous instance
 * nearest in view space. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "scene.h"
#include "native.h"

uint32_t vga_palette_argb(uint8_t i);

#define NEAR_Z 16.0f            /* the game projects when Z >> 16 > 10h */
#define FAR_Z 32768.0f

static struct native_vertex *vbuf;
static int vcap;
static struct native_res res;
static uint8_t tex_store[NATIVE_MAX_TEX][65536];
static uint8_t table_store[NATIVE_MAX_TABLES][256];
static uint32_t tex_addr[NATIVE_MAX_TEX], table_addr[NATIVE_MAX_TABLES];

/* slot of the texture window / remap table at linear address a (copied on first use) */
static int tex_slot(uint32_t a) {
    for (int i = 0; i < res.ntex; i++) if (tex_addr[i] == a) return i;
    if (res.ntex >= NATIVE_MAX_TEX) return -1;
    int i = res.ntex++;
    tex_addr[i] = a;
    uint32_t n = a < MEM_SIZE ? (MEM_SIZE - a < 65536 ? MEM_SIZE - a : 65536) : 0;
    if (n) memcpy(tex_store[i], MEM + a, n);
    if (n < 65536) memset(tex_store[i] + n, 0, 65536 - n);
    return i;
}
static int table_slot(uint32_t a) {
    for (int i = 0; i < res.ntables; i++) if (table_addr[i] == a) return i;
    if (res.ntables >= NATIVE_MAX_TABLES) return -1;
    int i = res.ntables++;
    table_addr[i] = a;
    if (a == ~0u) for (int k = 0; k < 256; k++) table_store[i][k] = (uint8_t)k;   /* identity */
    else if (a + 256 <= MEM_SIZE) memcpy(table_store[i], MEM + a, 256);
    else memset(table_store[i], 0, 256);
    return i;
}
/* ---- frame history for motion vectors ---- */
#define HMAX_P 16384
#define HMAX_V 131072
#define HBITS 15
struct hprim { uint32_t key; int vfirst, next; uint16_t count; int16_t cx, cy; };
struct hist {
    int np, nv;
    uint32_t number, gen;
    struct hprim p[HMAX_P];
    float v[HMAX_V][3];                 /* view X/Y/Z (game units / 65536) */
    uint32_t hkey[1 << HBITS], hgen[1 << HBITS];
    int head[1 << HBITS], tail[1 << HBITS];
};
static struct hist hist[2];
static int hcur;                        /* hist[hcur] = the frame being drawn, hist[hcur ^ 1] = previous */
static uint32_t occ_key[1 << HBITS], occ_gen[1 << HBITS], occ_n[1 << HBITS], occ_g;

static uint32_t hslot(uint32_t key) { return (key * 2654435761u) >> (32 - HBITS); }
static int hist_find(const struct hist *h, uint32_t key) {
    for (uint32_t s = hslot(key), k = 0; k < (1u << HBITS); k++, s = (s + 1) & ((1u << HBITS) - 1)) {
        if (h->hgen[s] != h->gen) return -1;
        if (h->hkey[s] == key) return h->head[s];
    }
    return -1;
}
static void hist_add(struct hist *h, uint32_t key, const struct scene_vert *v, int count, int16_t cx, int16_t cy) {
    if (h->np >= HMAX_P || h->nv + count > HMAX_V) return;
    int i = h->np++;
    struct hprim *e = &h->p[i];
    e->key = key; e->vfirst = h->nv; e->count = (uint16_t)count; e->cx = cx; e->cy = cy; e->next = -1;
    for (int k = 0; k < count; k++, h->nv++) {
        h->v[h->nv][0] = (float)v[k].x / 65536.0f; h->v[h->nv][1] = (float)v[k].y / 65536.0f; h->v[h->nv][2] = (float)v[k].z / 65536.0f;
    }
    for (uint32_t s = hslot(key), k = 0; k < (1u << HBITS); k++, s = (s + 1) & ((1u << HBITS) - 1)) {
        if (h->hgen[s] != h->gen) { h->hgen[s] = h->gen; h->hkey[s] = key; h->head[s] = h->tail[s] = i; return; }
        if (h->hkey[s] == key) { h->p[h->tail[s]].next = i; h->tail[s] = i; return; }
    }
}
/* occurrence number of key in the frame being built */
static int occurrence(uint32_t key) {
    for (uint32_t s = hslot(key), k = 0; k < (1u << HBITS); k++, s = (s + 1) & ((1u << HBITS) - 1)) {
        if (occ_gen[s] != occ_g) { occ_gen[s] = occ_g; occ_key[s] = key; occ_n[s] = 1; return 0; }
        if (occ_key[s] == key) return (int)occ_n[s]++;
    }
    return 0;
}
/* the previous frame's primitive for (key, occurrence): -1 if none */
static int hist_match(const struct hist *h, uint32_t key, int occ, int count, const float *want, int *instanced) {
    int first = hist_find(h, key);
    if (first < 0) return -1;
    int c = 0;
    for (int i = first; i >= 0 && c < 65; i = h->p[i].next) c++;
    if (c == 1) return h->p[first].count == count ? first : -1;
    *instanced = 1;
    if (c <= 64) {                      /* the instance nearest to where vertex 0 is expected */
        float x = want[0], y = want[1], z = want[2];
        int best = -1; float bd = 0;
        for (int i = first; i >= 0; i = h->p[i].next) {
            if (h->p[i].count != count) continue;
            const float *q = h->v[h->p[i].vfirst];
            float d = (q[0] - x) * (q[0] - x) + (q[1] - y) * (q[1] - y) + (q[2] - z) * (q[2] - z);
            if (best < 0 || d < bd) { best = i; bd = d; }
        }
        return best;
    }
    int i = first;
    for (int k = 0; k < occ && i >= 0; k++) i = h->p[i].next;
    return i >= 0 && h->p[i].count == count ? i : -1;
}

/* ---- camera delta: rigid motion of static scenery between two frames ----
 * prev ≈ R·cur + t, fitted with Horn's quaternion method on vertices of
 * primitives that matched uniquely, refitted without outliers (moving
 * objects).  Used to choose among instances and for primitives with no match. */
struct rigid { double r[3][3], t[3]; int valid; };
static struct rigid cam;

static void jacobi4(double a[4][4], double v[4][4], double e[4]) {
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) v[i][j] = i == j;
    for (int sweep = 0; sweep < 50; sweep++) {
        double off = 0;
        for (int i = 0; i < 4; i++) for (int j = i + 1; j < 4; j++) off += a[i][j] * a[i][j];
        if (off < 1e-22) break;
        for (int p = 0; p < 4; p++) for (int q = p + 1; q < 4; q++) {
            if (fabs(a[p][q]) < 1e-30) continue;
            double th = (a[q][q] - a[p][p]) / (2 * a[p][q]);
            double t = (th >= 0 ? 1 : -1) / (fabs(th) + sqrt(th * th + 1));
            double c = 1 / sqrt(t * t + 1), s = t * c;
            for (int k = 0; k < 4; k++) {
                double akp = a[k][p], akq = a[k][q];
                a[k][p] = c * akp - s * akq; a[k][q] = s * akp + c * akq;
            }
            for (int k = 0; k < 4; k++) {
                double apk = a[p][k], aqk = a[q][k];
                a[p][k] = c * apk - s * aqk; a[q][k] = s * apk + c * aqk;
            }
            for (int k = 0; k < 4; k++) {
                double vkp = v[k][p], vkq = v[k][q];
                v[k][p] = c * vkp - s * vkq; v[k][q] = s * vkp + c * vkq;
            }
        }
    }
    for (int i = 0; i < 4; i++) e[i] = a[i][i];
}

/* fit prev = R cur + t over pairs with use[i] set */
static int fit_rigid(const float (*a)[3], const float (*b)[3], const uint8_t *use, int n, struct rigid *out) {
    double ca[3] = {0}, cb[3] = {0};
    int m = 0;
    for (int i = 0; i < n; i++) if (use[i]) { for (int k = 0; k < 3; k++) { ca[k] += a[i][k]; cb[k] += b[i][k]; } m++; }
    if (m < 3) return 0;
    for (int k = 0; k < 3; k++) { ca[k] /= m; cb[k] /= m; }
    double S[3][3] = {{0}};
    for (int i = 0; i < n; i++) if (use[i])
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) S[r][c] += (a[i][r] - ca[r]) * (b[i][c] - cb[c]);
    double N[4][4] = {
        {S[0][0] + S[1][1] + S[2][2], S[1][2] - S[2][1], S[2][0] - S[0][2], S[0][1] - S[1][0]},
        {S[1][2] - S[2][1], S[0][0] - S[1][1] - S[2][2], S[0][1] + S[1][0], S[2][0] + S[0][2]},
        {S[2][0] - S[0][2], S[0][1] + S[1][0], -S[0][0] + S[1][1] - S[2][2], S[1][2] + S[2][1]},
        {S[0][1] - S[1][0], S[2][0] + S[0][2], S[1][2] + S[2][1], -S[0][0] - S[1][1] + S[2][2]}};
    double V[4][4], e[4];
    jacobi4(N, V, e);
    int bi = 0;
    for (int i = 1; i < 4; i++) if (e[i] > e[bi]) bi = i;
    double w = V[0][bi], x = V[1][bi], y = V[2][bi], z = V[3][bi];
    double (*R)[3] = out->r;
    R[0][0] = w*w + x*x - y*y - z*z; R[0][1] = 2*(x*y - w*z);         R[0][2] = 2*(x*z + w*y);
    R[1][0] = 2*(x*y + w*z);         R[1][1] = w*w - x*x + y*y - z*z; R[1][2] = 2*(y*z - w*x);
    R[2][0] = 2*(x*z - w*y);         R[2][1] = 2*(y*z + w*x);         R[2][2] = w*w - x*x - y*y + z*z;
    for (int r = 0; r < 3; r++) out->t[r] = cb[r] - (R[r][0] * ca[0] + R[r][1] * ca[1] + R[r][2] * ca[2]);
    out->valid = 1;
    return m;
}
static int native_cam_valid(const struct rigid *T) { return T->valid; }
static void apply_rigid(const struct rigid *T, const float *p, float *o) {
    for (int r = 0; r < 3; r++) o[r] = (float)(T->r[r][0] * p[0] + T->r[r][1] * p[1] + T->r[r][2] * p[2] + T->t[r]);
}

#define CAM_MAXP 32768
static float cam_a[CAM_MAXP][3], cam_b[CAM_MAXP][3], cam_sorted[CAM_MAXP];
static uint8_t cam_use[CAM_MAXP];
static int cmpf(const void *x, const void *y) { float a = *(const float *)x, b = *(const float *)y; return a < b ? -1 : a > b; }

#define CAM_TOL 1.0f                /* game units; static points fit to ~0.05 */
static float residual(const struct rigid *T, int i) {
    float o[3]; apply_rigid(T, cam_a[i], o);
    return sqrtf((o[0] - cam_b[i][0]) * (o[0] - cam_b[i][0]) + (o[1] - cam_b[i][1]) * (o[1] - cam_b[i][1]) +
                 (o[2] - cam_b[i][2]) * (o[2] - cam_b[i][2]));
}
/* RANSAC over the pairs not excluded (avail): rigid transforms from random
 * triples, the one with most pairs within CAM_TOL, refitted on those.
 * Marks its inliers in cam_use; returns their count. */
static int ransac(const uint8_t *avail, int np, struct rigid *out) {
    static uint32_t seed = 12345;
    static uint8_t tri[CAM_MAXP];
    int idx[3], best = 0, navail = 0;
    for (int i = 0; i < np; i++) navail += avail[i];
    if (navail < 8) return 0;
    struct rigid T, bestT;
    memset(tri, 0, (size_t)np);
    for (int it = 0; it < 64; it++) {
        for (int k = 0; k < 3; k++) {
            do { seed = seed * 1664525u + 1013904223u; idx[k] = (int)((seed >> 8) % (uint32_t)np); } while (!avail[idx[k]]);
            tri[idx[k]] = 1;
        }
        int ok = fit_rigid(cam_a, cam_b, tri, np, &T);
        for (int k = 0; k < 3; k++) tri[idx[k]] = 0;
        if (!ok) continue;
        int c = 0;
        for (int i = 0; i < np; i++) if (avail[i] && residual(&T, i) < CAM_TOL) c++;
        if (c > best) { best = c; bestT = T; }
    }
    if (best < 8) return 0;
    for (int i = 0; i < np; i++) cam_use[i] = avail[i] && residual(&bestT, i) < CAM_TOL;
    if (!fit_rigid(cam_a, cam_b, cam_use, np, out)) return 0;
    int c = 0;
    for (int i = 0; i < np; i++) { cam_use[i] = avail[i] && residual(out, i) < CAM_TOL; c += cam_use[i]; }
    return c;
}
static float median_z(const uint8_t *sel, int np) {
    int n = 0;
    for (int i = 0; i < np; i++) if (sel[i]) cam_sorted[n++] = cam_a[i][2];
    if (!n) return 0;
    qsort(cam_sorted, (size_t)n, sizeof *cam_sorted, cmpf);
    return cam_sorted[n / 2];
}

static int orient_sign = 1;     /* 3D orientation vs the game's screen test */
/* diagnostics: instance matches rejected in the last build */
static struct { uint32_t key; int count, cands; float dist, z, stat; } rej[8];
static int rej_n;
void native_log_rejects(void) {
    for (int i = 0; i < rej_n; i++)
        rt_log("native:   rejected key %08x (%d verts, %d instances): nearest %.2f from the camera prediction, %.2f from the current position, Z %.1f",
               rej[i].key, rej[i].count, rej[i].cands, rej[i].dist, rej[i].stat, rej[i].z);
}
/* O native_debug=N: 1 skip flat, discs and lines, 2 skip Gouraud,
 * 4 skip textured, 8 textured in solid magenta, 16 no back-face culling,
 * 32 show motion vectors as colour (d3d12.c), 128 FSR runs but taa is shown,
 * 256 skip near-plane flat, 512 skip effects, 1024 skip plain flat,
 * 2048 skip lines, 4096 skip discs */
int native_debug;
int native_probe;              /* S: log the polygons reaching the top of the view, once */
static int probe_n;

static int push(int n) {
    if (n > vcap) { vcap = n * 2; vbuf = realloc(vbuf, (size_t)vcap * sizeof *vbuf); }
    return vbuf != NULL;
}

/* the game's test on its own points: keep when >= 0 (decompiled 0x44C904) */
static long long screen_area(const struct scene_vert *v) {
    return (long long)(v[2].sy - v[0].sy) * (v[1].sx - v[0].sx) - (long long)(v[2].sx - v[0].sx) * (v[1].sy - v[0].sy);
}
static double orient3d(const struct scene_vert *v) {
    double ax = v[0].x, ay = v[0].y, az = v[0].z;
    double bx = v[1].x, by = v[1].y, bz = v[1].z;
    double cx = v[2].x, cy = v[2].y, cz = v[2].z;
    return ax * (by * cz - bz * cy) - ay * (bx * cz - bz * cx) + az * (bx * cy - by * cx);
}

/* clip rectangle of a primitive in game pixels, widened at the sides when it
 * covers the whole 320 width (the main 3D view; sub-viewports such as the
 * arming screen's keep theirs) */
static float g_ext;
static void wide_lr(const struct scene_prim *p, float *l, float *r) {
    int full = p->left <= 0 && p->right >= 319;
    *l = (float)p->left - (full ? g_ext : 0);
    *r = (float)(p->right + 1) + (full ? g_ext : 0);
}
/* ... and its top/bottom (bottom exclusive): in widescreen a full-width
 * view also reaches the whole screen height, so the world shows at the
 * sides below the cockpit panel (the game clips its 3D view where the
 * panel starts; in the middle the panel still covers it) */
static void wide_tb(const struct scene_prim *p, float *t, float *b) {
    int full = g_ext > 0 && p->left <= 0 && p->right >= 319;
    *t = full && p->top > 0 ? 0.0f : (float)p->top;
    *b = full && p->bottom < 199 ? 200.0f : (float)(p->bottom + 1);
}
int native_full_view(const struct scene_frame *f) {
    if (!f) return 0;
    for (int i = 0; i < f->nprims; i++)
        if (!f->prims[i].derived && f->prims[i].left <= 0 && f->prims[i].right >= 319) return 1;
    return 0;
}

/* Screen-space polygons (sky/haze/ground bands, TFX_CODE.md 9.12) are a
 * strip or half-plane clipped to the screen rectangle.  Edges lying along
 * the rectangle are clipping artefacts; the others are the true boundary
 * lines.  The polygon is rebuilt as the (wider) rectangle cut by those lines,
 * and attributes are extended with a plane fit in screen space (the game
 * interpolates them linearly on screen). */
/* does segment a-b (either direction) occur as an edge of polygon w? */
/* polygon area x 2 (screen points) */
static float area2_screen(const struct scene_vert *w, int wc) {
    float a = 0;
    for (int k = 0; k < wc; k++) a += (float)w[k].sx * w[(k + 1) % wc].sy - (float)w[(k + 1) % wc].sx * w[k].sy;
    return a;
}
/* is a-b a boundary between this band (centroid c) and another band of the
 * game's chain: an edge of a neighbouring band with area that lies on the
 * other side of it?  (Consecutive identical bands occur and share every
 * edge without being on the other side; zero-height bands are skipped.) */
static int edge_shared(float ax, float ay, float bx, float by, float cx0, float cy0,
                       const struct scene_frame *f, int i) {
    float nx = -(by - ay), ny = bx - ax;
    float side = nx * (cx0 - ax) + ny * (cy0 - ay);
    for (int j = i - 4; j <= i + 4; j++) {
        if (j == i || j < 0 || j >= f->nprims || f->prims[j].derived != 2) continue;
        const struct scene_vert *w = &f->verts[f->prims[j].first];
        int wc = f->prims[j].count > 32 ? 32 : f->prims[j].count;
        if (wc < 3 || fabsf(area2_screen(w, wc)) < 1.0f) continue;
        for (int k = 0; k < wc; k++) {
            const struct scene_vert *c = &w[k], *d = &w[(k + 1) % wc];
            float cx = c->sx, cy = c->sy, ex = d->sx, ey = d->sy;
            int same = (fabsf(cx - ax) <= 1 && fabsf(cy - ay) <= 1 && fabsf(ex - bx) <= 1 && fabsf(ey - by) <= 1) ||
                       (fabsf(cx - bx) <= 1 && fabsf(cy - by) <= 1 && fabsf(ex - ax) <= 1 && fabsf(ey - ay) <= 1);
            if (!same) continue;
            float wx = 0, wy = 0;
            for (int m = 0; m < wc; m++) { wx += w[m].sx; wy += w[m].sy; }
            wx /= (float)wc; wy /= (float)wc;
            float wside = nx * (wx - ax) + ny * (wy - ay);
            if (side * wside < 0) return 1;
        }
    }
    return 0;
}

/* Motion of the background (sky/haze bands): it is infinitely far away, so
 * it moves with the camera's rotation only.  A game pixel (gx, gy) is the
 * view direction ((gx - cx) / 256, (gy - cy) / 256, 1); the camera delta
 * takes it to the previous frame's view space, where it projects again.
 * Written as homogeneous clip coordinates (x w, y w, w), which are linear in
 * (gx, gy): interpolated over a polygon they give the exact previous position
 * of every pixel.  Without it the bands had no motion vectors, and DLSS
 * smeared the sky and the horizon glow in every roll (DEBUG_LOG 115).
 * Returns 0 when the direction was behind the previous camera. */
#define SKY_FAR 1.0e6f
static int screen_prev(const struct rigid *camd, int cx, int cy, float gx, float gy, int dx, int dy, float sx, float sy,
                       int ow, int oh, struct native_vertex *q) {
    float d[3] = {(gx - (float)cx) / 256.0f * SKY_FAR, (gy - (float)cy) / 256.0f * SKY_FAR, SKY_FAR}, o[3];
    apply_rigid(camd, d, o);
    if (o[2] < 0.01f * SKY_FAR) return 0;
    float w = o[2] / SKY_FAR;
    float pgx = (float)cx + 256.0f * o[0] / o[2], pgy = (float)cy + 256.0f * o[1] / o[2];
    q->px = (2.0f * ((float)dx + pgx * sx) / (float)ow - 1.0f) * w;
    q->py = (1.0f - 2.0f * ((float)dy + pgy * sy) / (float)oh) * w;
    q->pw = w;
    return 1;
}

static int build_screen(const struct scene_prim *p, const struct scene_vert *v, uint32_t info, uint32_t col,
                        int n, int dx, int dy, float sx, float sy, int ow, int oh, int uv_mode, float uv_scale, int shade_mode,
                        const struct scene_frame *f, int self, const struct rigid *camd) {
    int cnt = p->count > 32 ? 32 : p->count;
    if (cnt < 3) return n;
    /* the sky routine clips its polygons to the whole screen (x 0..319,
     * y 0..200), not to the 3D clip rectangle, which in the cockpit view
     * ends where the panel starts (TFX_CODE.md 9.14) */
    float L, R; wide_lr(p, &L, &R);
    float T, B; wide_tb(p, &T, &B);
    float poly[2][72][2]; int np = 4, cur = 0;
    poly[0][0][0] = L; poly[0][0][1] = T; poly[0][1][0] = R; poly[0][1][1] = T;
    poly[0][2][0] = R; poly[0][2][1] = B; poly[0][3][0] = L; poly[0][3][1] = B;
    if (fabsf(area2_screen(v, cnt)) < 1.0f) return n;     /* zero-height band: covers nothing in the original */
    {   /* a band wholly outside the view covers nothing there either.  (In a
         * near-vertical roll a sliver of ground haze at x ~ -6200 had a
         * corner rounded to y 199 instead of 200: that edge, taken for a
         * boundary and extended, painted the sky white) */
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (int k = 0; k < cnt; k++) {
            x0 = fminf(x0, v[k].sx); x1 = fmaxf(x1, v[k].sx); y0 = fminf(y0, v[k].sy); y1 = fmaxf(y1, v[k].sy);
        }
        float gx = x1 < L ? L - x1 : x0 > R ? x0 - R : 0, gy = y1 < T ? T - y1 : y0 > B ? y0 - B : 0;
        if (gx > 1 || gy > 1) {
            /* ... unless the band, which is a strip between two parallel
             * lines, slopes into the view beyond the game's own screen: in a
             * roll the far ground band lay just above the 4:3 picture's top
             * (y -1603..-2) and came down into the corner of the wide view,
             * which stayed empty (DEBUG_LOG 112).  Its direction: the longest
             * edge that isn't on a screen side */
            float ex = 0, ey = 0, best = 0;
            for (int k = 0; k < cnt; k++) {
                float ax = v[k].sx, ay = v[k].sy, bx = v[(k + 1) % cnt].sx, by = v[(k + 1) % cnt].sy;
                if ((ax == bx && (ax <= 0.5f || ax >= 318.5f)) || (ay == by && (ay <= 0.5f || ay >= 198.5f))) continue;
                float len = (bx - ax) * (bx - ax) + (by - ay) * (by - ay);
                if (len > best) { best = len; ex = fabsf(bx - ax); ey = fabsf(by - ay); }
            }
            if (best <= 0) return n;
            float span_x = fmaxf(R, x1) - fminf(L, x0), span_y = fmaxf(B, y1) - fminf(T, y0);
            if (gy > 1 && ex > 0 && ey / ex * span_x < gy) return n;
            if (gx > 1 && ey > 0 && ex / ey * span_y < gx) return n;
        }
    }
    /* the game's back-face test applies to these too.  The last sky band
     * reaches y ~ -32000; beyond the 16-bit range its far corners wrap
     * (e.g. -47400 -> +18136), the winding flips and the game skips it.
     * Drawn, it covered the whole sky in c251 (TFX_CODE.md 9.14) */
    if (p->cull) {
        long long a = (long long)(v[2].sy - v[0].sy) * (v[1].sx - v[0].sx) - (long long)(v[2].sx - v[0].sx) * (v[1].sy - v[0].sy);
        if (a < 0) return n;
    }
    float cxm = 0, cym = 0;
    for (int k = 0; k < cnt; k++) { cxm += v[k].sx; cym += v[k].sy; }
    cxm /= (float)cnt; cym /= (float)cnt;
    for (int k = 0; k < cnt && np >= 3; k++) {
        const struct scene_vert *a = &v[k], *b = &v[(k + 1) % cnt];
        float ax = a->sx, ay = a->sy, bx = b->sx, by = b->sy;
        /* a clipping artefact: an axis-aligned edge on a screen side.  (Testing
         * "beyond the 3D clip rectangle" instead took a low horizon for an
         * artefact: the ground polygon became the whole screen, painting the
         * sky brown in rolls and climbs) */
        /* ... unless the neighbouring band (previous or next in the game's
         * chain) has the same edge: then it is their common boundary, which
         * can fall on a screen side too (a horizon at row 199/200 made two
         * ground-haze bands boundary-less, i.e. the whole screen) */
        if (((ax == bx && (ax <= 0.5f || ax >= 318.5f)) || (ay == by && (ay <= 0.5f || ay >= 198.5f))) &&
            !edge_shared(ax, ay, bx, by, cxm, cym, f, self)) continue;
        float nx = -(by - ay), ny = bx - ax;
        if (nx == 0 && ny == 0) continue;
        float s = nx * (cxm - ax) + ny * (cym - ay) >= 0 ? 1.0f : -1.0f;
        int m = 0;
        for (int j = 0; j < np; j++) {                     /* Sutherland-Hodgman against this line */
            const float *P = poly[cur][j], *Q = poly[cur][(j + 1) % np];
            float dp = s * (nx * (P[0] - ax) + ny * (P[1] - ay)), dq = s * (nx * (Q[0] - ax) + ny * (Q[1] - ay));
            if (dp >= 0 && m < 70) { poly[cur ^ 1][m][0] = P[0]; poly[cur ^ 1][m][1] = P[1]; m++; }
            if ((dp >= 0) != (dq >= 0) && m < 70) {
                float t = dp / (dp - dq);
                poly[cur ^ 1][m][0] = P[0] + t * (Q[0] - P[0]); poly[cur ^ 1][m][1] = P[1] + t * (Q[1] - P[1]); m++;
            }
        }
        cur ^= 1; np = m;
    }
    if (np < 3) return n;
    /* attribute planes a = c0 + c1 x + c2 y (first non-degenerate triangle) */
    double ca[3] = {0}, cb[3] = {0};
    if ((info & 3) != NMODE_FLAT) {
        for (int k = 2; k < cnt; k++) {
            double x0 = v[0].sx, y0 = v[0].sy, x1 = v[1].sx - x0, y1 = v[1].sy - y0, x2 = v[k].sx - x0, y2 = v[k].sy - y0;
            double det = x1 * y2 - x2 * y1;
            if (det > -0.5 && det < 0.5) continue;
            double da1 = (double)v[1].a - v[0].a, da2 = (double)v[k].a - v[0].a;
            double db1 = (double)v[1].b - v[0].b, db2 = (double)v[k].b - v[0].b;
            ca[1] = (da1 * y2 - da2 * y1) / det; ca[2] = (x1 * da2 - x2 * da1) / det; ca[0] = v[0].a - ca[1] * x0 - ca[2] * y0;
            cb[1] = (db1 * y2 - db2 * y1) / det; cb[2] = (x1 * db2 - x2 * db1) / det; cb[0] = v[0].b - cb[1] * x0 - cb[2] * y0;
            break;
        }
    }
    if (!push(n + 3 * (np - 2))) return -1;
    struct native_vertex q[72];
    for (int k = 0; k < np; k++) {
        float gx = poly[cur][k][0], gy = poly[cur][k][1];
        memset(&q[k], 0, sizeof q[k]);
        q[k].x = 2.0f * ((float)dx + gx * sx) / (float)ow - 1.0f;
        q[k].y = 1.0f - 2.0f * ((float)dy + gy * sy) / (float)oh;
        q[k].z = 0.9999f; q[k].w = 1.0f;                   /* background: farthest */
        q[k].argb = col; q[k].info = info;
        q[k].clip[0] = q[k].clip[1] = q[k].clip[2] = q[k].clip[3] = 1.0f;
        q[k].px = q[k].x; q[k].py = q[k].y; q[k].pw = 1.0f;
        float a = (float)(ca[0] + ca[1] * gx + ca[2] * gy), b = (float)(cb[0] + cb[1] * gx + cb[2] * gy);
        if ((info & 3) == NMODE_GOURAUD) q[k].u = shade_mode == 1 ? a / 128.0f : a / 256.0f;
        else if ((info & 3) == NMODE_TEXTURED) {
            q[k].v = ((uv_mode & 1) ? b : a) * uv_scale;
            q[k].u = ((uv_mode & 1) ? a : b) * uv_scale;
        }
    }
    if (camd && camd->valid) {                              /* motion from the camera's rotation, all corners or none */
        struct native_vertex t[72];
        int ok = 1;
        for (int k = 0; k < np && ok; k++) {
            t[k] = q[k];
            ok = screen_prev(camd, p->cx, p->cy, poly[cur][k][0], poly[cur][k][1], dx, dy, sx, sy, ow, oh, &t[k]);
            t[k].info |= NINFO_MOTION;
        }
        if (ok) memcpy(q, t, (size_t)np * sizeof q[0]);
    }
    for (int k = 1; k + 1 < np; k++) { vbuf[n++] = q[0]; vbuf[n++] = q[k]; vbuf[n++] = q[k + 1]; }
    return n;
}

/* Widescreen: the far end of each chain of sky/haze bands continued as a
 * half-plane, behind everything.  The game's bands end at some distance from
 * the horizon (the far ground band about 1600 game pixels, less when looking
 * down), and beyond it the picture relies on terrain polygons, which the game
 * doesn't submit far outside its own 4:3 view: in a 32:9 window a roll left
 * the far corner of the ground side empty (DEBUG_LOG 112).
 * A chain is a run of bands that each share an edge with the next one (sky:
 * from the horizon up; ground haze: from far below up to the horizon).  Its
 * far end is whichever of its first and last band is thicker; the fill is
 * the wide view beyond the edge that band shares with its neighbour, in its
 * colour. */
static int band_shared_edge(const struct scene_frame *f, int i, int j, float *e) {
    const struct scene_prim *a = &f->prims[i], *b = &f->prims[j];
    const struct scene_vert *va = &f->verts[a->first], *vb = &f->verts[b->first];
    int ca = a->count > 32 ? 32 : a->count, cb = b->count > 32 ? 32 : b->count;
    for (int k = 0; k < cb; k++) {
        float cx = vb[k].sx, cy = vb[k].sy, ex = vb[(k + 1) % cb].sx, ey = vb[(k + 1) % cb].sy;
        if (cx == ex && cy == ey) continue;
        for (int m = 0; m < ca; m++) {
            float ax = va[m].sx, ay = va[m].sy, bx = va[(m + 1) % ca].sx, by = va[(m + 1) % ca].sy;
            if ((fabsf(cx - ax) <= 1 && fabsf(cy - ay) <= 1 && fabsf(ex - bx) <= 1 && fabsf(ey - by) <= 1) ||
                (fabsf(cx - bx) <= 1 && fabsf(cy - by) <= 1 && fabsf(ex - ax) <= 1 && fabsf(ey - ay) <= 1)) {
                e[0] = cx; e[1] = cy; e[2] = ex; e[3] = ey;
                return 1;
            }
        }
    }
    return 0;
}

static float band_thickness(const struct scene_frame *f, int i, const float *e) {
    const struct scene_prim *a = &f->prims[i];
    const struct scene_vert *va = &f->verts[a->first];
    int ca = a->count > 32 ? 32 : a->count;
    float nx = -(e[3] - e[1]), ny = e[2] - e[0], len = sqrtf(nx * nx + ny * ny), t = 0;
    if (len <= 0) return 0;
    for (int m = 0; m < ca; m++) t = fmaxf(t, fabsf(nx * (va[m].sx - e[0]) + ny * (va[m].sy - e[1])) / len);
    return t;
}

/* Widescreen: the colour the game cleared its 3D view to, behind everything.
 * Before the sky and haze bands the game fills the view with one colour
 * (the rectangle fill 0x458E34), and looking steeply up or down it draws no
 * bands at all: the 4:3 middle then shows that fill from the game's picture,
 * and the wide view's sides had nothing (black bars; DEBUG_LOG 121).
 * vga.c finds the colour in the displayed picture; -1 when none is left. */
int vga_clear_index(int top, int bottom);
static int build_clear_fill(const struct scene_frame *f, int n, int dx, int dy, float sx, float sy, int ow, int oh) {
    const struct scene_prim *pf = NULL;
    for (int i = 0; i < f->nprims && !pf; i++)
        if (!f->prims[i].derived && f->prims[i].left <= 0 && f->prims[i].right >= 319) pf = &f->prims[i];
    if (!pf) return n;
    int idx = vga_clear_index(pf->top, pf->bottom);
    if (idx < 0) return n;
    if (!push(n + 6)) return -1;
    float L, R, T, B; wide_lr(pf, &L, &R); wide_tb(pf, &T, &B);
    float c[4][2] = {{L, T}, {R, T}, {R, B}, {L, B}};
    struct native_vertex q[4];
    for (int j = 0; j < 4; j++) {
        memset(&q[j], 0, sizeof q[j]);
        q[j].x = 2.0f * ((float)dx + c[j][0] * sx) / (float)ow - 1.0f;
        q[j].y = 1.0f - 2.0f * ((float)dy + c[j][1] * sy) / (float)oh;
        q[j].z = 0.9999f; q[j].w = 1.0f;
        q[j].argb = vga_palette_argb((uint8_t)idx) | 0xff000000u; q[j].info = NMODE_FLAT | NINFO_MOTION;
        q[j].clip[0] = q[j].clip[1] = q[j].clip[2] = q[j].clip[3] = 1.0f;
        q[j].px = q[j].x; q[j].py = q[j].y; q[j].pw = 1.0f;     /* one colour: it has no motion to show */
    }
    vbuf[n++] = q[0]; vbuf[n++] = q[1]; vbuf[n++] = q[2];
    vbuf[n++] = q[0]; vbuf[n++] = q[2]; vbuf[n++] = q[3];
    return n;
}

static int build_far_fill(const struct scene_frame *f, int n, int dx, int dy, float sx, float sy, int ow, int oh,
                          const struct rigid *camd) {
    int idx[64], m = 0;
    for (int i = 0; i < f->nprims && m < 64; i++) {       /* the bands: the frame's first screen-space flat polygons */
        const struct scene_prim *p = &f->prims[i];
        if (p->derived != 2 || p->kind != PRIM_FLAT || p->count < 3) { if (m) break; else continue; }
        idx[m++] = i;
    }
    for (int start = 0, k = 0; k < m; k++) {
        float e[4];
        if (k + 1 < m && band_shared_edge(f, idx[k], idx[k + 1], e)) continue;
        int a = start, b = k;                               /* a chain: idx[a..b] */
        start = k + 1;
        if (b - a < 1) continue;
        float ea[4], eb[4];
        if (!band_shared_edge(f, idx[a], idx[a + 1], ea) || !band_shared_edge(f, idx[b], idx[b - 1], eb)) continue;
        int first_far = band_thickness(f, idx[a], ea) > band_thickness(f, idx[b], eb);
        int far = first_far ? idx[a] : idx[b], nb = first_far ? idx[a + 1] : idx[b - 1];
        const float *ed = first_far ? ea : eb;
        const struct scene_prim *pf = &f->prims[far], *pn = &f->prims[nb];
        /* the far band lies on the other side of the shared edge than its neighbour */
        const struct scene_vert *vn = &f->verts[pn->first];
        int cn = pn->count > 32 ? 32 : pn->count;
        float nx = -(ed[3] - ed[1]), ny = ed[2] - ed[0], side = 0;
        for (int q = 0; q < cn; q++) {
            float d = nx * (vn[q].sx - ed[0]) + ny * (vn[q].sy - ed[1]);
            if (fabsf(d) > fabsf(side)) side = d;
        }
        if (side == 0) continue;
        float s = side > 0 ? -1.0f : 1.0f;
        float L, R, T, B; wide_lr(pf, &L, &R); wide_tb(pf, &T, &B);
        float in[4][2] = {{L, T}, {R, T}, {R, B}, {L, B}}, out[8][2];
        int np = 0;
        for (int j = 0; j < 4; j++) {                       /* the wide view cut by that line */
            const float *P = in[j], *Q = in[(j + 1) % 4];
            float dp = s * (nx * (P[0] - ed[0]) + ny * (P[1] - ed[1])), dq = s * (nx * (Q[0] - ed[0]) + ny * (Q[1] - ed[1]));
            if (dp >= 0) { out[np][0] = P[0]; out[np][1] = P[1]; np++; }
            if ((dp >= 0) != (dq >= 0)) {
                float t = dp / (dp - dq);
                out[np][0] = P[0] + t * (Q[0] - P[0]); out[np][1] = P[1] + t * (Q[1] - P[1]); np++;
            }
        }
        if (np < 3) continue;
        if (!push(n + 3 * (np - 2))) return -1;
        struct native_vertex q[8];
        uint32_t col = vga_palette_argb(pf->color) | 0xff000000u;
        for (int j = 0; j < np; j++) {
            memset(&q[j], 0, sizeof q[j]);
            q[j].x = 2.0f * ((float)dx + out[j][0] * sx) / (float)ow - 1.0f;
            q[j].y = 1.0f - 2.0f * ((float)dy + out[j][1] * sy) / (float)oh;
            q[j].z = 0.9999f; q[j].w = 1.0f;
            q[j].argb = col; q[j].info = NMODE_FLAT;
            q[j].clip[0] = q[j].clip[1] = q[j].clip[2] = q[j].clip[3] = 1.0f;
            q[j].px = q[j].x; q[j].py = q[j].y; q[j].pw = 1.0f;
        }
        if (camd && camd->valid) {                          /* motion as for the bands */
            struct native_vertex t[8];
            int ok = 1;
            for (int j = 0; j < np && ok; j++) {
                t[j] = q[j];
                ok = screen_prev(camd, pf->cx, pf->cy, out[j][0], out[j][1], dx, dy, sx, sy, ow, oh, &t[j]);
                t[j].info |= NINFO_MOTION;
            }
            if (ok) memcpy(q, t, (size_t)np * sizeof q[0]);
        }
        for (int j = 1; j + 1 < np; j++) { vbuf[n++] = q[0]; vbuf[n++] = q[j]; vbuf[n++] = q[j + 1]; }
    }
    return n;
}

struct rigid;
static void apply_rigid(const struct rigid *T, const float *p, float *o);
static int native_cam_valid(const struct rigid *T);
/* a model line as a thin quad, LINE_W game pixels wide, at its 3D depth
 * (2D when an end was near-clipped); moved with the camera delta for motion */
#define LINE_W 0.6f
static int build_line(const struct scene_prim *p, const struct scene_vert *v, int n, int dx, int dy,
                      float sx, float sy, int ow, int oh, float zs, const struct rigid *camd) {
    if (!push(n + 6)) return -1;
    float px[2], py[2], w[2], z[2], ppx[2], ppy[2];
    int three_d = !v[0].near && !v[1].near && v[0].z > 0 && v[1].z > 0, moving = three_d && camd && camd->valid;
    for (int k = 0; k < 2; k++) {
        if (three_d) {
            float X = (float)v[k].x / 65536.0f, Y = (float)v[k].y / 65536.0f, Z = (float)v[k].z / 65536.0f;
            px[k] = (float)dx + ((float)p->cx + 256.0f * X / Z) * sx;
            py[k] = (float)dy + ((float)p->cy + 256.0f * Y / Z) * sy;
            w[k] = Z; z[k] = (Z - NEAR_Z) * zs;
            if (moving) {
                float c0[3] = {X, Y, Z}, q[3];
                apply_rigid(camd, c0, q);
                if (q[2] > 0.001f) {
                    ppx[k] = (float)dx + ((float)p->cx + 256.0f * q[0] / q[2]) * sx;
                    ppy[k] = (float)dy + ((float)p->cy + 256.0f * q[1] / q[2]) * sy;
                } else moving = 0;
            }
        } else {
            px[k] = (float)dx + ((float)v[k].sx + 0.5f) * sx; py[k] = (float)dy + ((float)v[k].sy + 0.5f) * sy;
            w[k] = 1.0f; z[k] = 0.0f;
        }
    }
    if (!three_d && camd && camd->valid) {
        /* a line known only in 2D (an end was near-clipped) had no motion at
         * all and left ghost outlines in rolls.  Its depth is unknown, so it
         * gets the camera's rotation, as the background does: exact in a
         * turn for anything far away, and nothing when flying straight */
        struct native_vertex t;
        moving = 1;
        for (int k = 0; k < 2 && moving; k++) {
            moving = screen_prev(camd, p->cx, p->cy, (float)v[k].sx + 0.5f, (float)v[k].sy + 0.5f, 0, 0, 1.0f, 1.0f, 2, 2, &t);
            if (moving) {       /* back from clip space (ow = oh = 2: x = g - 1, y = 1 - g) to game pixels */
                ppx[k] = (float)dx + (t.px / t.pw + 1.0f) * sx;
                ppy[k] = (float)dy + (1.0f - t.py / t.pw) * sy;
            }
        }
    }
    float ddx = px[1] - px[0], ddy = py[1] - py[0], len = sqrtf(ddx * ddx + ddy * ddy);
    if (len < 0.01f) return n;
    float nx = -ddy / len * LINE_W * 0.5f * sx, ny = ddx / len * LINE_W * 0.5f * sy;
    float gl, gr; wide_lr(p, &gl, &gr);
    float L = (float)dx + gl * sx, R = (float)dx + gr * sx;
    float gt, gb; wide_tb(p, &gt, &gb);
    float T = (float)dy + gt * sy, Bm = (float)dy + gb * sy;
    uint32_t col = vga_palette_argb(p->color) | 0xff000000u;
    struct native_vertex q[4];
    for (int c = 0; c < 4; c++) {
        int k = c >> 1;
        float s = (c & 1) ? 1.0f : -1.0f;
        float ox = px[k] + s * nx, oy = py[k] + s * ny;
        memset(&q[c], 0, sizeof q[c]);
        q[c].x = (2.0f * ox / (float)ow - 1.0f) * w[k];
        q[c].y = (1.0f - 2.0f * oy / (float)oh) * w[k];
        q[c].z = z[k]; q[c].w = w[k];
        q[c].argb = col; q[c].info = NMODE_FLAT | (moving ? NINFO_MOTION : 0);
        q[c].clip[0] = (ox - L) * w[k]; q[c].clip[1] = (R - ox) * w[k]; q[c].clip[2] = (oy - T) * w[k]; q[c].clip[3] = (Bm - oy) * w[k];
        float mx = moving ? ppx[k] - px[k] : 0, my = moving ? ppy[k] - py[k] : 0;
        q[c].px = (2.0f * (ox + mx) / (float)ow - 1.0f) * w[k];
        q[c].py = (1.0f - 2.0f * (oy + my) / (float)oh) * w[k];
        q[c].pw = w[k];
    }
    vbuf[n++] = q[0]; vbuf[n++] = q[1]; vbuf[n++] = q[3];
    vbuf[n++] = q[0]; vbuf[n++] = q[3]; vbuf[n++] = q[2];
    return n;
}

/* a filled disc as a 48-segment fan.  Game pixels aren't square, so it is
 * an ellipse in output pixels (rx = r * dw/320, ry = r * dh/200), centred on
 * the vertex's exact projection, with the exact (unrounded) radius. */
#define DISC_SEG 48
/* the game's stippled disc fills (cloud puffs, TFX_CODE.md 9.15): span
 * routines generated at run time that write only some VGA planes on each
 * row.  0x4726CE: planes 1+3 on even rows, 0+2 on odd rows (a checkerboard,
 * 50 %); 0x4727FF: plane 1 / plane 3 (25 %).  Checked by their "and al,
 * imm8" instructions.  Drawn here as that much opacity */
static uint32_t disc_alpha(uint32_t fill) {
    if (fill == 0x4726ceu && MEM[0x47275c] == 0x24 && MEM[0x47275d] == 0x0a && MEM[0x4727d0] == 0x24 && MEM[0x4727d1] == 0x05)
        return 0x80;
    if (fill == 0x4727ffu && MEM[0x47288c] == 0x24 && MEM[0x47288d] == 0x02 && MEM[0x472900] == 0x24 && MEM[0x472901] == 0x08)
        return 0x40;
    /* the other stippled styles of the fill table 0x4752C3 (TFX_CODE.md 9.18),
     * as the share of pixels they write: one plane of every other byte on
     * every other row (0x47292F, 1/16), one plane of every fourth byte on
     * every other row (0x472C36, 1/32), random bits of two planes (0x472F60,
     * 0x473ACD, 1/4) */
    if (fill == 0x47292fu) return 0x10;
    if (fill == 0x472c36u) return 0x08;
    if (fill == 0x472f60u || fill == 0x473acdu) return 0x40;
    return 0xff;
}
static int build_disc(const struct scene_prim *p, const struct scene_vert *v, int n, int dx, int dy,
                      float sx, float sy, int ow, int oh, float B, float D, float zs,
                      const float *prev, int16_t pcx, int16_t pcy) {
    if (!push(n + 3 * DISC_SEG)) return -1;
    float r = v->b ? (float)v->b / 256.0f : (float)v->a;
    float cxo, cyo, w, z;                      /* centre in output pixels, clip w, depth */
    if (!v->near && v->z > 0) {
        float X = (float)v->x / 65536.0f, Y = (float)v->y / 65536.0f, Z = (float)v->z / 65536.0f;
        cxo = (float)dx + ((float)p->cx + 256.0f * X / Z) * sx;
        cyo = (float)dy + ((float)p->cy + 256.0f * Y / Z) * sy;
        w = Z; z = (Z - NEAR_Z) * zs;
        if (z < 0) z = 0;
    } else {
        cxo = (float)dx + ((float)v->sx + 0.5f) * sx;
        cyo = (float)dy + ((float)v->sy + 0.5f) * sy;
        w = 1.0f; z = 0.0f;
    }
    (void)B; (void)D;
    /* previous centre (output pixels): the disc moves as a whole */
    float mdx = 0, mdy = 0;
    int moving = prev && !v->near && v->z > 0 && prev[2] > 0.001f;
    if (moving) {
        float pxo = (float)dx + ((float)pcx + 256.0f * prev[0] / prev[2]) * sx;
        float pyo = (float)dy + ((float)pcy + 256.0f * prev[1] / prev[2]) * sy;
        mdx = pxo - cxo; mdy = pyo - cyo;
    }
    float gl, gr; wide_lr(p, &gl, &gr);
    float L = (float)dx + gl * sx, R = (float)dx + gr * sx;
    float gt, gb; wide_tb(p, &gt, &gb);
    float T = (float)dy + gt * sy, Bm = (float)dy + gb * sy;
    uint32_t col = (vga_palette_argb(p->color) & 0xffffffu) | disc_alpha(p->fill) << 24;
    struct native_vertex ring[DISC_SEG + 1];
    for (int k = 0; k <= DISC_SEG; k++) {
        float px = cxo, py = cyo;
        if (k < DISC_SEG) {
            float t = 6.2831853f * (float)k / (float)DISC_SEG;
            px += r * sx * cosf(t); py += r * sy * sinf(t);
        }
        struct native_vertex *q = &ring[k];
        memset(q, 0, sizeof *q);
        q->x = (2.0f * px / (float)ow - 1.0f) * w;
        q->y = (1.0f - 2.0f * py / (float)oh) * w;
        q->z = z; q->w = w;                    /* z is already clip-space ((Z - near) * zs, as for polygons) */
        q->argb = col; q->info = NMODE_FLAT | (moving ? NINFO_MOTION : 0);
        q->clip[0] = px - L; q->clip[1] = R - px; q->clip[2] = py - T; q->clip[3] = Bm - py;
        q->px = (2.0f * (px + mdx) / (float)ow - 1.0f) * w;
        q->py = (1.0f - 2.0f * (py + mdy) / (float)oh) * w;
        q->pw = w;
    }
    for (int k = 0; k < DISC_SEG; k++) {
        vbuf[n++] = ring[DISC_SEG]; vbuf[n++] = ring[k]; vbuf[n++] = ring[(k + 1) % DISC_SEG];
    }
    return n;
}

int native_build(const struct scene_frame *f, int dx, int dy, int dw, int dh, int ow, int oh, float ext,
                 const struct native_vertex **out, const struct native_res **resp, struct native_stats *st) {
    memset(st, 0, sizeof *st);
    g_ext = ext;
    *resp = &res;
    res.ntex = res.ntables = 0;
    res.tex = tex_store; res.tables = table_store;
    if (!f) return 0;
    for (int i = 0; i < 256; i++) res.palette[i] = vga_palette_argb((uint8_t)i) | 0xff000000u;
    int uv_mode = scene_uv_mode(), shade_mode = scene_shade_mode();
    float uv_scale = uv_mode >= 2 ? 1.0f / 256.0f : 1.0f / 128.0f;
    int n = 0;
    float sx = (float)dw / 320.0f, sy = (float)dh / 200.0f;
    /* clip = A*Z + B*X  (x),  C*Z - D*Y  (y); A and C follow each primitive's centre */
    float B = 2.0f * 256.0f * sx / (float)ow, D = 2.0f * 256.0f * sy / (float)oh;
    float zs = FAR_Z / (FAR_Z - NEAR_Z);
    int agree = 0, disagree = 0;
    /* a new scene frame: it becomes the history the next one is matched against */
    int fill = f->number != hist[hcur].number;
    if (fill) {
        hcur ^= 1;
        struct hist *h = &hist[hcur];
        h->np = h->nv = 0; h->gen++; h->number = f->number;
    }
    st->new_frame = fill;
    const struct hist *prev = &hist[hcur ^ 1];
    int have_prev = prev->number && prev->number < f->number;
    st->history_frames = have_prev ? (int)(f->number - prev->number) : 0;
    occ_g++;
    static int occ_of[16384];
    static struct native_stats cam_stats;
    for (int i = 0; i < f->nprims && i < 16384; i++) {
        const struct scene_prim *p = &f->prims[i];
        if (p->derived || p->kind == PRIM_LINE) continue;
        occ_of[i] = occurrence(p->key);
        if (fill) hist_add(&hist[hcur], p->key, f->verts + p->first, p->count, p->cx, p->cy);
    }
    if (fill) {                                   /* camera delta from uniquely matched primitives */
        memset(&cam_stats, 0, sizeof cam_stats);
        cam.valid = 0;
        int np = 0;
        for (int i = 0; have_prev && i < f->nprims && i < 16384; i++) {
            const struct scene_prim *p = &f->prims[i];
            if (p->derived || p->kind == PRIM_DISC || p->kind == PRIM_LINE || p->kind == PRIM_POINT) continue;
            int first = hist_find(prev, p->key);
            if (first < 0 || prev->p[first].next >= 0 || prev->p[first].count != p->count) continue;
            const struct scene_vert *v = &f->verts[p->first];
            for (int k = 0; k < p->count && np < CAM_MAXP; k++, np++) {
                cam_a[np][0] = (float)v[k].x / 65536.0f; cam_a[np][1] = (float)v[k].y / 65536.0f; cam_a[np][2] = (float)v[k].z / 65536.0f;
                memcpy(cam_b[np], prev->v[prev->p[first].vfirst + k], sizeof cam_b[np]);
                cam_use[np] = 1;
            }
        }
        cam_stats.cam_pairs = np;
        /* the dominant rigid motion, then a second one among the rest: in a
         * chase view the aircraft rides with the camera and is a large
         * consistent group of its own.  The world is the group farther away. */
        static uint8_t avail[CAM_MAXP], set1[CAM_MAXP];
        memset(avail, 1, (size_t)np);
        struct rigid t1, t2;
        int m = ransac(avail, np, &t1);
        if (m) {
            memcpy(set1, cam_use, (size_t)np);
            for (int i = 0; i < np; i++) avail[i] = !set1[i];
            int m2 = ransac(avail, np, &t2);
            if (m2 >= m / 4 && median_z(cam_use, np) > median_z(set1, np)) { cam = t2; m = m2; }
            else { cam = t1; memcpy(cam_use, set1, (size_t)np); }
            cam_stats.cam_groups = m2 >= 8 ? 2 : 1;
        }
        if (m) {
            double tr = cam.r[0][0] + cam.r[1][1] + cam.r[2][2], ss = 0;
            int ni = 0;
            for (int i = 0; i < np; i++) if (cam_use[i]) {
                float o[3]; apply_rigid(&cam, cam_a[i], o);
                ss += (o[0] - cam_b[i][0]) * (o[0] - cam_b[i][0]) + (o[1] - cam_b[i][1]) * (o[1] - cam_b[i][1]) +
                      (o[2] - cam_b[i][2]) * (o[2] - cam_b[i][2]);
                ni++;
            }
            cam_stats.cam_valid = 1; cam_stats.cam_inliers = m;
            cam_stats.cam_angle = (float)(acos(fmax(-1.0, fmin(1.0, (tr - 1) / 2))) * 57.29578);
            cam_stats.cam_move = (float)sqrt(cam.t[0] * cam.t[0] + cam.t[1] * cam.t[1] + cam.t[2] * cam.t[2]);
            cam_stats.cam_rms = ni ? (float)sqrt(ss / ni) : 0;
        }
    }
    st->cam_valid = cam_stats.cam_valid; st->cam_pairs = cam_stats.cam_pairs; st->cam_inliers = cam_stats.cam_inliers;
    st->cam_groups = cam_stats.cam_groups;
    st->cam_angle = cam_stats.cam_angle; st->cam_move = cam_stats.cam_move; st->cam_rms = cam_stats.cam_rms;
    double mv_sum = 0;
    int mv_n = 0;
    rej_n = 0;
    static float cam_prev[64][3];                 /* per-vertex previous position from the camera delta */
    if (g_ext > 0) {                              /* widescreen: ground and sky continued behind everything */
        if (!(native_debug & 64)) n = build_clear_fill(f, n, dx, dy, sx, sy, ow, oh);
        if (n < 0) return 0;
        if (!(native_debug & 128)) n = build_far_fill(f, n, dx, dy, sx, sy, ow, oh, have_prev ? &cam : NULL);
        if (n < 0) return 0;
    }
    for (int i = 0; i < f->nprims; i++) {
        const struct scene_prim *p = &f->prims[i];
        if (p->derived == 2) {                             /* screen-space background, rebuilt wide */
            uint32_t sinfo = NMODE_FLAT;
            if (p->kind == PRIM_GOURAUD || p->kind == PRIM_GOURAUD_NEAR) {
                int t = table_slot(shade_mode == 2 ? ~0u : p->table);
                if (t < 0) continue;
                sinfo = NMODE_GOURAUD | (uint32_t)t << 8;
            } else if (p->kind == PRIM_TEXTURED || p->kind == PRIM_TEXTURED_NEAR) {
                int t = table_slot(p->table), x = tex_slot(p->tex);
                if (t < 0 || x < 0) continue;
                sinfo = NMODE_TEXTURED | (uint32_t)t << 8 | (uint32_t)x << 16;
            }
            n = build_screen(p, &f->verts[p->first], sinfo, vga_palette_argb(p->color) | 0xff000000u, n, dx, dy, sx, sy,
                             ow, oh, uv_mode, uv_scale, shade_mode, f, i, have_prev ? &cam : NULL);
            if (n < 0) return 0;
            st->screen++;
            continue;
        }
        if (p->derived) { st->skipped_derived++; continue; }
        const struct scene_vert *v = &f->verts[p->first];
        if (p->kind == PRIM_LINE) {
            if (native_debug & (1 | 2048)) continue;      /* 2048: lines only */
            if (native_probe && (v[0].near || v[1].near))
                rt_log("probe: line #%d colour %d 2D (%d,%d)-(%d,%d) near %d/%d", i, p->color, v[0].sx, v[0].sy, v[1].sx, v[1].sy,
                       v[0].near, v[1].near);
            n = build_line(p, v, n, dx, dy, sx, sy, ow, oh, zs, have_prev ? &cam : NULL);
            if (n < 0) return 0;
            st->drawn++; st->lines++;
            if (p->fill == 1) st->lines_454336++;
            continue;
        }
        /* identity in the previous frame */
        int occ = i < 16384 ? occ_of[i] : 0, inst = 0;
        float want[3] = {(float)v[0].x / 65536.0f, (float)v[0].y / 65536.0f, (float)v[0].z / 65536.0f};
        if (cam.valid) { float c0[3] = {want[0], want[1], want[2]}; apply_rigid(&cam, c0, want); }
        int pm = have_prev ? hist_match(prev, p->key, occ, p->count, want, &inst) : -1;
        const struct hprim *hp = pm >= 0 ? &prev->p[pm] : NULL;
        if (hp && inst && cam.valid) {
            /* the nearest instance should sit where the camera delta puts it,
             * give or take the object's own motion: farther means another copy */
            const float *q = prev->v[hp->vfirst];
            float d2 = (q[0] - want[0]) * (q[0] - want[0]) + (q[1] - want[1]) * (q[1] - want[1]) + (q[2] - want[2]) * (q[2] - want[2]);
            if (d2 > 25.0f) {
                if (rej_n < 8) {
                    int c = 0;
                    for (int j = hist_find(prev, p->key); j >= 0; j = prev->p[j].next) c++;
                    rej[rej_n].key = p->key; rej[rej_n].count = p->count; rej[rej_n].cands = c;
                    rej[rej_n].dist = sqrtf(d2); rej[rej_n].z = want[2];
                    rej[rej_n].stat = sqrtf((q[0] - (float)v[0].x / 65536.0f) * (q[0] - (float)v[0].x / 65536.0f) +
                                            (q[1] - (float)v[0].y / 65536.0f) * (q[1] - (float)v[0].y / 65536.0f) +
                                            (q[2] - (float)v[0].z / 65536.0f) * (q[2] - (float)v[0].z / 65536.0f));
                    rej_n++;
                }
                hp = NULL; st->instance_rejected++;
            }
        }
        if (hp) { st->matched++; st->instanced += inst; } else st->unmatched++;
        /* no match: static scenery assumed, moved by the camera delta */
        int from_cam = !hp && have_prev && cam.valid && p->count <= 64;
        if (from_cam) {
            st->from_camera++;
            for (int k = 0; k < p->count; k++) {
                float c0[3] = {(float)v[k].x / 65536.0f, (float)v[k].y / 65536.0f, (float)v[k].z / 65536.0f};
                apply_rigid(&cam, c0, cam_prev[k]);
            }
        }
        if (hp && !v[0].near && v[0].z > 0 && prev->v[hp->vfirst][2] > NEAR_Z &&
            v[0].sx >= 0 && v[0].sx < 320 && v[0].sy >= 0 && v[0].sy < 200) {     /* on screen */
            const float *q = prev->v[hp->vfirst];
            float Z = (float)v[0].z / 65536.0f;
            float cx0 = ((float)p->cx + 256.0f * ((float)v[0].x / 65536.0f) / Z) * sx;
            float cy0 = ((float)p->cy + 256.0f * ((float)v[0].y / 65536.0f) / Z) * sy;
            float px0 = ((float)hp->cx + 256.0f * q[0] / q[2]) * sx, py0 = ((float)hp->cy + 256.0f * q[1] / q[2]) * sy;
            float m = sqrtf((px0 - cx0) * (px0 - cx0) + (py0 - cy0) * (py0 - cy0));
            mv_sum += m; mv_n++;
            if (m > st->mv_max) st->mv_max = m;
            st->mv_hist[inst][m < 2 ? 0 : m < 10 ? 1 : m < 50 ? 2 : 3]++;
        }
        if (p->kind == PRIM_POINT) {
            /* a VM point (light): a round dot with the area of the game's
             * square (1x1 or 2x2 game pixels), centred where that square is:
             * the exact projection, half a pixel further for the 2x2 */
            if (native_debug & (1 | 4096)) continue;
            struct scene_vert pv = *v;
            float size = (float)v->a;
            pv.b = (uint16_t)(size * 0.5642f * 256.0f);
            if (!v->near && v->z > 0 && size > 1) {
                int32_t off = (int32_t)(0.5 * (size - 1) * ((double)v->z / 256.0));
                pv.x += off; pv.y += off;
            }
            n = build_disc(p, &pv, n, dx, dy, sx, sy, ow, oh, B, D, zs,
                           hp ? prev->v[hp->vfirst] : from_cam ? cam_prev[0] : NULL,
                           hp ? hp->cx : p->cx, hp ? hp->cy : p->cy);
            if (n < 0) return 0;
            st->drawn++; st->points++;
            continue;
        }
        if (p->kind == PRIM_DISC) {
            if (native_debug & (1 | 4096)) continue;      /* 4096: discs only */
            if (native_probe)
                rt_log("probe: disc #%d colour %d at %d,%d r %d exact %.2f near %d view %d %d %d", i, p->color, v->sx, v->sy, v->a,
                       v->b / 256.0, v->near, v->x >> 16, v->y >> 16, v->z >> 16);
            n = build_disc(p, v, n, dx, dy, sx, sy, ow, oh, B, D, zs,
                           hp ? prev->v[hp->vfirst] : from_cam ? cam_prev[0] : NULL,
                           hp ? hp->cx : p->cx, hp ? hp->cy : p->cy);
            if (n < 0) return 0;
            st->drawn++; st->discs++;
            continue;
        }
        if (p->count < 3) continue;
        uint32_t info = NMODE_FLAT;
        {
            int g = p->kind == PRIM_GOURAUD || p->kind == PRIM_GOURAUD_NEAR;
            int t = p->kind == PRIM_TEXTURED || p->kind == PRIM_TEXTURED_NEAR;
            if ((native_debug & 1) && !g && !t) continue;
            if ((native_debug & 256) && p->kind == PRIM_FLAT_NEAR) continue;   /* debug: near-plane flat only */
            if ((native_debug & 512) && p->kind == PRIM_EFFECT) continue;      /* effects only */
            if ((native_debug & 1024) && p->kind == PRIM_FLAT) continue;       /* plain flat only */
            if ((native_debug & 2) && g) continue;
            if ((native_debug & 4) && t) continue;
        }
        if (p->kind == PRIM_GOURAUD || p->kind == PRIM_GOURAUD_NEAR) {
            int t = table_slot(shade_mode == 2 ? ~0u : p->table);
            if (t < 0) { st->tex_overflow++; continue; }
            info = NMODE_GOURAUD | (uint32_t)t << 8;
        } else if (p->kind == PRIM_TEXTURED || p->kind == PRIM_TEXTURED_NEAR) {
            int t = table_slot(p->table), x = tex_slot(p->tex);
            if (t < 0 || x < 0) { st->tex_overflow++; continue; }
            info = NMODE_TEXTURED | (uint32_t)t << 8 | (uint32_t)x << 16;
        }
        int all_projected = 1;
        for (int k = 0; k < p->count; k++) if (v[k].near) { all_projected = 0; break; }
        if (p->cull && !(native_debug & 16)) {
            if (all_projected) {
                long long a = screen_area(v);
                double o = orient3d(v);
                if (a != 0 && o != 0) { if ((a > 0) == (o > 0)) agree++; else disagree++; }
                if (a < 0) { st->culled++; continue; }
            } else if (orient3d(v) * orient_sign < 0) { st->culled++; continue; }
        }
        if (!push(n + 3 * (p->count - 2))) return 0;
        float A = 2.0f * ((float)dx + (float)p->cx * sx) / (float)ow - 1.0f;
        float C = 1.0f - 2.0f * ((float)dy + (float)p->cy * sy) / (float)oh;
        /* the clip rectangle (inclusive game pixels) as NDC bounds */
        float gl, gr; wide_lr(p, &gl, &gr);
        float L = 2.0f * ((float)dx + gl * sx) / (float)ow - 1.0f;
        float R = 2.0f * ((float)dx + gr * sx) / (float)ow - 1.0f;
        float gt, gb; wide_tb(p, &gt, &gb);
        float T = 1.0f - 2.0f * ((float)dy + gt * sy) / (float)oh;
        float Bm = 1.0f - 2.0f * ((float)dy + gb * sy) / (float)oh;
        uint32_t col = vga_palette_argb(p->color) | 0xff000000u;
        if ((native_debug & 8) && (info & 3) == NMODE_TEXTURED) { info = NMODE_FLAT; col = 0xffff00ffu; }
        float Ap = 0, Cp = 0;
        if (hp || from_cam) {
            int16_t pcx = hp ? hp->cx : p->cx, pcy = hp ? hp->cy : p->cy;
            info |= NINFO_MOTION;
            Ap = 2.0f * ((float)dx + (float)pcx * sx) / (float)ow - 1.0f;
            Cp = 1.0f - 2.0f * ((float)dy + (float)pcy * sy) / (float)oh;
        }
        struct native_vertex fan[64];
        int cnt = p->count > 64 ? 64 : p->count;
        for (int k = 0; k < cnt; k++) {
            float X = (float)v[k].x / 65536.0f, Y = (float)v[k].y / 65536.0f, Z = (float)v[k].z / 65536.0f;
            fan[k].x = A * Z + B * X;
            fan[k].y = C * Z - D * Y;
            fan[k].z = (Z - NEAR_Z) * zs;
            fan[k].w = Z;
            fan[k].argb = col;
            fan[k].info = info;
            fan[k].clip[0] = fan[k].x - L * Z;
            fan[k].clip[1] = R * Z - fan[k].x;
            fan[k].clip[2] = T * Z - fan[k].y;
            fan[k].clip[3] = fan[k].y - Bm * Z;
            if (hp || from_cam) {           /* the same model point in the previous frame */
                const float *q = hp ? prev->v[hp->vfirst + k] : cam_prev[k];
                fan[k].px = Ap * q[2] + B * q[0];
                fan[k].py = Cp * q[2] - D * q[1];
                fan[k].pw = q[2];
            } else { fan[k].px = fan[k].x; fan[k].py = fan[k].y; fan[k].pw = fan[k].w; }
            fan[k].pad = 0;
            if ((info & 3) == NMODE_GOURAUD) {
                fan[k].u = shade_mode == 1 ? (float)v[k].a / 128.0f : (float)v[k].a / 256.0f;
                fan[k].v = 0;
            } else if ((info & 3) == NMODE_TEXTURED) {
                float a = (float)v[k].a * uv_scale, b = (float)v[k].b * uv_scale;
                fan[k].v = (uv_mode & 1) ? b : a;     /* row */
                fan[k].u = (uv_mode & 1) ? a : b;     /* column */
            } else fan[k].u = fan[k].v = 0;
        }
        for (int k = 1; k + 1 < cnt; k++) { vbuf[n++] = fan[0]; vbuf[n++] = fan[k]; vbuf[n++] = fan[k + 1]; }
        if (native_probe && probe_n < 40) {
            /* debug (S): polygons reaching the top of the 3D view */
            /* clip at Z = 16 in 3D as the GPU does, project, and test whether
             * the polygon covers sky points (160,10) or (20,20) */
            float poly[2][70][3]; int pn = 0, pc = 0;
            for (int k = 0; k < cnt; k++) {
                poly[0][pn][0] = (float)v[k].x / 65536.0f; poly[0][pn][1] = (float)v[k].y / 65536.0f;
                poly[0][pn][2] = (float)v[k].z / 65536.0f; pn++;
            }
            int m = 0;
            for (int k = 0; k < pn; k++) {
                const float *P = poly[0][k], *Q = poly[0][(k + 1) % pn];
                int pin = P[2] >= NEAR_Z, qin = Q[2] >= NEAR_Z;
                if (pin && m < 68) { memcpy(poly[1][m], P, sizeof poly[1][m]); m++; }
                if (pin != qin && m < 68) {
                    float t = (NEAR_Z - P[2]) / (Q[2] - P[2]);
                    for (int c = 0; c < 3; c++) poly[1][m][c] = P[c] + t * (Q[c] - P[c]);
                    m++;
                }
            }
            pc = m;
            int hit = 0;
            static const float tp[2][2] = {{160, 10}, {-30, 188}};   /* probe points (game pixels; the 2nd in the wide side) */
            for (int t = 0; t < 2 && pc >= 3; t++) {
                int inside = 0;
                for (int k = 0, j = pc - 1; k < pc; j = k++) {
                    float xk = (float)p->cx + 256.0f * poly[1][k][0] / poly[1][k][2], yk = (float)p->cy + 256.0f * poly[1][k][1] / poly[1][k][2];
                    float xj = (float)p->cx + 256.0f * poly[1][j][0] / poly[1][j][2], yj = (float)p->cy + 256.0f * poly[1][j][1] / poly[1][j][2];
                    if (((yk > tp[t][1]) != (yj > tp[t][1])) && (tp[t][0] < (xj - xk) * (tp[t][1] - yk) / (yj - yk) + xk)) inside = !inside;
                }
                if (inside) hit = 1;
            }
            if (hit) {
                char b[400]; int o = 0;
                for (int k = 0; k < cnt && k < 6; k++)
                    o += snprintf(b + o, sizeof b - (size_t)o, " [%d %d %d|%d,%d%s]", v[k].x >> 16, v[k].y >> 16, v[k].z >> 16,
                                  v[k].sx, v[k].sy, v[k].near ? " N" : "");
                rt_log("probe: #%d kind %d colour %d n %d cull %d key %08x:%s", i, p->kind, p->color, p->count, p->cull, p->key, b);
                probe_n++;
            }
        }
        st->drawn++;
        if ((info & 3) == NMODE_GOURAUD) st->gouraud++;
        else if ((info & 3) == NMODE_TEXTURED) st->textured++;
    }
    /* learn the orientation sign for polygons that cross the near plane */
    if (agree + disagree > 16) orient_sign = agree >= disagree ? 1 : -1;
    st->orient_agree = agree; st->orient_disagree = disagree;
    st->mv_mean = mv_n ? (float)(mv_sum / mv_n) : 0;
    if (native_probe) { native_probe = 0; probe_n = 0; }
    st->triangles = n / 3;
    *out = vbuf;
    return n;
}
