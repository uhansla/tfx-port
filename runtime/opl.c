/* OPL2 (YM3812, the AdLib / Sound Blaster FM chip) at 388h/389h.
 *
 * Written for this port from the chip's documented behaviour: 9 two-operator
 * channels (18 operators), frequency number/block/multiplier, the four OPL2
 * waveforms, ADSR envelopes (rates scaled by key-scale rate, sustain level,
 * sustained/percussive mode), total level and key-scale level, feedback,
 * FM or additive connection, tremolo and vibrato, a simplified rhythm mode,
 * and the two timers with their status flags (drivers use them to detect the
 * card).  It renders at the chip's native 49,716 Hz on its own SDL audio
 * device; register writes take the device lock.  Timbre is close to the real
 * chip, not bit-exact. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL.h>
#include "rt.h"

#define OPL_RATE 49716
#define PI2 6.283185307179586

uint64_t host_ticks_us(void);

enum { ENV_OFF, ENV_ATT, ENV_DEC, ENV_SUS, ENV_REL };
struct op {
    int am, vib, egt, ksr, mult, ksl, tl, ar, dr, sl, rr, ws;
    double phase, env;          /* phase in cycles; env attenuation in dB */
    int state;
    double out, prev;           /* last two outputs (feedback) */
};
struct ch { int fnum, block, key, fb, cnt; };

static struct op ops[18];
static struct ch chs[9];
static uint8_t regs[256];
static int index_reg;
static int wse, rhythm, am_deep, vib_deep;
static double lfo_am_phase, lfo_vib_phase;
static uint32_t noise = 1;
static SDL_AudioDeviceID dev;

/* timers */
static int t1_on, t2_on, t1_mask, t2_mask, t1_flag, t2_flag;
static uint64_t t1_start, t2_start;
/* chip time: host time plus 1 us per status read, since drivers time their
 * waits (e.g. the 80 us timer test in AdLib detection) by reading the port */
static uint64_t io_us;
static uint64_t chip_us(void) { return host_ticks_us() + io_us; }

static const double mult_tab[16] = {0.5, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 10, 12, 12, 15, 15};
static const double ksl_tab[16] = {0, 18.00, 24.00, 27.75, 30.00, 32.25, 33.75, 35.25,
                                   36.00, 37.50, 38.25, 39.00, 39.75, 40.50, 41.25, 42.00};
/* register offset 0x00..0x15 -> operator index (-1 = unused) */
static const int slot_op[22] = {0, 1, 2, 3, 4, 5, -1, -1, 6, 7, 8, 9, 10, 11, -1, -1, 12, 13, 14, 15, 16, 17};
/* channel -> modulator / carrier operator */
static const int ch_mod[9] = {0, 1, 2, 6, 7, 8, 12, 13, 14};
static const int ch_car[9] = {3, 4, 5, 9, 10, 11, 15, 16, 17};

static int op_channel(int o) { for (int c = 0; c < 9; c++) if (ch_mod[c] == o || ch_car[c] == o) return c; return 0; }

/* effective envelope rate 0..63 */
static int eff_rate(struct op *o, int r) {
    if (!r) return 0;
    struct ch *c = &chs[op_channel((int)(o - ops))];
    int rof = (c->block << 1) | ((c->fnum >> 9) & 1);
    if (!o->ksr) rof >>= 2;
    int e = r * 4 + rof;
    return e > 63 ? 63 : e;
}
/* dB per sample for a 96 dB decay at effective rate e */
static double decay_step(int e) {
    if (e < 4) return 0;
    double ms = 39280.64 / pow(2.0, (e - 4) / 4.0);
    return 96.0 / (ms * OPL_RATE / 1000.0);
}
/* multiplicative attack factor per sample at effective rate e */
static double attack_factor(int e) {
    if (e < 4) return 1.0;
    if (e >= 60) return 0.0;                         /* instantaneous */
    double ms = 2826.24 / pow(2.0, (e - 4) / 4.0);
    double n = ms * OPL_RATE / 1000.0;
    return pow(0.001, 1.0 / n);                      /* 96 dB -> ~0.1 dB in n samples */
}

