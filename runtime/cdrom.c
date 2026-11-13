/* CD-ROM: MSCDEX (int 2Fh AH=15h) emulation backed by the original BIN/CUE
 * image, including Red Book audio playback through SDL. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <SDL.h>
#include "rt.h"

#define RAW 2352
#define MAX_TRACKS 99

struct track { int num, audio; uint32_t start; };   /* start: absolute frame (LBA) in image */
static struct track tracks[MAX_TRACKS];
static int ntracks;
static FILE *bin;
static uint32_t total_frames;
static SDL_mutex *cd_lock;
static int cd_drive = 3;             /* D: */

/* audio playback state (frames = 1/75 s sectors) */
static volatile int playing, paused;
static uint32_t play_pos, play_end;  /* LBA */
static uint8_t play_buf[RAW];
static int play_buf_pos = RAW;
static int last_play_start;
static SDL_AudioDeviceID cd_audio_dev;

int cd_present(void) { return bin != NULL; }

static uint32_t msf_to_lba(uint8_t m, uint8_t s, uint8_t f) { return (m * 60u + s) * 75u + f; }

/* ---- CUE parsing ------------------------------------------------------------------ */
int cd_open(const char *cue_path) {
    FILE *f = fopen(cue_path, "r");
    if (!f) { rt_log("cdrom: cannot open %s", cue_path); return 0; }
    char line[512], binname[512] = "";
    int cur = -1;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (!strncmp(p, "FILE", 4)) {
            char *q = strchr(p, '"');
            if (q) { char *e = strchr(q + 1, '"'); if (e) { *e = 0; snprintf(binname, sizeof binname, "%s", q + 1); } }
        } else if (!strncmp(p, "TRACK", 5)) {
            int n; char type[32];
            if (sscanf(p + 5, "%d %31s", &n, type) == 2 && ntracks < MAX_TRACKS) {
                cur = ntracks++;
                tracks[cur].num = n;
                tracks[cur].audio = !strcmp(type, "AUDIO");
            }
        } else if (!strncmp(p, "INDEX 01", 8) && cur >= 0) {
            int m, s, fr;
            if (sscanf(p + 8, "%d:%d:%d", &m, &s, &fr) == 3) tracks[cur].start = msf_to_lba(m, s, fr);
        }
    }
    fclose(f);
    /* the BIN lives next to the CUE */
    char path[1024];
    snprintf(path, sizeof path, "%s", cue_path);
    char *slash = strrchr(path, '\\'); char *s2 = strrchr(path, '/');
    if (s2 > slash) slash = s2;
    if (slash) snprintf(slash + 1, sizeof path - (slash + 1 - path), "%s", binname);
    else snprintf(path, sizeof path, "%s", binname);
    bin = fopen(path, "rb");
    if (!bin) { rt_log("cdrom: cannot open image %s", path); return 0; }
    _fseeki64(bin, 0, SEEK_END);
    total_frames = (uint32_t)(_ftelli64(bin) / RAW);
    cd_lock = SDL_CreateMutex();
    rt_log("cdrom: %s, %d tracks, %u frames", path, ntracks, total_frames);
    return 1;
}

/* read one 2048-byte data sector (MODE1/2352) */
static int read_data_sector(uint32_t lba, uint8_t *out) {
    if (!bin || lba >= total_frames) return 0;
    SDL_LockMutex(cd_lock);
    _fseeki64(bin, (long long)lba * RAW + 16, SEEK_SET);
    int ok = fread(out, 1, 2048, bin) == 2048;
    SDL_UnlockMutex(cd_lock);
    return ok;
}

/* ---- ISO 9660: the data track's files, read straight from the image --------
 * (so the game runs from the original BIN/CUE without an extracted copy) */
