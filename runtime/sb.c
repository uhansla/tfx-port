/* Sound Blaster 16 emulation: DSP (base 220h), mixer, the two 8237 DMA
 * controllers and IRQ delivery, played through its own SDL audio device.
 *
 * Timing model: the SDL audio callback (its own thread) consumes guest DMA
 * memory at the DSP sample rate, resampled to 44.1 kHz stereo.  When a DSP
 * block finishes it sets sb_irq_req; the main thread raises the PC IRQ in
 * hw.c's rt_service().  Port handlers take the audio-device lock while they
 * change transfer state.
 *
 * Resources follow the BLASTER setting the game uses: TFX.CFG has its own
 * line (written by CONFIG.EXE, e.g. "A220 I7 D1 H5 T6 F3"); main.c puts the
 * same value in the environment. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "rt.h"

/* card resources: from BLASTER (TFX.CFG's line, else the environment) */
static uint16_t SB_BASE = 0x220;
static int SB_IRQ = 5, SB_DMA8 = 1, SB_DMA16 = 5;
#define OUT_RATE 44100

static int trace;
#define TR(...) do { if (trace) rt_log(__VA_ARGS__); } while (0)

/* ---- 8237 DMA ---------------------------------------------------------------- */
struct dmach {
    uint32_t base_addr, cur_addr;      /* in transfer units (bytes or words) */
    uint32_t base_count, cur_count;    /* count - 1, as programmed */
    uint8_t page, mode, masked;
};
static struct dmach dma[8];
static int dma_ff[2];                  /* flip-flops: 8-bit / 16-bit controller */
static const uint16_t page_port[8] = {0x87, 0x83, 0x81, 0x82, 0x8f, 0x8b, 0x89, 0x8a};

static uint32_t dma_phys(int ch) {     /* physical (= linear) address of the next unit */
    struct dmach *c = &dma[ch];
    if (ch < 4) return ((uint32_t)c->page << 16) | (c->cur_addr & 0xffff);
    return ((uint32_t)(c->page & 0xfe) << 16) | ((c->cur_addr & 0xffff) << 1);
}

/* advance one unit; returns 1 when the channel reached terminal count */
static int dma_step(int ch) {
    struct dmach *c = &dma[ch];
    if (c->mode & 0x20) c->cur_addr--; else c->cur_addr++;
    c->cur_addr &= 0xffff;
    if (c->cur_count-- == 0) {
        if (c->mode & 0x10) { c->cur_addr = c->base_addr; c->cur_count = c->base_count; }
        else { c->masked = 1; c->cur_count = 0xffff; }
        return 1;
    }
    return 0;
}

/* ---- DSP state ------------------------------------------------------------------- */
static SDL_AudioDeviceID dev;
static uint8_t outq[64]; static int outq_n;          /* bytes for the game to read */
static uint8_t cmd, args[4]; static int nargs, need;
static int reset_hi;
static uint32_t rate = 22050;                        /* DSP output rate */
static uint32_t blk8 = 0x7ff;                        /* block size - 1 (0x48 command) */
static int speaker = 0;
static uint8_t test_reg;
static uint8_t mixer_idx, mixer[256];

/* active transfer */
static volatile int xfer_on;                         /* DMA playback running */
static int x16, xstereo, xsigned, xauto, xpaused;
static uint32_t xblock, xleft;                       /* DSP block length (samples - 1) and remaining */
static double xpos;                                  /* fractional source position */
static int16_t last_l, last_r;
volatile int sb_irq_req;                             /* set by the audio thread */
static int irq8_pending, irq16_pending;

static void out_push(uint8_t v) { if (outq_n < (int)sizeof outq) outq[outq_n++] = v; }

static void stop_xfer(void) { xfer_on = 0; }

