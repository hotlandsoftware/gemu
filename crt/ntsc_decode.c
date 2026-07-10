#include "ntsc_decode.h"
#include "fast_trig.h"
#include <math.h>

/* Accumulate one scanline's window of samples into the running Y/I/Q sums.
 * line_base is that scanline's absolute sample-index origin (line_index *
 * samples_per_line — i.e. its position in the flat signal buffer), which
 * is also what phase continuity is computed from, so a window "belonging"
 * to a different line than the one being decoded still gets an exactly
 * correct phase reference for its own samples, not an approximated one.
 *
 * Uniform (boxcar) weighting over exactly one subcarrier period is load
 * bearing, not just simple: a Hamming-style taper was tried here to cut
 * edge ringing, but a non-uniform window has nonzero DFT content at the
 * subcarrier's own fundamental frequency over this window length, which
 * lets the pedestal (luma) leak into the I/Q channels -- an effect a
 * uniform window cancels for free via discrete orthogonality (N equally
 * spaced samples over exactly one period sum a single-frequency term to
 * exactly zero). That leak's phase depends on absolute signal position,
 * so it doesn't average out -- it showed up as the whole picture cycling
 * through rainbow colors even in perfectly flat regions, far worse than
 * the ringing it was meant to fix. Properly windowing this decode
 * without that leak needs a real multi-null FIR design, not a drop-in
 * taper; deferred rather than attempted half-fixed. */
static void accumulate_window(const float *signal, int spl, long line_base,
                              int center, int taps, double cyc,
                              double *y_acc, double *i_acc, double *q_acc, int *n) {
    const float *row = signal + line_base;
    int k0 = -taps / 2;
    /* Phase advances by a fixed `cyc` per sample, so one fmod() at the
     * window's first tap plus a running add+wrap for the rest replaces
     * taps-1 redundant fmod()s (this loop runs 3x per output pixel for
     * the comb filter, so it's the hottest part of the whole pipeline). */
    double frac = fmod((double)(line_base + center + k0) * cyc, 1.0);
    if (frac < 0.0) frac += 1.0;
    for (int k = k0; k < taps / 2; k++) {
        int s = center + k;
        if (s >= 0 && s < spl) {
            double sample = row[s];
            *y_acc += sample;
            *i_acc += sample * ntsc_cos_frac(frac);
            *q_acc += sample * ntsc_sin_frac(frac);
            (*n)++;
        }
        frac += cyc;
        if (frac >= 1.0) frac -= 1.0;
    }
}

void ntsc_decode(const NtscDecodeSpec *spec, const float *signal, uint32_t *out) {
    ntsc_trig_lut_init();
    int spl = spec->samples_per_line;
    int lines = spec->lines;
    int ow = spec->out_width;
    double cyc = spec->subcarrier_cycles_per_sample;

    /* Integrate over one full subcarrier cycle: this cancels chroma out of
     * the luma average and gives synchronous I/Q demodulation a clean,
     * correctly-scaled result (a window that isn't a whole cycle leaves
     * chroma residue bleeding into luma). */
    int taps = (int)(1.0 / cyc + 0.5);
    if (taps < 4) taps = 4;

    for (int line = 0; line < lines; line++) {
        uint32_t *out_row = out + (size_t)line * ow;

        /* Comb filter: NTSC's classic 2-line, 180-degrees-per-line comb
         * design doesn't apply here — this signal (like real NES hardware,
         * whose dot clock isn't a broadcast-standard multiple of
         * colorburst either) advances a non-180-degree amount per line and
         * only realigns every 3 lines (see crt/ntsc_decode.h and the
         * feature's design notes for the exact numbers). Pooling 3 lines'
         * samples into one joint demodulation — rather than the 2-line
         * add/subtract a broadcast comb does — is the structure that
         * actually matches this signal's real periodicity. */
        int line_lo = spec->comb_filter && line > 0        ? line - 1 : line;
        int line_hi = spec->comb_filter && line < lines - 1 ? line + 1 : line;

        for (int ox = 0; ox < ow; ox++) {
            int center = (int)((long)ox * spl / ow);

            double y_acc = 0.0, i_acc = 0.0, q_acc = 0.0;
            int n = 0;
            accumulate_window(signal, spl, (long)line * spl, center, taps, cyc,
                              &y_acc, &i_acc, &q_acc, &n);
            if (spec->comb_filter) {
                accumulate_window(signal, spl, (long)line_lo * spl, center, taps, cyc,
                                  &y_acc, &i_acc, &q_acc, &n);
                accumulate_window(signal, spl, (long)line_hi * spl, center, taps, cyc,
                                  &y_acc, &i_acc, &q_acc, &n);
            }

            if (n == 0) n = 1;
            double Y = y_acc / n;
            /* x2: synchronous demodulation of a cosine-modulated signal
             * recovers half the true amplitude; restore it here. */
            double I = (i_acc / n) * 2.0;
            double Q = (q_acc / n) * 2.0;

            double r = Y + 0.956 * I + 0.621 * Q;
            double g = Y - 0.272 * I - 0.647 * Q;
            double b = Y - 1.106 * I + 1.703 * Q;

            uint8_t R = (uint8_t)(r < 0.0 ? 0 : r > 1.0 ? 255 : r * 255.0 + 0.5);
            uint8_t G = (uint8_t)(g < 0.0 ? 0 : g > 1.0 ? 255 : g * 255.0 + 0.5);
            uint8_t B = (uint8_t)(b < 0.0 ? 0 : b > 1.0 ? 255 : b * 255.0 + 0.5);
            out_row[ox] = 0xFF000000u | ((uint32_t)R << 16) | ((uint32_t)G << 8) | B;
        }
    }
}
