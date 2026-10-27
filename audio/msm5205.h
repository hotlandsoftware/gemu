#pragma once
/*
 * OKI MSM5205 ADPCM speech synthesis LSI.
 *
 * The board feeds a 4-bit (or 3-bit) code on D0-D3 and the chip latches it
 * on each VCK edge; VCK is the oscillator divided by 96, 64 or 48 (S1/S2),
 * e.g. 4 kHz from a 384 kHz resonator.  Each code moves the 12-bit
 * signal by a step from the OKI ADPCM table and adapts the step size;
 * the result drives a 10-bit DAC.  RESET clears the signal and step and
 * holds the DAC at its midpoint.
 *
 * The board owns VCK: it calls msm5205_vck() once per period with the
 * code on the data pins, and reads msm5205_output() whenever it wants
 * the DAC level (which holds between VCK edges).
 */
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int16_t signal;    /* 12-bit, -2048..2047 */
    int     step;      /* 0..48 */
    bool    reset;     /* RESET pin */
    bool    bits3;     /* 4B/3B pin low: 3-bit codes on D1-D3 */
} Msm5205;

void  msm5205_init(Msm5205 *m);
void  msm5205_set_reset(Msm5205 *m, bool reset);
void  msm5205_vck(Msm5205 *m, uint8_t code);   /* one VCK period with D0-D3 = code */
float msm5205_output(const Msm5205 *m);        /* DAC: -1 .. +1 */
