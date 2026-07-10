#pragma once
#include <math.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Shared cos/sin lookup table for the NTSC encode/decode hot loops --
 * replaces per-sample libm cos()/sin() calls (measured as the dominant
 * real-time cost of -device crt: ~118M calls/sec for cheap mode, ~295M/sec
 * for the comb filter's 3x sample volume, at 60fps) with a table lookup.
 * 4096 entries gives ~0.09-degree resolution, far finer than the
 * oversampled signal itself needs. Callers pass phase as a [0,1) cycle
 * fraction rather than radians -- they already compute this via fmod for
 * phase-continuity reasons, so this only removes the trig call itself, not
 * the fmod. */

#define NTSC_TRIG_LUT_BITS 12
#define NTSC_TRIG_LUT_SIZE (1 << NTSC_TRIG_LUT_BITS)
#define NTSC_TRIG_LUT_MASK (NTSC_TRIG_LUT_SIZE - 1)

static float ntsc_trig_cos_lut[NTSC_TRIG_LUT_SIZE];
static float ntsc_trig_sin_lut[NTSC_TRIG_LUT_SIZE];
static bool  ntsc_trig_lut_ready;

static inline void ntsc_trig_lut_init(void) {
    if (ntsc_trig_lut_ready) return;
    for (int i = 0; i < NTSC_TRIG_LUT_SIZE; i++) {
        double theta = 2.0 * M_PI * i / NTSC_TRIG_LUT_SIZE;
        ntsc_trig_cos_lut[i] = (float)cos(theta);
        ntsc_trig_sin_lut[i] = (float)sin(theta);
    }
    ntsc_trig_lut_ready = true;
}

static inline float ntsc_cos_frac(double frac) {
    uint32_t idx = (uint32_t)(frac * NTSC_TRIG_LUT_SIZE) & NTSC_TRIG_LUT_MASK;
    return ntsc_trig_cos_lut[idx];
}

static inline float ntsc_sin_frac(double frac) {
    uint32_t idx = (uint32_t)(frac * NTSC_TRIG_LUT_SIZE) & NTSC_TRIG_LUT_MASK;
    return ntsc_trig_sin_lut[idx];
}

/* Wraps a value known to already be within one cycle-width of [0,1) --
 * cheap alternative to fmod for the encoder's phase-minus-color-phase
 * subtraction, whose result is bounded to [-0.5, 1.5). */
static inline double ntsc_wrap01(double x) {
    if (x < 0.0) x += 1.0;
    if (x >= 1.0) x -= 1.0;
    return x;
}
