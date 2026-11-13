/* DOS (int 21h) emulation with DOS/4GW 32-bit pointer semantics. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <direct.h>
#include <windows.h>
#include "rt.h"

#define SET_AX(v) (eax = (eax & 0xffff0000u) | (uint16_t)(v))
#define SET_AL(v) (eax = (eax & 0xffffff00u) | (uint8_t)(v))
#define AX ((uint16_t)eax)
#define AH ((uint8_t)(eax >> 8))
#define AL ((uint8_t)eax)
#define BX ((uint16_t)ebx)
#define CX ((uint16_t)ecx)
#define DX ((uint16_t)edx)

char game_dir[512];
uint64_t dos_last_open_us;           /* for test scripts: last file open */
const char *extra_env[32];          /* NAME=VALUE pairs added with --env */
int n_extra_env;
char cd_dir[512];
static int cur_drive = 2;           /* C: */
static uint32_t dta_addr;
static int last_exit_code;

static void set_cf(int c) { rt_set_cf(c ? 1 : 0); }
static void dos_error(int code) { SET_AX(code); set_cf(1); }

static void read_str(uint32_t a, char *buf, int n) {
    int i = 0;
    for (; i < n - 1; i++) { char c = (char)RB(a + i); buf[i] = c; if (!c) break; }
    buf[i] = 0;
    /* DOS ignores trailing spaces in file names (START.EXE's tune table has
     * "tunes\intro.SCC ") */
    while (i > 0 && buf[i - 1] == ' ') buf[--i] = 0;
}

/* ---- path mapping ------------------------------------------------------------
 * C:\ (and relative paths) -> game_dir;  D:\ (the CD drive) -> cd_dir.
 * Lookups are case-insensitive on Windows already.
 */
int dos_map_path(const char *dos, char *host, int hostlen) {
    char tmp[512];
    const char *p = dos;
    const char *root = game_dir;
    if (p[0] && p[1] == ':') {
        char d = (char)toupper((unsigned char)p[0]);
        root = (d == 'D' && cd_dir[0]) ? cd_dir : game_dir;
        p += 2;
    }
    while (*p == '\\' || *p == '/') p++;
    snprintf(tmp, sizeof tmp, "%s\\%s", root, p);
    for (char *q = tmp; *q; q++) if (*q == '/') *q = '\\';
    snprintf(host, hostlen, "%s", tmp);
    return 1;
}

/* ---- file handles ---------------------------------------------------------------- */
#define MAXH 64
static int hfd[MAXH];

static void handles_init(void) {
    for (int i = 0; i < MAXH; i++) hfd[i] = -1;
    hfd[0] = 0; hfd[1] = 1; hfd[2] = 2; hfd[3] = -2; hfd[4] = -2;
}

static int h_new(int fd) {
    for (int i = 5; i < MAXH; i++) if (hfd[i] == -1) { hfd[i] = fd; return i; }
    return -1;
}

/* files opened from the CD image (no extracted copy): hfd = -100 - slot */
int iso_lookup(const char *path, uint32_t *lba, uint32_t *size);
int iso_read(uint32_t lba, uint32_t pos, uint8_t *dst, uint32_t n);
static struct { uint32_t lba, size, pos; int used; } isof[MAXH];
#define IS_ISO(h) (hfd[h] <= -100)
#define ISO_SLOT(h) (-100 - hfd[h])

static int iso_open(const char *dos_path) {
    const char *p = dos_path;
    if (p[0] && p[1] == ':') p += 2;
    uint32_t lba, size;
    if (!iso_lookup(p, &lba, &size)) return -1;
    for (int s = 0; s < MAXH; s++) if (!isof[s].used) {
        isof[s].used = 1; isof[s].lba = lba; isof[s].size = size; isof[s].pos = 0;
        return h_new(-100 - s);
    }
    return -1;
}

/* ---- conventional memory allocator (paragraph granularity) ----------------- */
struct dblk { uint16_t seg, paras; uint8_t used; };
static struct dblk dblks[256];
static int ndblk;

