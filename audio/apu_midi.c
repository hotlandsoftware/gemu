#include "apu_midi.h"
#include <math.h>
#include <string.h>

/* MIDI channel assignments */
#define CH_P1    0   /* pulse 1  → Lead 1 (square) */
#define CH_P2    1   /* pulse 2  → Lead 1 (square) */
#define CH_TRI   2   /* triangle → Pan Flute */
#define CH_NOISE 9   /* noise    → GM drum channel */

/* Noise period index 0-15, high-freq → hi-hat, mid → snare, low → kick */
static const uint8_t noise_drum[16] = {
    42, 42, 42, 42,   /* 0-3:  closed hi-hat  */
    38, 38, 46, 46,   /* 4-7:  snare / open hi-hat */
    36, 36, 36, 36,   /* 8-11: bass kick */
    41, 41, 35, 35,   /* 12-15: floor tom / bass drum 2 */
};

static int period_to_note(uint16_t period, int triangle) {
    if (period < 8) return -1;
    double freq = 1789773.0 / ((triangle ? 32.0 : 16.0) * (period + 1));
    int note    = (int)(69.0 + 12.0 * log2(freq / 440.0) + 0.5);
    return (note >= 0 && note <= 127) ? note : -1;
}

static int vol_to_vel(uint8_t vol_reg) {
    /* bit4 = const_vol, bits[3:0] = level 0-15 */
    int vel = (vol_reg & 0x10) ? (vol_reg & 0xF) * 8 + 7 : 100;
    return vel < 1 ? 1 : vel > 127 ? 127 : vel;
}

void apu_midi_open(ApuMidi *m) {
    memset(m, 0, sizeof *m);
    for (int i = 0; i < 4; i++) m->note[i] = -1;
    m->out = midi_out_open();
    if (!m->out) return;
    midi_out_program(m->out, CH_P1,  80);  /* Lead 1 (square) */
    midi_out_program(m->out, CH_P2,  80);  /* Lead 1 (square) */
    midi_out_program(m->out, CH_TRI, 75);  /* Pan Flute       */
}

void apu_midi_close(ApuMidi *m) {
    if (!m->out) return;
    midi_out_all_off(m->out);
    midi_out_close(m->out);
    m->out = NULL;
}

static void ch_off(ApuMidi *m, int idx, int ch) {
    if (m->note[idx] >= 0) {
        midi_out_note_off(m->out, ch, (int)m->note[idx]);
        m->note[idx] = -1;
    }
}

static void pulse_on(ApuMidi *m, int idx, uint16_t period) {
    int ch   = idx ? CH_P2 : CH_P1;
    int note = period_to_note(period, 0);
    if (note < 0) { ch_off(m, idx, ch); return; }
    if (m->note[idx] != (int8_t)note) ch_off(m, idx, ch);
    /* same note: skip note-off so the synth re-attacks cleanly */
    midi_out_note_on(m->out, ch, note, vol_to_vel(m->pulse_vol[idx]));
    m->note[idx] = (int8_t)note;
}

static void tri_on(ApuMidi *m, uint16_t period) {
    int note = period_to_note(period, 1);
    if (note < 0) { ch_off(m, 2, CH_TRI); return; }
    if (m->note[2] != (int8_t)note) ch_off(m, 2, CH_TRI);
    midi_out_note_on(m->out, CH_TRI, note, 80);
    m->note[2] = (int8_t)note;
}

static void noise_on(ApuMidi *m) {
    int metallic = (m->noise_per & 0x80) != 0;
    int drum     = metallic ? 46 : (int)noise_drum[m->noise_per & 0x0F];
    ch_off(m, 3, CH_NOISE);
    midi_out_note_on(m->out, CH_NOISE, drum, vol_to_vel(m->noise_vol));
    m->note[3] = (int8_t)drum;
}

void apu_midi_tap(uint16_t addr, uint8_t val, void *ud) {
    ApuMidi *m = ud;
    if (!m->out) return;

    switch (addr) {
    /* Cache volume/duty for velocity on next trigger */
    case 0x4000: m->pulse_vol[0] = val; break;
    case 0x4004: m->pulse_vol[1] = val; break;
    case 0x400C: m->noise_vol    = val; break;

    /* Timer low bytes */
    case 0x4002: m->pulse_lo[0] = val; break;
    case 0x4006: m->pulse_lo[1] = val; break;
    case 0x400A: m->tri_lo      = val; break;

    /* Noise mode + period index */
    case 0x400E: m->noise_per = val; break;

    /* Triggers: high-byte write = note-on */
    case 0x4003:
        pulse_on(m, 0, (uint16_t)((val & 7) << 8) | m->pulse_lo[0]);
        break;
    case 0x4007:
        pulse_on(m, 1, (uint16_t)((val & 7) << 8) | m->pulse_lo[1]);
        break;
    case 0x400B:
        tri_on(m, (uint16_t)((val & 7) << 8) | m->tri_lo);
        break;
    case 0x400F:
        noise_on(m);
        break;

    /* $4015 channel-enable: disabled channels get note-off */
    case 0x4015:
        if (!(val & 0x01)) ch_off(m, 0, CH_P1);
        if (!(val & 0x02)) ch_off(m, 1, CH_P2);
        if (!(val & 0x04)) ch_off(m, 2, CH_TRI);
        if (!(val & 0x08)) ch_off(m, 3, CH_NOISE);
        break;
    }
}
