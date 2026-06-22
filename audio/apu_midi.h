#pragma once
#include "midi_out.h"
#include <stdint.h>

/* Watches 2A03 APU register writes and translates them to MIDI events.
 * Plug into Apu2a03.write_tap to activate. */
typedef struct {
    MidiOut *out;
    int8_t   note[4];      /* current MIDI note: [0]=P1 [1]=P2 [2]=Tri [3]=Noise; -1=off */
    uint8_t  pulse_vol[2]; /* last $4000/$4004 (duty+vol byte, used for velocity) */
    uint8_t  noise_vol;    /* last $400C */
    uint8_t  pulse_lo[2];  /* last $4002/$4006 timer low byte */
    uint8_t  tri_lo;       /* last $400A timer low byte */
    uint8_t  noise_per;    /* last $400E (mode bit + period index) */
} ApuMidi;

void apu_midi_open (ApuMidi *m); /* opens ALSA port + sends GM program changes */
void apu_midi_close(ApuMidi *m);
void apu_midi_tap  (uint16_t addr, uint8_t val, void *ud); /* Apu2a03.write_tap */