static void key_on(int o) { ops[o].state = ENV_ATT; ops[o].phase = 0; }
static void key_off(int o) { if (ops[o].state != ENV_OFF) ops[o].state = ENV_REL; }

static void opl_write(int r, uint8_t v) {
    regs[r] = v;
    if (r == 0x01) { wse = (v & 0x20) != 0; return; }
    if (r == 0x04) {
        if (v & 0x80) { t1_flag = t2_flag = 0; return; }
        t1_mask = (v & 0x40) != 0; t2_mask = (v & 0x20) != 0;
        if ((v & 1) && !t1_on) t1_start = chip_us();
        if ((v & 2) && !t2_on) t2_start = chip_us();
        t1_on = v & 1; t2_on = (v & 2) != 0;
        return;
    }
    if (r == 0xbd) {
        int old = rhythm ? regs[0xbd] : 0;
        (void)old;
        am_deep = (v & 0x80) != 0; vib_deep = (v & 0x40) != 0;
        rhythm = (v & 0x20) != 0;
        if (rhythm) {
            static uint8_t prev;
            uint8_t on = v & 0x1f, chg = on ^ prev;
            prev = on;
            /* BD: ch6 both ops; SD: op16; TT: op14; CY: op17; HH: op13 */
            if (chg & 0x10) { if (on & 0x10) { key_on(12); key_on(15); } else { key_off(12); key_off(15); } }
            if (chg & 0x08) { if (on & 0x08) key_on(16); else key_off(16); }
            if (chg & 0x04) { if (on & 0x04) key_on(14); else key_off(14); }
            if (chg & 0x02) { if (on & 0x02) key_on(17); else key_off(17); }
            if (chg & 0x01) { if (on & 0x01) key_on(13); else key_off(13); }
        }
        return;
    }
    int lo = r & 0x1f;
    if (r >= 0x20 && r <= 0x95 && lo <= 0x15 && slot_op[lo] >= 0) {
        struct op *o = &ops[slot_op[lo]];
        switch (r & 0xe0) {
        case 0x20: o->am = (v >> 7) & 1; o->vib = (v >> 6) & 1; o->egt = (v >> 5) & 1; o->ksr = (v >> 4) & 1; o->mult = v & 15; break;
        case 0x40: o->ksl = v >> 6; o->tl = v & 63; break;
        case 0x60: o->ar = v >> 4; o->dr = v & 15; break;
        case 0x80: o->sl = v >> 4; o->rr = v & 15; break;
        }
        return;
    }
    if (r >= 0xe0 && r <= 0xf5 && slot_op[r - 0xe0] >= 0) { ops[slot_op[r - 0xe0]].ws = v & 3; return; }
    if (r >= 0xa0 && r <= 0xa8) { struct ch *c = &chs[r - 0xa0]; c->fnum = (c->fnum & 0x300) | v; return; }
    if (r >= 0xb0 && r <= 0xb8) {
        int n = r - 0xb0; struct ch *c = &chs[n];
        c->fnum = (c->fnum & 0xff) | ((v & 3) << 8);
        c->block = (v >> 2) & 7;
        int k = (v >> 5) & 1;
        if (k && !c->key) { key_on(ch_mod[n]); key_on(ch_car[n]); }
        if (!k && c->key) { key_off(ch_mod[n]); key_off(ch_car[n]); }
        c->key = k;
        return;
    }
    if (r >= 0xc0 && r <= 0xc8) { struct ch *c = &chs[r - 0xc0]; c->fb = (v >> 1) & 7; c->cnt = v & 1; return; }
}

static double wave(int ws, double p) {
    p -= floor(p);
    double s = sin(PI2 * p);
    if (!wse) ws = 0;
    switch (ws) {
    case 1: return p < 0.5 ? s : 0;
    case 2: return fabs(s);
    case 3: return fmod(p, 0.5) < 0.25 ? fabs(s) : 0;
    default: return s;
    }
}