static void start_xfer(int is16, int autoinit, int stereo, int is_signed, uint32_t len_minus1) {
    x16 = is16; xauto = autoinit; xstereo = stereo; xsigned = is_signed;
    xblock = xleft = len_minus1;
    xpos = 0; xpaused = 0; xfer_on = 1;
    TR("sb: start %d-bit %s %s %s, %u samples per block at %u Hz", is16 ? 16 : 8,
       autoinit ? "auto-init" : "single", stereo ? "stereo" : "mono", is_signed ? "signed" : "unsigned",
       len_minus1 + 1, rate);
}

/* read one sample unit (8-bit byte or 16-bit word) from the DMA channel */
static int16_t fetch_unit(void) {
    int ch = x16 ? SB_DMA16 : SB_DMA8;
    if (dma[ch].masked) return 0;
    uint32_t a = dma_phys(ch);
    int16_t v;
    if (x16) { uint16_t w = (a + 1 < MEM_SIZE) ? (uint16_t)(MEM[a] | MEM[a + 1] << 8) : 0; v = xsigned ? (int16_t)w : (int16_t)(w - 0x8000); }
    else { uint8_t b = a < MEM_SIZE ? MEM[a] : 0x80; v = xsigned ? (int16_t)((int8_t)b << 8) : (int16_t)((b - 0x80) << 8); }
    dma_step(ch);
    return v;
}

/* one DSP sample frame (mono or stereo); handles block end / IRQ */
static void next_frame(void) {
    last_l = fetch_unit();
    last_r = xstereo ? fetch_unit() : last_l;
    if (xleft-- == 0) {
        if (x16) irq16_pending = 1; else irq8_pending = 1;
        sb_irq_req = 1;
        irq_pending = 1;                     /* wake the main thread's IRQ check */
        if (xauto) xleft = xblock; else xfer_on = 0;
    }
}

/* TFX_SB_WAV=<file>: record the card's output (44.1 kHz stereo 16-bit) */
static FILE *wav;
static uint32_t wav_bytes;
static void wav_close(void) {
    if (!wav) return;
    uint32_t v = 36 + wav_bytes;
    fseek(wav, 4, SEEK_SET); fwrite(&v, 4, 1, wav);
    fseek(wav, 40, SEEK_SET); fwrite(&wav_bytes, 4, 1, wav);
    fclose(wav); wav = NULL;
}
static void wav_open(const char *path) {
    wav = fopen(path, "wb");
    if (!wav) return;
    static const uint8_t hdr[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,
        0x44,0xac,0,0, 0x10,0xb1,2,0, 4,0,16,0, 'd','a','t','a',0,0,0,0};
    fwrite(hdr, 1, 44, wav);
    atexit(wav_close);
}

static void SDLCALL audio_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    int16_t *o = (int16_t *)stream;
    int frames = len / 4;
    /* SB16 volumes: master (30h/31h) x voice (32h/33h), 5 bits each */
    int vl = ((mixer[0x30] >> 3) * (mixer[0x32] >> 3)), vr = ((mixer[0x31] >> 3) * (mixer[0x33] >> 3));
    double step = (double)rate / OUT_RATE;
    for (int i = 0; i < frames; i++) {
        if (xfer_on && !xpaused) {
            xpos += step;
            while (xpos >= 1.0 && xfer_on) { xpos -= 1.0; next_frame(); }
            o[2 * i] = (int16_t)(last_l * vl / 961);
            o[2 * i + 1] = (int16_t)(last_r * vr / 961);
        } else {
            o[2 * i] = o[2 * i + 1] = 0;
        }
    }
    if (wav) { fwrite(stream, 1, (size_t)len, wav); wav_bytes += (uint32_t)len; }
}

static void audio_open(void) {
    if (dev) return;
    SDL_AudioSpec want = {0}, have;
    want.freq = OUT_RATE; want.format = AUDIO_S16LSB; want.channels = 2; want.samples = 512;
    want.callback = audio_cb;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) { rt_log("sb: cannot open audio: %s", SDL_GetError()); return; }
    SDL_PauseAudioDevice(dev, 0);
}

