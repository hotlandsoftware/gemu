#include "ntsc_decode.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void ntsc_decode(const NtscDecodeSpec *spec, const float *signal, uint32_t *out) {
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
        const float *row = signal + (size_t)line * spl;
        uint32_t *out_row = out + (size_t)line * ow;
        long line_base = (long)line * spl;   /* absolute sample index, for continuous phase */

        for (int ox = 0; ox < ow; ox++) {
            int center = (int)((long)ox * spl / ow);

            double y_acc = 0.0, i_acc = 0.0, q_acc = 0.0;
            int n = 0;
            for (int k = -taps / 2; k < taps / 2; k++) {
                int s = center + k;
                if (s < 0 || s >= spl) continue;
                double sample = row[s];
                long abs_idx = line_base + s;
                double phase = 2.0 * M_PI * fmod((double)abs_idx * cyc, 1.0);
                y_acc += sample;
                i_acc += sample * cos(phase);
                q_acc += sample * sin(phase);
                n++;
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