static void dos_mem_init(void) {
    ndblk = 1;
    dblks[0].seg = DOS_POOL_LO; dblks[0].paras = DOS_POOL_HI - DOS_POOL_LO; dblks[0].used = 0;
}

int dos_alloc(uint16_t paras, uint16_t *seg, uint16_t *maxavail) {
    uint16_t best = 0;
    for (int i = 0; i < ndblk; i++) {
        if (dblks[i].used) continue;
        if (dblks[i].paras >= paras) {
            if (dblks[i].paras > paras && ndblk < 256) {
                memmove(&dblks[i + 2], &dblks[i + 1], (ndblk - i - 1) * sizeof(dblks[0]));
                dblks[i + 1].seg = dblks[i].seg + paras;
                dblks[i + 1].paras = dblks[i].paras - paras;
                dblks[i + 1].used = 0;
                dblks[i].paras = paras;
                ndblk++;
            }
            dblks[i].used = 1;
            *seg = dblks[i].seg;
            return 1;
        }
        if (dblks[i].paras > best) best = dblks[i].paras;
    }
    *maxavail = best;
    return 0;
}

void dos_free(uint16_t seg) {
    for (int i = 0; i < ndblk; i++) if (dblks[i].seg == seg) dblks[i].used = 0;
}

/* ---- extended memory heap (first fit, coalescing) ------------------------------ */
struct hblk { uint32_t addr, size; int used; };
static struct hblk *hb;
static int nhb, cap_hb;

static void heap_init(void) {
    cap_hb = 1024;
    hb = calloc(cap_hb, sizeof *hb);
    nhb = 1;
    hb[0].addr = HEAP_LO; hb[0].size = MEM_SIZE - HEAP_LO; hb[0].used = 0;
}

uint32_t heap_alloc(uint32_t size) {
    size = (size + 15) & ~15u;
    if (!size) size = 16;
    for (int i = 0; i < nhb; i++) {
        if (hb[i].used || hb[i].size < size) continue;
        if (hb[i].size > size) {
            if (nhb == cap_hb) { cap_hb *= 2; hb = realloc(hb, cap_hb * sizeof *hb); }
            memmove(&hb[i + 2], &hb[i + 1], (nhb - i - 1) * sizeof *hb);
            hb[i + 1].addr = hb[i].addr + size;
            hb[i + 1].size = hb[i].size - size;
            hb[i + 1].used = 0;
            hb[i].size = size;
            nhb++;
        }
        hb[i].used = 1;
        memset(MEM + hb[i].addr, 0, size);
        return hb[i].addr;
    }
    return 0;
}

void heap_free(uint32_t addr) {
    for (int i = 0; i < nhb; i++) {
        if (hb[i].addr == addr && hb[i].used) {
            hb[i].used = 0;
            if (i + 1 < nhb && !hb[i + 1].used) {
                hb[i].size += hb[i + 1].size;
                memmove(&hb[i + 1], &hb[i + 2], (nhb - i - 2) * sizeof *hb); nhb--;
            }
            if (i > 0 && !hb[i - 1].used) {
                hb[i - 1].size += hb[i].size;
                memmove(&hb[i], &hb[i + 1], (nhb - i - 1) * sizeof *hb); nhb--;
            }
            return;
        }
    }
}

uint32_t heap_resize(uint32_t addr, uint32_t size) {
    for (int i = 0; i < nhb; i++) {
        if (hb[i].addr == addr && hb[i].used) {
            uint32_t old = hb[i].size;
            if (((size + 15) & ~15u) <= old) return addr;
            uint32_t n = heap_alloc(size);
            if (!n) return 0;
            memmove(MEM + n, MEM + addr, old);
            heap_free(addr);
            return n;
        }
    }
    return 0;
}

