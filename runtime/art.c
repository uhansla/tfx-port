/* Upscaled art pack: loading and atlas (see art.h, RENDERER.md "Art pack") */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "settings.h"
#include "art.h"

#define ATLAS_W 2048
#define ATLAS_MAX_H 4096
#define PAD 2

static struct { int type, gw, gh; uint32_t hash; } as[ART_MAX];
static float table[ART_MAX * 8];
static int nas, tried, version;
static uint8_t *atlas;
static int atlas_h;

uint32_t art_hash(const uint16_t *idx, int w, int h) {
    uint32_t x = 2166136261u;
    x = (x ^ (uint32_t)w) * 16777619u;
    x = (x ^ (uint32_t)h) * 16777619u;
    for (int i = 0; i < w * h; i++) {
        x = (x ^ (idx[i] & 0xff)) * 16777619u;
        x = (x ^ (idx[i] >> 8)) * 16777619u;
    }
    return x;
}

static uint8_t *read_raw(const char *path, int *w, int *h) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char mg[4]; uint32_t wh[2];
    uint8_t *p = NULL;
    if (fread(mg, 1, 4, f) == 4 && !memcmp(mg, "TFXA", 4) && fread(wh, 4, 2, f) == 2 && wh[0] && wh[1] &&
        wh[0] <= ATLAS_W - 2 * PAD && wh[1] <= 2048) {
        size_t n = (size_t)wh[0] * wh[1] * 4;
        p = malloc(n);
        if (p && fread(p, 1, n, f) != n) { free(p); p = NULL; }
        *w = (int)wh[0]; *h = (int)wh[1];
    }
    fclose(f);
    return p;
}

static void load(void) {
    char dir[MAX_PATH], path[MAX_PATH + 64];
    GetModuleFileNameA(NULL, dir, sizeof dir);
    char *s = strrchr(dir, '\\');
    if (s) *s = 0;
    snprintf(path, sizeof path, "%s\\art\\remacri\\pack.txt", dir);
    FILE *f = fopen(path, "r");
    if (!f) { rt_log("art: no pack at %s (ui_filter=remacri works as mmpx)", path); return; }
    atlas = calloc((size_t)ATLAS_W * ATLAS_MAX_H, 4);
    if (!atlas) { fclose(f); return; }
    int sx = 0, sy = 0, sh = 0;
    char line[512];
    while (fgets(line, sizeof line, f) && nas < ART_MAX) {
        char kind[16], file[256]; unsigned hash; int gw, gh;
        if (line[0] == '#' || sscanf(line, "%15s %x %255s %d %d", kind, &hash, file, &gw, &gh) != 5) continue;
        int w, h;
        snprintf(path, sizeof path, "%s\\art\\remacri\\%s", dir, file);
        uint8_t *px = read_raw(path, &w, &h);
        if (!px) { rt_log("art: cannot read %s", path); continue; }
        if (sx + w + 2 * PAD > ATLAS_W) { sx = 0; sy += sh; sh = 0; }
        if (sy + h + 2 * PAD > ATLAS_MAX_H) { free(px); rt_log("art: atlas full"); break; }
        /* copy with PAD texels of replicated edge around it (bilinear sampling at the asset's border) */
        for (int y = -PAD; y < h + PAD; y++)
            for (int x = -PAD; x < w + PAD; x++) {
                int cx = x < 0 ? 0 : x >= w ? w - 1 : x, cy = y < 0 ? 0 : y >= h ? h - 1 : y;
                memcpy(atlas + ((size_t)(sy + PAD + y) * ATLAS_W + (sx + PAD + x)) * 4, px + ((size_t)cy * w + cx) * 4, 4);
            }
        free(px);
        as[nas].type = !strcmp(kind, "template") ? ART_TEMPLATE : ART_SPRITE;
        as[nas].hash = hash; as[nas].gw = gw; as[nas].gh = gh;
        float *t = table + nas * 8;
        t[0] = (float)(sx + PAD); t[1] = (float)(sy + PAD);
        t[2] = (float)w / (float)gw; t[3] = (float)h / (float)gh;
        t[4] = (float)gw; t[5] = (float)gh; t[6] = t[7] = 0;
        nas++;
        sx += w + 2 * PAD;
        if (h + 2 * PAD > sh) sh = h + 2 * PAD;
    }
    fclose(f);
    atlas_h = sy + sh;
    if (atlas_h < 1) atlas_h = 1;
    version++;
    rt_log("art: %d asset(s) from art\\remacri, atlas %dx%d", nas, ATLAS_W, atlas_h);
}

int art_active(void) {
    if (settings.ui_filter != 3) return 0;
    if (!tried) { tried = 1; load(); }
    return nas > 0;
}

int art_find(int type, uint32_t hash) {
    for (int i = 0; i < nas; i++) if (as[i].type == type && as[i].hash == hash) return i;
    return -1;
}

int art_count(void) { return nas; }

int art_info(int i, int *type, int *gw, int *gh, uint32_t *hash) {
    if (i < 0 || i >= nas) return 0;
    *type = as[i].type; *gw = as[i].gw; *gh = as[i].gh; *hash = as[i].hash;
    return 1;
}

const uint8_t *art_atlas(int *w, int *h, int *ver) { *w = ATLAS_W; *h = atlas_h; *ver = version; return atlas; }

const float *art_table(int *n) { *n = nas; return table; }