static void lock(void) { if (dev) SDL_LockAudioDevice(dev); }
static void unlock(void) { if (dev) SDL_UnlockAudioDevice(dev); }

static int cmd_len(uint8_t c) {
    if (c >= 0xb0 && c <= 0xcf) return 3;
    switch (c) {
    case 0x10: case 0x40: case 0xe0: case 0xe4: case 0x38: return 1;
    case 0x14: case 0x16: case 0x17: case 0x24: case 0x41: case 0x42: case 0x48: case 0x74:
    case 0x75: case 0x76: case 0x77: case 0x80: return 2;
    default: return 0;
    }
}

static void dsp_command(void) {
    TR("sb: dsp cmd %02x %02x %02x %02x", cmd, args[0], args[1], args[2]);
    switch (cmd) {
    case 0x10: break;                                   /* direct DAC: ignored */
    case 0x14: case 0x16: case 0x17:                    /* 8-bit single-cycle DMA */
        start_xfer(0, 0, (mixer[0x0e] & 2) != 0, 0, args[0] | args[1] << 8); break;
    case 0x1c: case 0x1f: case 0x90:                    /* 8-bit auto-init (high speed) */
        start_xfer(0, 1, (mixer[0x0e] & 2) != 0, 0, blk8); break;
    case 0x91: start_xfer(0, 0, (mixer[0x0e] & 2) != 0, 0, blk8); break;
    case 0x40: {                                        /* time constant */
        uint32_t r = 1000000u / (256u - args[0]);
        rate = (mixer[0x0e] & 2) ? r / 2 : r;
        break;
    }
    case 0x41: case 0x42: rate = (uint32_t)(args[0] << 8 | args[1]); if (rate < 4000) rate = 4000; break;
    case 0x48: blk8 = (uint32_t)(args[0] | args[1] << 8); break;
    case 0x80: sb_irq_req = 1; irq8_pending = 1; irq_pending = 1; break; /* silence block: just the IRQ */
    case 0xd0: case 0xd5: xpaused = 1; break;
    case 0xd4: case 0xd6: xpaused = 0; break;
    case 0xd9: case 0xda: xauto = 0; break;             /* exit auto-init after this block */
    case 0xd1: speaker = 1; break;
    case 0xd3: speaker = 0; break;
    case 0xd8: out_push(speaker ? 0xff : 0x00); break;
    case 0xe0: out_push((uint8_t)~args[0]); break;
    case 0xe1: out_push(4); out_push(5); break;         /* DSP 4.05: SB16 */
    case 0xe3: { const char *c = "COPYRIGHT (C) CREATIVE TECHNOLOGY LTD, 1992."; while (*c) out_push((uint8_t)*c++); out_push(0); break; }
    case 0xe4: test_reg = args[0]; break;
    case 0xe8: out_push(test_reg); break;
    case 0xf2: irq8_pending = 1; sb_irq_req = 1; irq_pending = 1; break;
    case 0xf3: irq16_pending = 1; sb_irq_req = 1; irq_pending = 1; break;
    case 0xf8: out_push(0); break;
    default:
        if (cmd >= 0xb0 && cmd <= 0xcf) {               /* SB16 generic DMA: B=16-bit, C=8-bit */
            int is16 = cmd < 0xc0, autoinit = (cmd & 4) != 0;
            uint8_t mode = args[0];
            start_xfer(is16, autoinit, (mode & 0x20) != 0, (mode & 0x10) != 0, (uint32_t)(args[1] | args[2] << 8));
        } else {
            TR("sb: unhandled dsp command %02x", cmd);
        }
    }
}