static int iso_dir_find(uint32_t lba, uint32_t size, const char *name, int nlen,
                        uint32_t *out_lba, uint32_t *out_size, int *is_dir) {
    uint8_t sec[2048];
    for (uint32_t off = 0; off < size; off += 2048) {
        if (!read_data_sector(lba + off / 2048, sec)) return 0;
        for (int i = 0; i < 2048;) {
            int len = sec[i];
            if (!len) break;                       /* rest of the sector is padding */
            int fl = sec[i + 25], nl = sec[i + 32];
            const char *fn = (const char *)sec + i + 33;
            int cmp = nl;
            for (int k = 0; k < nl; k++) if (fn[k] == ';') { cmp = k; break; }
            if (cmp > 0 && fn[cmp - 1] == '.') cmp--;  /* "NAME." for names without extension */
            if (cmp == nlen && !_strnicmp(fn, name, (size_t)nlen)) {
                *out_lba = sec[i + 2] | sec[i + 3] << 8 | sec[i + 4] << 16 | (uint32_t)sec[i + 5] << 24;
                *out_size = sec[i + 10] | sec[i + 11] << 8 | sec[i + 12] << 16 | (uint32_t)sec[i + 13] << 24;
                *is_dir = (fl & 2) != 0;
                return 1;
            }
            i += len;
        }
    }
    return 0;
}

/* path relative to the CD root ("did.dat", "\\TUNES\\X.MID"); 1 = found file */
int iso_lookup(const char *path, uint32_t *lba, uint32_t *size) {
    uint8_t pvd[2048];
    if (!bin || !read_data_sector(16, pvd) || pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5)) return 0;
    const uint8_t *root = pvd + 156;
    uint32_t cur = root[2] | root[3] << 8 | root[4] << 16 | (uint32_t)root[5] << 24;
    uint32_t csz = root[10] | root[11] << 8 | root[12] << 16 | (uint32_t)root[13] << 24;
    int is_dir = 1;
    const char *p = path;
    while (*p) {
        while (*p == '\\' || *p == '/') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != '\\' && *e != '/') e++;
        if (!is_dir || !iso_dir_find(cur, csz, p, (int)(e - p), &cur, &csz, &is_dir)) return 0;
        p = e;
    }
    if (is_dir) return 0;
    *lba = cur; *size = csz;
    return 1;
}

/* read n bytes at pos of a file stored at lba */
int iso_read(uint32_t lba, uint32_t pos, uint8_t *dst, uint32_t n) {
    uint8_t sec[2048];
    uint32_t done = 0;
    while (done < n) {
        uint32_t s = (pos + done) / 2048, o = (pos + done) % 2048, k = 2048 - o;
        if (k > n - done) k = n - done;
        if (o == 0 && k == 2048) {
            if (!read_data_sector(lba + s, dst + done)) break;
        } else {
            if (!read_data_sector(lba + s, sec)) break;
            memcpy(dst + done, sec + o, k);
        }
        done += k;
    }
    return (int)done;
}

/* ---- Red Book audio ----------------------------------------------------------------- */
static void SDLCALL cd_audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    memset(stream, 0, len);
    if (!playing || paused) return;
    int o = 0;
    while (o < len) {
        if (play_buf_pos >= RAW) {
            if (play_pos >= play_end) { playing = 0; break; }
            SDL_LockMutex(cd_lock);
            _fseeki64(bin, (long long)play_pos * RAW, SEEK_SET);
            size_t n = fread(play_buf, 1, RAW, bin);
            SDL_UnlockMutex(cd_lock);
            if (n != RAW) { playing = 0; break; }
            play_pos++;
            play_buf_pos = 0;
        }
        int n = RAW - play_buf_pos;
        if (n > len - o) n = len - o;
        memcpy(stream + o, play_buf + play_buf_pos, n);
        play_buf_pos += n; o += n;
    }
}

static void audio_init(void) {
    if (cd_audio_dev) return;
    SDL_AudioSpec want = {0}, have;
    want.freq = 44100; want.format = AUDIO_S16LSB; want.channels = 2; want.samples = 2048;
    want.callback = cd_audio_cb;
    cd_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!cd_audio_dev) { rt_log("cdrom: cannot open audio: %s", SDL_GetError()); return; }
    SDL_PauseAudioDevice(cd_audio_dev, 0);
}

