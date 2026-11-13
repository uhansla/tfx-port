/* MPU-401 (UART mode) at 330h/331h, as used by START.EXE's Roland SCC-1 and
 * LAPC-1 drivers.  MIDI bytes go to a Windows MIDI output: the default
 * (Microsoft GS Wavetable Synth, a General MIDI/GS synthesiser that suits
 * the SCC-1 music) or the device whose name contains TFX_MIDI_DEVICE (for
 * example an MT-32 emulator such as Munt for the LAPC-1 music). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <mmsystem.h>
#include "rt.h"

#define MPU_BASE 0x330
static HMIDIOUT midi;
static int midi_tried;
static uint8_t ack_q[8]; static int ack_n;
static int uart;
static uint8_t msg[3], run_status; static int msg_n, msg_need;
static uint8_t sysex[1024]; static int sysex_n, in_sysex;
static int trace;

static void midi_open(void) {
    if (midi_tried) return;
    midi_tried = 1;
    trace = getenv("TFX_TRACE_MIDI") != NULL;
    UINT dev = MIDI_MAPPER;
    const char *want = getenv("TFX_MIDI_DEVICE");
    UINT n = midiOutGetNumDevs();
    for (UINT i = 0; i < n; i++) {
        MIDIOUTCAPSA caps;
        if (midiOutGetDevCapsA(i, &caps, sizeof caps) != MMSYSERR_NOERROR) continue;
        rt_log("mpu: MIDI output %u: %s", i, caps.szPname);
        if (want && strstr(caps.szPname, want)) dev = i;
    }
    if (midiOutOpen(&midi, dev, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { midi = NULL; rt_log("mpu: cannot open MIDI output"); }
}

static int data_len(uint8_t st) {
    switch (st & 0xf0) { case 0xc0: case 0xd0: return 1; case 0xf0: return 0; default: return 2; }
}

static void send_sysex(void) {
    if (!midi || sysex_n < 2) return;
    MIDIHDR h; memset(&h, 0, sizeof h);
    h.lpData = (LPSTR)sysex; h.dwBufferLength = (DWORD)sysex_n;
    if (midiOutPrepareHeader(midi, &h, sizeof h) == MMSYSERR_NOERROR) {
        midiOutLongMsg(midi, &h, sizeof h);
        for (int i = 0; i < 200 && !(h.dwFlags & MHDR_DONE); i++) Sleep(1);
        midiOutUnprepareHeader(midi, &h, sizeof h);
    }
}

static void midi_byte(uint8_t b) {
    midi_open();
    if (in_sysex) {
        if ((size_t)sysex_n < sizeof sysex) sysex[sysex_n++] = b;
        if (b == 0xf7) { in_sysex = 0; send_sysex(); }
        return;
    }
    if (b >= 0xf8) { if (midi) midiOutShortMsg(midi, b); return; }      /* realtime */
    if (b == 0xf0) { in_sysex = 1; sysex_n = 0; sysex[sysex_n++] = b; return; }
    if (b & 0x80) { run_status = b; msg_n = 0; msg_need = data_len(b); if (!msg_need && midi) midiOutShortMsg(midi, b); return; }
    if (!run_status) return;
    msg[msg_n++] = b;
    if (msg_n == msg_need) {
        DWORD m = run_status | (DWORD)msg[0] << 8 | (msg_need > 1 ? (DWORD)msg[1] << 16 : 0);
        if (trace) rt_log("midi: %02x %02x %02x", run_status, msg[0], msg_need > 1 ? msg[1] : 0);
        if (midi) midiOutShortMsg(midi, m);
        msg_n = 0;
    }
}

int mpu_port(uint16_t p) { return p == MPU_BASE || p == MPU_BASE + 1; }

uint8_t mpu_in(uint16_t p) {
    if (p == MPU_BASE) {                       /* data: command acknowledgements */
        if (ack_n) { uint8_t v = ack_q[0]; memmove(ack_q, ack_q + 1, --ack_n); return v; }
        return 0xfe;
    }
    return (uint8_t)(ack_n ? 0x00 : 0x80);     /* status: bit 6 clear = ready, bit 7 clear = data waiting */
}

void mpu_out(uint16_t p, uint8_t v) {
    if (p == MPU_BASE + 1) {                   /* command */
        if (v == 0xff) { uart = 0; if (ack_n < 8) ack_q[ack_n++] = 0xfe; }
        else if (v == 0x3f) { uart = 1; if (ack_n < 8) ack_q[ack_n++] = 0xfe; midi_open(); }
        else if (ack_n < 8) ack_q[ack_n++] = 0xfe;
        return;
    }
    midi_byte(v);                              /* data: MIDI stream */
}

void mpu_shutdown(void) {
    if (!midi) return;
    for (int ch = 0; ch < 16; ch++) midiOutShortMsg(midi, (DWORD)(0xb0 | ch) | 123 << 8);   /* all notes off */
    midiOutReset(midi);
    midiOutClose(midi);
    midi = NULL;
}