static void mixer_reset(void) {
    memset(mixer, 0, sizeof mixer);
    mixer[0x30] = mixer[0x31] = 0xc0;                   /* master */
    mixer[0x32] = mixer[0x33] = 0xc0;                   /* voice */
    mixer[0x80] = (uint8_t)(SB_IRQ == 2 ? 1 : SB_IRQ == 5 ? 2 : SB_IRQ == 7 ? 4 : SB_IRQ == 10 ? 8 : 2);
    mixer[0x81] = (uint8_t)((1 << SB_DMA8) | (1 << SB_DMA16));
}

uint8_t sb_in(uint16_t port) {
    uint8_t r = 0xff;
    switch (port - SB_BASE) {
    case 0x5:
        if (mixer_idx == 0x82) r = (uint8_t)((irq8_pending ? 1 : 0) | (irq16_pending ? 2 : 0));
        else r = mixer[mixer_idx];
        break;
    case 0xa: if (outq_n) { r = outq[0]; memmove(outq, outq + 1, --outq_n); } else r = 0; break;
    case 0xc: r = 0x7f; break;                          /* ready for a write */
    case 0xe: irq8_pending = 0; r = outq_n ? 0xff : 0x7f; break;
    case 0xf: irq16_pending = 0; r = 0xff; break;
    case 0x4: r = mixer_idx; break;
    }
    TR("sb: in %03x -> %02x", port, r);
    return r;
}

void sb_out(uint16_t port, uint8_t v) {
    TR("sb: out %03x = %02x", port, v);
    switch (port - SB_BASE) {
    case 0x4: mixer_idx = v; break;
    case 0x5:
        if (mixer_idx == 0x00) mixer_reset();
        else if (mixer_idx != 0x80 && mixer_idx != 0x81 && mixer_idx != 0x82) mixer[mixer_idx] = v;
        break;
    case 0x6:
        if (v & 1) reset_hi = 1;
        else if (reset_hi) {
            reset_hi = 0;
            lock(); stop_xfer(); unlock();
            outq_n = 0; nargs = need = 0; cmd = 0;
            irq8_pending = irq16_pending = 0;
            out_push(0xaa);
        }
        break;
    case 0xc:
        if (need) {
            args[nargs++] = v;
            if (nargs == need) { need = 0; audio_open(); lock(); dsp_command(); unlock(); }
        } else {
            cmd = v; nargs = 0; need = cmd_len(v);
            if (!need) { audio_open(); lock(); dsp_command(); unlock(); }
        }
        break;
    }
}

/* ---- DMA controller ports ----------------------------------------------------------- */
int dma_port(uint16_t p) { return p <= 0x0f || (p >= 0x80 && p <= 0x8f) || (p >= 0xc0 && p <= 0xdf); }

void dma_out(uint16_t p, uint8_t v) {
    TR("dma: out %02x = %02x", p, v);
    lock();
    if (p >= 0x80 && p <= 0x8f) {
        for (int ch = 0; ch < 8; ch++) if (page_port[ch] == p) dma[ch].page = v;
    } else {
        int hi = p >= 0xc0, reg = hi ? (p - 0xc0) / 2 : p;
        int base = hi ? 4 : 0;
        if (reg < 8) {                                  /* address / count of a channel */
            struct dmach *c = &dma[base + reg / 2];
            int lohi = dma_ff[hi]; dma_ff[hi] ^= 1;
            if (reg & 1) {
                c->base_count = lohi ? (c->base_count & 0xff) | (uint32_t)v << 8 : (c->base_count & 0xff00) | v;
                c->cur_count = c->base_count;
            } else {
                c->base_addr = lohi ? (c->base_addr & 0xff) | (uint32_t)v << 8 : (c->base_addr & 0xff00) | v;
                c->cur_addr = c->base_addr;
            }
        } else switch (reg) {
            case 0xa: dma[base + (v & 3)].masked = (v & 4) != 0; break;
            case 0xb: dma[base + (v & 3)].mode = v; break;
            case 0xc: dma_ff[hi] = 0; break;
            case 0xd: dma_ff[hi] = 0; for (int i = 0; i < 4; i++) dma[base + i].masked = 1; break;
            case 0xe: for (int i = 0; i < 4; i++) dma[base + i].masked = 0; break;
            case 0xf: for (int i = 0; i < 4; i++) dma[base + i].masked = (v >> i) & 1; break;
        }
    }
    unlock();
}