static void play(uint32_t start, uint32_t count) {
    audio_init();
    SDL_LockAudioDevice(cd_audio_dev);
    play_pos = start; play_end = start + count;
    if (play_end > total_frames) play_end = total_frames;
    play_buf_pos = RAW; paused = 0; playing = 1;
    last_play_start = (int)start;
    SDL_UnlockAudioDevice(cd_audio_dev);
    int t = -1;
    for (int i = 0; i < ntracks; i++) if (tracks[i].start <= start) t = tracks[i].num;
    rt_log("cdrom: play %u frames from %u (track %d)", count, start, t);
}

/* ---- MSCDEX ------------------------------------------------------------------------- */
#define RM(seg, off) (((uint32_t)(seg) << 4) + (uint16_t)(off))

static uint32_t to_msf(uint32_t lba) {     /* red book address as stored: frame, sec, min, 0 */
    lba += 150;
    uint32_t m = lba / (75 * 60), s = (lba / 75) % 60, f = lba % 75;
    return f | (s << 8) | (m << 16);
}

static uint32_t addr_to_lba(uint8_t mode, uint32_t a) {
    if (mode == 1) {                         /* red book: frame, second, minute */
        uint32_t f = a & 0xff, s = (a >> 8) & 0xff, m = (a >> 16) & 0xff;
        return msf_to_lba(m, s, f) - 150;
    }
    return a;                                /* HSG = LBA */
}

static void ioctl_input(uint32_t buf) {
    uint8_t code = RB(buf);
    switch (code) {
    case 0: WD(buf + 1, 0); break;                          /* device header address */
    case 1: {                                               /* location of head */
        uint8_t mode = RB(buf + 1);
        uint32_t lba = playing ? play_pos : 0;
        WD(buf + 2, mode ? to_msf(lba) : lba);
        break;
    }
    case 4:                                                 /* audio channel info */
        for (int i = 0; i < 4; i++) { WB(buf + 1 + i * 2, (uint8_t)i); WB(buf + 2 + i * 2, 0xff); }
        break;
    case 6: WD(buf + 1, 0x00000390u | (playing ? 0 : 0)); break;  /* status: door closed, raw+audio */
    case 7: WB(buf + 1, 0); WW(buf + 2, 2048); break;       /* sector size */
    case 8: WD(buf + 1, total_frames); break;               /* volume size */
    case 9: WB(buf + 1, 1); break;                          /* media not changed */
    case 10: {                                              /* audio disk info */
        WB(buf + 1, (uint8_t)tracks[0].num);
        WB(buf + 2, (uint8_t)tracks[ntracks - 1].num);
        WD(buf + 3, to_msf(total_frames));
        break;
    }
    case 11: {                                              /* audio track info */
        int n = RB(buf + 1);
        for (int i = 0; i < ntracks; i++) {
            if (tracks[i].num == n) {
                WD(buf + 2, to_msf(tracks[i].start));
                WB(buf + 6, tracks[i].audio ? 0x00 : 0x40); /* control: 0x40 = data track */
                return;
            }
        }
        WD(buf + 2, 0); WB(buf + 6, 0);
        break;
    }
    case 12: {                                              /* Q channel info */
        uint32_t lba = playing ? play_pos : 0;
        int t = 1;
        for (int i = 0; i < ntracks; i++) if (tracks[i].start <= lba) t = tracks[i].num;
        uint32_t rel = lba - tracks[t - 1].start;
        uint32_t ra = to_msf(rel - 150 + 150), aa = to_msf(lba);
        WB(buf + 1, 0x01); WB(buf + 2, (uint8_t)t); WB(buf + 3, 1);
        WB(buf + 4, (uint8_t)(ra >> 16)); WB(buf + 5, (uint8_t)(ra >> 8)); WB(buf + 6, (uint8_t)ra);
        WB(buf + 7, 0);
        WB(buf + 8, (uint8_t)(aa >> 16)); WB(buf + 9, (uint8_t)(aa >> 8)); WB(buf + 10, (uint8_t)aa);
        break;
    }
    case 15: {                                              /* audio status */
        WW(buf + 1, paused ? 1 : 0);
        WD(buf + 3, to_msf((uint32_t)last_play_start));
        WD(buf + 7, to_msf(play_end));
        break;
    }
    default:
        rt_log("cdrom: IOCTL input code %d", code);
    }
}

