/* Upscaled art pack (tfx.ini ui_filter=remacri; RENDERER.md "Art pack").
 * Static cockpit art, recognised by the hash of its game pixels, is drawn
 * from higher-resolution images instead (the rest of the game's pixels go
 * through MMPX).  Pack: <exe dir>\art\<name>\pack.txt, one line per asset:
 *   template|sprite <hash, hex> <file> <game width> <game height>
 * files: "TFXA", u32 width, u32 height, then BGRA rows (straight alpha). */
#pragma once
#include <stdint.h>

enum { ART_TEMPLATE = 1, ART_SPRITE = 2 };
#define ART_MAX 250

int art_active(void);                       /* ui_filter=remacri and a pack loaded */
int art_find(int type, uint32_t hash);      /* asset index, or -1 */
int art_count(void);
int art_info(int i, int *type, int *gw, int *gh, uint32_t *hash);
/* hash of an image of palette indices (0x100 = transparent) */
uint32_t art_hash(const uint16_t *idx, int w, int h);
/* the atlas (BGRA) and its version (changes when reloaded) */
const uint8_t *art_atlas(int *w, int *h, int *version);
/* 8 floats per asset: atlas x, y (texels), texels per game pixel x, y,
 * game width, height, 0, 0 */
const float *art_table(int *n);
