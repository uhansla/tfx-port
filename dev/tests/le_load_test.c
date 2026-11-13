/* Check runtime/le_load.c against the image written by tools/recomp.py:
 *   le_load_test EXE image.bin
 * Loads EXE with the C loader and compares every object byte for byte. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "rt.h"

int le_load(const char *path, uint64_t expect_hash, uint32_t *entry, uint32_t *stack,
            void (*placed)(int, uint32_t, uint32_t, uint32_t), char *err, int errlen);
uint8_t *MEM;
static uint32_t pb[16][3]; static int np;
static void placed(int i, uint32_t base, uint32_t size, uint32_t flags) { pb[i][0] = base; pb[i][1] = size; pb[i][2] = flags; np = i + 1; }

int main(int argc, char **argv) {
    MEM = VirtualAlloc(NULL, MEM_SIZE + 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    uint32_t entry, stack; char err[256];
    if (le_load(argv[1], 0, &entry, &stack, placed, err, sizeof err)) { printf("load failed: %s\n", err); return 1; }
    FILE *f = fopen(argv[2], "rb");
    char magic[4]; uint32_t hdr[3], objs[16][3];
    fread(magic, 1, 4, f); fread(hdr, 4, 3, f); fread(objs, 12, hdr[2], f);
    int bad = 0;
    if (hdr[0] != entry || hdr[1] != stack) { printf("entry/stack %08x/%08x vs %08x/%08x\n", entry, stack, hdr[0], hdr[1]); bad++; }
    if ((int)hdr[2] != np) { printf("objects %d vs %u\n", np, hdr[2]); bad++; }
    for (uint32_t i = 0; i < hdr[2] && i < (uint32_t)np; i++) {
        if (objs[i][0] != pb[i][0] || objs[i][1] != pb[i][1] || objs[i][2] != pb[i][2]) {
            printf("obj %u: %08x %08x %04x vs %08x %08x %04x\n", i + 1, pb[i][0], pb[i][1], pb[i][2], objs[i][0], objs[i][1], objs[i][2]); bad++;
        }
        uint8_t *buf = malloc(objs[i][1]);
        fread(buf, 1, objs[i][1], f);
        uint32_t diffs = 0, first = 0;
        for (uint32_t k = 0; k < objs[i][1]; k++) if (buf[k] != MEM[objs[i][0] + k]) { if (!diffs++) first = k; }
        printf("obj %u at %08x size %08x: %u byte differences%s", i + 1, objs[i][0], objs[i][1], diffs, diffs ? "" : "\n");
        if (diffs) { printf(" (first at +%x)\n", first); bad++; }
        free(buf);
    }
    printf("%s\n", bad ? "MISMATCH" : "identical");
    return bad != 0;
}