/* device driver request at linear address req */
static void device_request(uint32_t req) {
    uint8_t cmd = RB(req + 2);
    if (getenv("TFX_TRACE_INT")) rt_log("cdrom: request cmd %d len %d (ioctl code %d) addr %08x cnt %d", cmd, RB(req), cmd == 3 ? RB(RM(RD(req + 14) >> 16, RD(req + 14) & 0xffff)) : -1, RD(req + 20), RW(req + 18));
    uint16_t status = 0x0100;                               /* done */
    switch (cmd) {
    case 3: {                                               /* IOCTL input */
        uint32_t ta = RD(req + 14);
        ioctl_input(RM(ta >> 16, ta & 0xffff));
        break;
    }
    case 12: break;                                         /* IOCTL output */
    case 13: case 14: break;                                /* open / close */
    case 128: {                                             /* READ LONG */
        uint8_t mode = RB(req + 13);
        uint32_t ta = RD(req + 14);
        uint32_t dst = RM(ta >> 16, ta & 0xffff);
        uint16_t count = RW(req + 18);
        uint32_t lba = addr_to_lba(mode, RD(req + 20));
        uint8_t sec[2048];
        for (uint32_t i = 0; i < count; i++) {
            if (!read_data_sector(lba + i, sec)) { status = 0x8108; break; }   /* sector not found */
            for (int k = 0; k < 2048; k++) WB(dst + i * 2048 + k, sec[k]);
        }
        break;
    }
    case 131: break;                                        /* SEEK */
    case 132: {                                             /* PLAY AUDIO */
        uint8_t mode = RB(req + 13);
        uint32_t start = addr_to_lba(mode, RD(req + 14));
        play(start, RD(req + 18));
        break;
    }
    case 133:                                               /* STOP AUDIO */
        if (playing && !paused) paused = 1; else { playing = 0; paused = 0; }
        break;
    case 136: paused = 0; break;                            /* RESUME */
    default:
        rt_log("cdrom: device request command %d", cmd);
        status = 0x8103;                                    /* unknown command */
    }
    if (playing && !paused) status |= 0x0200;               /* busy = audio playing */
    WW(req + 3, status);
}

/* Real-mode int 2Fh AH=15h.  regs = DPMI real-mode call structure.
 * Returns 1 if handled. */
int mscdex_int2f(uint32_t regs) {
    uint16_t ax = RW(regs + 0x1c);
    if ((ax >> 8) != 0x15) return 0;
    if (!bin) {                                             /* no CD: leave BX = 0 */
        if (ax == 0x1500) WW(regs + 0x10, 0);
        return 1;
    }
    uint16_t es = RW(regs + 0x22);
    uint16_t bx = RW(regs + 0x10);
    switch (ax) {
    case 0x1500: WW(regs + 0x10, 1); WW(regs + 0x18, (uint16_t)cd_drive); break;
    case 0x150b: WW(regs + 0x1c, RW(regs + 0x18) == cd_drive ? 0x5ad8 : 0); WW(regs + 0x10, 0xadad); break;
    case 0x150c: WW(regs + 0x10, 0x0215); break;
    case 0x150d: WB(RM(es, bx), (uint8_t)cd_drive); break;
    case 0x1510: device_request(RM(es, bx)); break;
    default: rt_log("cdrom: MSCDEX function %04x", ax);
    }
    return 1;
}