/* one sample of an operator, with phase modulation pm (cycles) */
static double op_sample(struct op *o, struct ch *c, double pm, double vib_mul, double am_db) {
    /* envelope */
    switch (o->state) {
    case ENV_ATT: {
        double f = attack_factor(eff_rate(o, o->ar));
        o->env *= f;
        if (o->env < 0.1) { o->env = 0; o->state = ENV_DEC; }
        break;
    }
    case ENV_DEC: {
        o->env += decay_step(eff_rate(o, o->dr));
        double sl = o->sl == 15 ? 93 : o->sl * 3.0;
        if (o->env >= sl) { o->env = sl; o->state = ENV_SUS; }
        break;
    }
    case ENV_SUS:
        if (!o->egt) o->env += decay_step(eff_rate(o, o->rr));   /* percussive: keep decaying */
        break;
    case ENV_REL:
        o->env += decay_step(eff_rate(o, o->rr));
        if (o->env >= 96) { o->env = 96; o->state = ENV_OFF; }
        break;
    default: o->env = 96; break;
    }
    if (o->env > 96) o->env = 96;
    /* level */
    double ksl = 0;
    if (o->ksl) {
        ksl = ksl_tab[c->fnum >> 6] - 6.0 * (7 - c->block);
        if (ksl < 0) ksl = 0;
        ksl = o->ksl == 1 ? ksl / 2 : o->ksl == 2 ? ksl / 4 : ksl;
    }
    double att = o->env + o->tl * 0.75 + ksl + (o->am ? am_db : 0);
    double amp = att >= 96 ? 0 : pow(10.0, -att / 20.0);
    /* phase */
    double f = c->fnum * pow(2.0, c->block) * mult_tab[o->mult] / 1048576.0;   /* cycles per sample */
    if (o->vib) f *= vib_mul;
    o->phase += f;
    if (o->phase > 1e6) o->phase -= floor(o->phase);
    double v = amp * wave(o->ws, o->phase + pm);
    o->prev = o->out; o->out = v;
    return v;
}

/* TFX_OPL_WAV=<file>: record the FM output (49716 Hz stereo 16-bit) */
static FILE *wav;
static uint32_t wav_bytes;
static void wav_close(void) {
    if (!wav) return;
    uint32_t v = 36 + wav_bytes;
    fseek(wav, 4, SEEK_SET); fwrite(&v, 4, 1, wav);
    fseek(wav, 40, SEEK_SET); fwrite(&wav_bytes, 4, 1, wav);
    fclose(wav); wav = NULL;
}

static void SDLCALL opl_cb(void *ud, Uint8 *stream, int len) {
    (void)ud;
    int16_t *out = (int16_t *)stream;
    int n = len / 4;
    for (int i = 0; i < n; i++) {
        lfo_am_phase += 3.7 / OPL_RATE; lfo_vib_phase += 6.07 / OPL_RATE;
        double am_db = (am_deep ? 4.8 : 1.0) * (1 + sin(PI2 * lfo_am_phase)) / 2;
        double vib_mul = pow(2.0, (vib_deep ? 14.0 : 7.0) / 1200.0 * sin(PI2 * lfo_vib_phase));
        noise = (noise >> 1) ^ ((noise & 1) ? 0x400181u : 0);
        double mix = 0;
        int nch = rhythm ? 6 : 9;
        for (int c = 0; c < nch; c++) {
            struct ch *ch = &chs[c];
            struct op *m = &ops[ch_mod[c]], *cr = &ops[ch_car[c]];
            if (m->state == ENV_OFF && cr->state == ENV_OFF) continue;
            double fb = ch->fb ? (m->out + m->prev) * 4.0 / (double)(1 << (9 - ch->fb)) : 0;
            double mo = op_sample(m, ch, fb, vib_mul, am_db);
            double co = ch->cnt ? op_sample(cr, ch, 0, vib_mul, am_db) + mo : op_sample(cr, ch, mo * 4.0, vib_mul, am_db);
            mix += co;
        }
        if (rhythm) {
            /* BD: channel 6 as a normal channel (output doubled, as on the chip) */
            struct ch *c6 = &chs[6], *c7 = &chs[7], *c8 = &chs[8];
            struct op *bm = &ops[12], *bc = &ops[15];
            if (bm->state != ENV_OFF || bc->state != ENV_OFF) {
                double fb = c6->fb ? (bm->out + bm->prev) * 4.0 / (double)(1 << (9 - c6->fb)) : 0;
                double mo = op_sample(bm, c6, fb, vib_mul, am_db);
                mix += 2 * (c6->cnt ? op_sample(bc, c6, 0, vib_mul, am_db) : op_sample(bc, c6, mo * 4.0, vib_mul, am_db));
            }
            double nz = (noise & 1) ? 1.0 : -1.0;
            /* HH (op13), SD (op16), TT (op14), CY (op17): noise-shaped approximations */
            if (ops[13].state != ENV_OFF) mix += 2 * op_sample(&ops[13], c7, 0.25 * nz, vib_mul, am_db);
            if (ops[16].state != ENV_OFF) mix += 2 * op_sample(&ops[16], c7, 0.15 * nz, vib_mul, am_db);
            if (ops[14].state != ENV_OFF) mix += 2 * op_sample(&ops[14], c8, 0, vib_mul, am_db);
            if (ops[17].state != ENV_OFF) { double v = op_sample(&ops[17], c8, 0.5 * nz, vib_mul, am_db); mix += 2 * (v > 0 ? fabs(v) : -fabs(v)); }
        }
        /* output coupling capacitor: DC-blocking high-pass (~5 Hz) */
        static double dc_x, dc_y;
        double hp = mix - dc_x + 0.9994 * dc_y;
        dc_x = mix; dc_y = hp;
        int s = (int)(hp * 3500.0);
        if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
        out[2 * i] = out[2 * i + 1] = (int16_t)s;
    }
    if (wav) { fwrite(stream, 1, (size_t)len, wav); wav_bytes += (uint32_t)len; }
}