/* ---- PSP / environment ------------------------------------------------------------ */
void dos_init(int argc, char **argv) {
    handles_init();
    dos_mem_init();
    heap_init();
    uint32_t psp = PSP_SEG << 4, env = ENV_SEG << 4;
    memset(MEM + psp, 0, 256);
    WW(psp + 0, 0x20cd);
    WW(psp + 2, DOS_POOL_HI);
    WW(psp + 0x2c, SEL_ENV);
    /* command tail */
    char tail[128] = "";
    for (int i = 1; i < argc; i++) {
        strncat(tail, " ", sizeof tail - strlen(tail) - 1);
        strncat(tail, argv[i], sizeof tail - strlen(tail) - 1);
    }
    int n = (int)strlen(tail);
    if (n > 126) n = 126;
    WB(psp + 0x80, (uint8_t)n);
    memcpy(MEM + psp + 0x81, tail, n);
    WB(psp + 0x81 + n, 0x0d);
    /* environment: strings, double NUL, count, program path */
    const char *envs[] = {"COMSPEC=C:\\COMMAND.COM", "PATH=C:\\", "DOS4G=QUIET", NULL};
    uint32_t p = env;
    for (int i = 0; envs[i]; i++) { size_t l = strlen(envs[i]) + 1; memcpy(MEM + p, envs[i], l); p += l; }
    for (int i = 0; i < n_extra_env; i++) {
        size_t l = strlen(extra_env[i]) + 1;
        if (p + l > env + 0x7f0) break;
        memcpy(MEM + p, extra_env[i], l); p += l;
    }
    WB(p++, 0);
    WW(p, 1); p += 2;
    const char *prog = "C:\\TFX\\PROGRAM.EXE";
    memcpy(MEM + p, prog, strlen(prog) + 1);
    dta_addr = psp + 0x80;
}

/* ---- find first/next ------------------------------------------------------------- */
static HANDLE find_h = INVALID_HANDLE_VALUE;
static char find_pat[512];
static int find_attr;

static int fill_dta(WIN32_FIND_DATAA *fd) {
    uint32_t d = dta_addr;
    int attr = 0;
    if (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) attr |= 0x10;
    if (fd->dwFileAttributes & FILE_ATTRIBUTE_READONLY) attr |= 0x01;
    if ((attr & 0x10) && !(find_attr & 0x10)) return 0;
    const char *name = fd->cAlternateFileName[0] ? fd->cAlternateFileName : fd->cFileName;
    if (strlen(fd->cFileName) <= 12) name = fd->cFileName;
    WB(d + 0x15, (uint8_t)attr);
    FILETIME lt; WORD dd, tt;
    FileTimeToLocalFileTime(&fd->ftLastWriteTime, &lt);
    FileTimeToDosDateTime(&lt, &dd, &tt);
    WW(d + 0x16, tt); WW(d + 0x18, dd);
    WD(d + 0x1a, fd->nFileSizeLow);
    char up[13]; int i;
    for (i = 0; i < 12 && name[i]; i++) up[i] = (char)toupper((unsigned char)name[i]);
    up[i] = 0;
    for (i = 0; i < 13; i++) WB(d + 0x1e + i, i < (int)strlen(up) ? up[i] : 0);
    return 1;
}

static int find_next_match(void) {
    WIN32_FIND_DATAA fd;
    while (FindNextFileA(find_h, &fd)) if (fill_dta(&fd)) return 1;
    FindClose(find_h); find_h = INVALID_HANDLE_VALUE;
    return 0;
}