uint8_t dma_in(uint16_t p) {
    uint8_t r = 0;
    if (p >= 0x80 && p <= 0x8f) {
        for (int ch = 0; ch < 8; ch++) if (page_port[ch] == p) r = dma[ch].page;
    } else {
        int hi = p >= 0xc0, reg = hi ? (p - 0xc0) / 2 : p;
        int base = hi ? 4 : 0;
        if (reg < 8) {
            lock();
            struct dmach *c = &dma[base + reg / 2];
            uint32_t v = (reg & 1) ? c->cur_count : c->cur_addr;
            int lohi = dma_ff[hi]; dma_ff[hi] ^= 1;
            r = (uint8_t)(lohi ? v >> 8 : v);
            unlock();
        } else if (reg == 8) {
            r = 0;                                      /* status: no terminal counts reported */
        }
    }
    TR("dma: in %02x -> %02x", p, r);
    return r;
}

/* main thread (rt_service): deliver the card's IRQ */
void sb_service(void) {
    if (sb_irq_req) { sb_irq_req = 0; TR("sb: raise IRQ %d", SB_IRQ); irq_raise(SB_IRQ); }
}

int sb_port(uint16_t p) { return p >= SB_BASE && p <= SB_BASE + 0x0f; }

/* parse "A220 I7 D1 H5 T6 F3" */
static void sb_configure(const char *b) {
    for (const char *p = b; *p; p++) {
        if (p != b && p[-1] != ' ') continue;
        char k = (char)(*p & ~0x20);
        if (k == 'A') SB_BASE = (uint16_t)strtoul(p + 1, NULL, 16);
        else if (k == 'I') SB_IRQ = (int)strtol(p + 1, NULL, 10);
        else if (k == 'D') SB_DMA8 = (int)strtol(p + 1, NULL, 10);
        else if (k == 'H') SB_DMA16 = (int)strtol(p + 1, NULL, 10);
    }
    if (SB_IRQ < 2 || SB_IRQ > 7) SB_IRQ = 5;    /* only the master PIC is emulated */
    if (SB_DMA8 < 0 || SB_DMA8 > 3) SB_DMA8 = 1;
    if (SB_DMA16 < 4 || SB_DMA16 > 7) SB_DMA16 = 5;
}

/* BLASTER value for the environment and the card; game_dir's TFX.CFG first */
static char blaster[128] = "A220 I5 D1 H5 T6";
const char *sb_blaster(void) {
    extern char game_dir[512];
    char path[600], line[256];
    snprintf(path, sizeof path, "%s\\TFX.CFG", game_dir);
    FILE *f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof line, f))
            if (!_strnicmp(line, "BLASTER=", 8) && line[8] > ' ') {
                char *e = line + strlen(line);
                while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
                snprintf(blaster, sizeof blaster, "%s", line + 8);
            }
        fclose(f);
    } else if (getenv("BLASTER")) {
        snprintf(blaster, sizeof blaster, "%s", getenv("BLASTER"));
    }
    return blaster;
}

void sb_init(void) {
    trace = getenv("TFX_TRACE_SB") != NULL;
    if (getenv("TFX_SB_WAV")) { wav_open(getenv("TFX_SB_WAV")); audio_open(); }
    sb_configure(sb_blaster());
    rt_log("sb: Sound Blaster 16 at %03x, IRQ %d, DMA %d/%d (BLASTER=%s)", SB_BASE, SB_IRQ, SB_DMA8, SB_DMA16, blaster);
    mixer_reset();
    for (int i = 0; i < 8; i++) dma[i].masked = 1;
}