static void opl_open(void) {
    static int tried;
    if (dev || tried) return;
    tried = 1;
    for (int i = 0; i < 18; i++) { ops[i].env = 96; ops[i].state = ENV_OFF; }
    if (getenv("TFX_OPL_WAV") && (wav = fopen(getenv("TFX_OPL_WAV"), "wb")) != NULL) {
        static const uint8_t hdr[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,
            0x34,0xc2,0,0, 0xd0,0x08,3,0, 4,0,16,0, 'd','a','t','a',0,0,0,0};
        fwrite(hdr, 1, 44, wav);
        atexit(wav_close);
    }
    /* the AdLib driver initialises the chip before the video/audio start-up */
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { rt_log("opl: SDL audio: %s", SDL_GetError()); return; }
    SDL_AudioSpec want = {0}, have;
    want.freq = OPL_RATE; want.format = AUDIO_S16LSB; want.channels = 2; want.samples = 1024;
    want.callback = opl_cb;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev) { rt_log("opl: cannot open audio: %s", SDL_GetError()); return; }
    if (have.freq != OPL_RATE) rt_log("opl: device runs at %d Hz (SDL resamples)", have.freq);
    SDL_PauseAudioDevice(dev, 0);
}

int opl_port(uint16_t p) { return p == 0x388 || p == 0x389; }

static int opl_trace = -1;
uint8_t opl_in(uint16_t p) {
    if (opl_trace < 0) opl_trace = getenv("TFX_TRACE_OPL") != NULL;
    if (p != 0x388) return 0xff;
    io_us++;
    uint64_t now = chip_us();
    if (t1_on && !t1_flag && (now - t1_start) >= (uint64_t)(256 - regs[2]) * 80) t1_flag = !t1_mask;
    if (t2_on && !t2_flag && (now - t2_start) >= (uint64_t)(256 - regs[3]) * 320) t2_flag = !t2_mask;
    return (uint8_t)(((t1_flag || t2_flag) ? 0x80 : 0) | (t1_flag ? 0x40 : 0) | (t2_flag ? 0x20 : 0));
}

void opl_out(uint16_t p, uint8_t v) {
    if (opl_trace < 0) opl_trace = getenv("TFX_TRACE_OPL") != NULL;
    if (p == 0x388) { index_reg = v; return; }
    if (opl_trace) rt_log("opl: %02x = %02x", index_reg, v);
    opl_open();
    if (dev) SDL_LockAudioDevice(dev);
    opl_write(index_reg, v);
    if (dev) SDL_UnlockAudioDevice(dev);
}