/* ---- int 21h ---------------------------------------------------------------------- */
void dos_int21(void) {
    char path[512], host[600];
    set_cf(0);
    switch (AH) {
    case 0x01: SET_AL(0x0d); break;
    case 0x02: putchar((char)edx); fflush(stdout); break;
    case 0x06: if ((uint8_t)edx != 0xff) putchar((char)edx); else { SET_AL(0); rt_set_arith_flags(0x40); } break;
    case 0x09: {
        uint32_t a = ds_base + edx;
        for (int i = 0; i < 4096; i++) { char c = (char)RB(a + i); if (c == '$') break; putchar(c); }
        fflush(stdout);
        break;
    }
    case 0x0b: SET_AL(0); break;     /* no key waiting */
    case 0x0e: cur_drive = (uint8_t)edx; SET_AL(26); break;
    case 0x19: SET_AL(cur_drive); break;
    case 0x1a: dta_addr = ds_base + edx; break;
    case 0x25: {                     /* set vector (DOS/4GW: PM vector DS:EDX) */
        pm_vectors[AL].sel = ds; pm_vectors[AL].off = edx;
        rt_log("int21 set vector %02x -> %04x:%08x", AL, ds, edx);
        break;
    }
    case 0x2a: {
        time_t t = time(NULL); struct tm *tm = localtime(&t);
        ecx = (ecx & 0xffff0000u) | (tm->tm_year + 1900);
        edx = (edx & 0xffff0000u) | ((tm->tm_mon + 1) << 8) | tm->tm_mday;
        SET_AL(tm->tm_wday);
        break;
    }
    case 0x2c: {
        SYSTEMTIME st; GetLocalTime(&st);
        ecx = (ecx & 0xffff0000u) | (st.wHour << 8) | st.wMinute;
        edx = (edx & 0xffff0000u) | (st.wSecond << 8) | (st.wMilliseconds / 10);
        break;
    }
    case 0x2f: rt_load_seg(0, SEL_FLAT_DS); ebx = dta_addr; break;
    case 0x30: SET_AX(0x0005); ebx &= 0xffff0000u; break;    /* DOS 5.0 */
    case 0x33: if (AL == 0) edx = (edx & 0xffffff00u); break;
    case 0x35:                       /* get vector -> ES:EBX */
        rt_load_seg(0, pm_vectors[AL].sel); ebx = pm_vectors[AL].off;
        break;
    case 0x36:                       /* free disk space */
        SET_AX(64); ebx = (ebx & 0xffff0000u) | 0x7fff; ecx = (ecx & 0xffff0000u) | 512;
        edx = (edx & 0xffff0000u) | 0xffff;
        break;
    case 0x37: if (AL == 0) edx = (edx & 0xffffff00u) | '/'; break;
    case 0x39: case 0x3a: {
        read_str(ds_base + edx, path, sizeof path); dos_map_path(path, host, sizeof host);
        if ((AH == 0x39 ? _mkdir(host) : _rmdir(host)) != 0) dos_error(5);
        break;
    }
    case 0x3b: break;                /* chdir: accept */
    case 0x3c: case 0x5b: {          /* create */
        read_str(ds_base + edx, path, sizeof path); dos_map_path(path, host, sizeof host);
        int fd = _open(host, _O_RDWR | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
        if (fd < 0) { dos_error(3); break; }
        int h = h_new(fd);
        rt_log("create %s -> %d", path, h);
        SET_AX(h);
        break;
    }
    case 0x3d: {                     /* open */
        dos_last_open_us = host_ticks_us();
        read_str(ds_base + edx, path, sizeof path); dos_map_path(path, host, sizeof host);
        int mode = AL & 3;
        int fl = (mode == 0 ? _O_RDONLY : mode == 1 ? _O_WRONLY : _O_RDWR) | _O_BINARY;
        int is_cd = toupper((unsigned char)path[0]) == 'D' && path[1] == ':';
        int fd = (is_cd && !cd_dir[0]) ? -1 : _open(host, fl);
        if (fd < 0 && mode == 0 && path[1] != ':' && cd_dir[0]) {
            /* START.EXE's DID=<drive>: data files missing from the game
             * directory are read from the CD (e.g. did.dat) */
            char cdp[600];
            snprintf(cdp, sizeof cdp, "D:%s%s", path[0] == '\\' ? "" : "\\", path);
            dos_map_path(cdp, host, sizeof host);
            fd = _open(host, fl);
        }
        if (fd < 0 && mode == 0 && !cd_dir[0] && (is_cd || path[1] != ':')) {
            /* no extracted CD copy: read from the CD image itself */
            int h = iso_open(path);
            rt_log("open %s (mode %d) -> %s", path, mode, h < 0 ? "fail" : "ok (cd image)");
            if (h < 0) dos_error(2); else SET_AX(h);
            break;
        }
        rt_log("open %s (mode %d) -> %s", path, mode, fd < 0 ? "fail" : "ok");
        if (fd < 0) { dos_error(2); break; }
        SET_AX(h_new(fd));
        break;
    }
    case 0x3e:
        if (BX > 4 && BX < MAXH && IS_ISO(BX)) { isof[ISO_SLOT(BX)].used = 0; hfd[BX] = -1; break; }
        if (BX < MAXH && hfd[BX] >= 0) { if (BX > 4) _close(hfd[BX]); if (BX > 4) hfd[BX] = -1; }
        else if (BX >= MAXH || hfd[BX] == -1) dos_error(6);
        break;
    case 0x3f: {                     /* read */
        int h = BX;
        if (h >= MAXH || hfd[h] == -1) { dos_error(6); break; }
        uint32_t a = ds_base + edx;
        if (h > 4 && IS_ISO(h)) {
            if (a + ecx > MEM_SIZE) { dos_error(5); break; }
            int s = ISO_SLOT(h);
            uint32_t want = ecx, left = isof[s].size > isof[s].pos ? isof[s].size - isof[s].pos : 0;
            if (want > left) want = left;
            uint8_t *tmp = malloc(want ? want : 1);
            int n = iso_read(isof[s].lba, isof[s].pos, tmp, want);
            for (int i = 0; i < n; i++) WB(a + i, tmp[i]);
            free(tmp);
            isof[s].pos += (uint32_t)n;
            eax = (uint32_t)n;
            break;
        }
        if (hfd[h] < 0 || h == 0) { eax = 0; break; }
        if (a + ecx > MEM_SIZE) { dos_error(5); break; }
        int n;
        if (vga_planar && a < 0xB0000 && a + ecx > 0xA0000) {
            uint8_t *tmp = malloc(ecx); n = _read(hfd[h], tmp, ecx);
            for (int i = 0; i < n; i++) WB(a + i, tmp[i]);
            free(tmp);
        } else n = _read(hfd[h], MEM + a, ecx);
        if (n < 0) { dos_error(5); break; }
        eax = (uint32_t)n;
        break;
    }
    case 0x40: {                     /* write */
        int h = BX;
        if (h >= MAXH || hfd[h] == -1) { dos_error(6); break; }
        uint32_t a = ds_base + edx;
        if (h == 1 || h == 2) { fwrite(MEM + a, 1, ecx, h == 1 ? stdout : stderr); fflush(stdout); eax = ecx; break; }
        if (hfd[h] < 0) { eax = ecx; break; }
        if (ecx == 0) { _chsize(hfd[h], _lseek(hfd[h], 0, SEEK_CUR)); eax = 0; break; }
        int n = _write(hfd[h], MEM + a, ecx);
        if (n < 0) { dos_error(5); break; }
        eax = (uint32_t)n;
        break;
    }
    case 0x41: {
        read_str(ds_base + edx, path, sizeof path); dos_map_path(path, host, sizeof host);
        if (_unlink(host) != 0) dos_error(2);
        break;
    }
    case 0x42: {                     /* seek */
        int h = BX;
        long off = (long)(((uint32_t)CX << 16) | DX);
        if (h > 4 && h < MAXH && IS_ISO(h)) {
            int s = ISO_SLOT(h);
            long base = AL == 0 ? 0 : AL == 1 ? (long)isof[s].pos : (long)isof[s].size;
            long r = base + off;
            if (r < 0) { dos_error(25); break; }
            isof[s].pos = (uint32_t)r;
            SET_AX(r & 0xffff);
            edx = (edx & 0xffff0000u) | ((uint32_t)r >> 16);
            break;
        }
        if (h >= MAXH || hfd[h] < 0) { dos_error(6); break; }
        long r = _lseek(hfd[h], off, AL);
        if (r < 0) { dos_error(25); break; }
        SET_AX(r & 0xffff);
        edx = (edx & 0xffff0000u) | ((uint32_t)r >> 16);
        eax = (eax & 0xffff) | 0;   /* keep high word clear */
        break;
    }
    case 0x43: {                     /* attributes */
        read_str(ds_base + edx, path, sizeof path); dos_map_path(path, host, sizeof host);
        DWORD at = GetFileAttributesA(host);
        if (at == INVALID_FILE_ATTRIBUTES) { dos_error(2); break; }
        if (AL == 0) ecx = (ecx & 0xffff0000u) | ((at & FILE_ATTRIBUTE_DIRECTORY) ? 0x10 : 0) |
                           ((at & FILE_ATTRIBUTE_READONLY) ? 1 : 0);
        break;
    }
    case 0x44: {                     /* IOCTL */
        if (AL == 0x00) {
            int h = BX;
            if (h >= MAXH || hfd[h] == -1) { dos_error(6); break; }
            uint16_t info = (h <= 4) ? 0x80d3 : 0x0002;
            if (h == 1 || h == 2) info = 0x80d3;
            edx = (edx & 0xffff0000u) | info;
        } else if (AL == 0x08) {
            SET_AX(BX == 0 || BX > 2 ? 1 : 0);
        } else if (AL == 0x09) {
            edx = (edx & 0xffff0000u) | ((BX == 4) ? 0x1000 : 0);
        } else {
            dos_error(1);
        }
        break;
    }
    case 0x47: {                     /* get cwd */
        WB(ds_base + esi, 0);
        const char *cwd = "TFX";
        for (int i = 0; i <= (int)strlen(cwd); i++) WB(ds_base + esi + i, cwd[i]);
        break;
    }
    case 0x48: {                     /* DOS/4GW: allocate memory, BX paragraphs -> AX selector */
        uint32_t size = (uint32_t)BX * 16;
        if (BX == 0xffff) { dos_error(8); ebx = (ebx & 0xffff0000u) | 0xffff; break; }
        uint32_t a = heap_alloc(size);
        if (!a) { dos_error(8); ebx &= 0xffff0000u; break; }
        SET_AX(sel_alloc(a, size - 1));
        break;
    }
    case 0x49: {                     /* free: ES = selector */
        uint32_t b = es_base;
        heap_free(b);
        sel_free(es);
        rt_load_seg(0, 0);
        break;
    }
    case 0x4a: {                     /* resize: in DOS/4GW for PSP, ignore */
        break;
    }
    case 0x4b: {                     /* EXEC */
        read_str(ds_base + edx, path, sizeof path);
        rt_log("EXEC %s (not supported)", path);
        dos_error(2);
        break;
    }
    case 0x4c:
        rt_log("program exit code %d", AL);
        if (getenv("TFX_DEBUG")) rt_dump_ftrace(600);
        { extern int rt_exit_status; rt_exit_status = AL; }
        exit(AL);
    case 0x4d: SET_AX(last_exit_code); break;
    case 0x4e: {                     /* find first */
        read_str(ds_base + edx, path, sizeof path); dos_map_path(path, host, sizeof host);
        if (find_h != INVALID_HANDLE_VALUE) FindClose(find_h);
        find_attr = CX;
        strcpy(find_pat, host);
        WIN32_FIND_DATAA fd;
        find_h = FindFirstFileA(host, &fd);
        if (find_h == INVALID_HANDLE_VALUE) { dos_error(18); break; }
        if (!fill_dta(&fd) && !find_next_match()) { dos_error(18); break; }
        break;
    }
    case 0x4f:
        if (find_h == INVALID_HANDLE_VALUE || !find_next_match()) dos_error(18);
        break;
    case 0x56: {
        char p2[512], h2[600];
        read_str(ds_base + edx, path, sizeof path); dos_map_path(path, host, sizeof host);
        read_str(es_base + edi, p2, sizeof p2); dos_map_path(p2, h2, sizeof h2);
        if (rename(host, h2) != 0) dos_error(5);
        break;
    }
    case 0x57: {
        if (AL == 0) { ecx &= 0xffff0000u; edx = (edx & 0xffff0000u) | 0x2121; }
        break;
    }
    case 0x62: ebx = (ebx & 0xffff0000u) | SEL_PSP; break;
    case 0xff:
        if (AL == 0x00 && DX == 0x0078) {
            /* DOS/4GW presence check: answer "yes" */
            SET_AL(1);
            rt_load_seg(5, 0);
        } else {
            SET_AL(0);
        }
        break;
    default:
        rt_dump_regs();
        rt_fatal("unhandled int 21h function AX=%04x", AX);
    }
}
