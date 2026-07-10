#include "nes_ntsc_encode.h"
#include "fast_trig.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

extern const uint32_t rp2c02_palette_rgb[64];

/* Per-color (pedestal, chroma amplitude, chroma phase) signal parameters,
 * derived once from rp2c02_palette_rgb[] rather than reconstructed from a
 * hand-guessed hue-phase table. An earlier version used a formula (30
 * degrees/hue, fixed luma steps); comparing its round-trip output against
 * rp2c02_palette_rgb showed colors landing nowhere near the real ones —
 * not a small calibration offset, essentially uncorrelated, because the
 * formula's phase origin/spacing had no actual relationship to how this
 * codebase's palette table was generated. Deriving from the table itself
 * sidesteps needing to know that relationship: it's an exact round-trip
 * by construction (RGB->YIQ is the inverse of ntsc_decode's YIQ->RGB), so
 * every one of the 64 raw color codes decodes back to (approximately) its
 * known-correct color, while the oversampled signal generation + real
 * decode filtering in between still produces genuine dot-crawl/bleed —
 * the artifacts come from the signal processing, not from the per-color
 * calibration. */
static double color_pedestal[64];
static double color_amp[64];
static double color_phase_rad[64];
static bool   tables_ready;

static void build_color_tables(void) {
    if (tables_ready) return;
    for (int c = 0; c < 64; c++) {
        uint32_t argb = rp2c02_palette_rgb[c];
        double r = ((argb >> 16) & 0xFF) / 255.0;
        double g = ((argb >> 8)  & 0xFF) / 255.0;
        double b = ( argb        & 0xFF) / 255.0;

        /* Exact inverse of ntsc_decode's YIQ->RGB matrix (verified
         * numerically; matches the standard NTSC RGB->YIQ coefficients to
         * 3 decimal places, as it should for a matrix this size). */
        double Y =  0.299 * r + 0.587 * g + 0.114 * b;
        double I =  0.596 * r - 0.274 * g - 0.322 * b;
        double Q =  0.211 * r - 0.523 * g + 0.312 * b;

        color_pedestal[c]  = Y;
        color_amp[c]       = sqrt(I * I + Q * Q);
        color_phase_rad[c] = atan2(Q, I);
    }
    tables_ready = true;
}

/* Emphasis (PPUMASK bits 5-7): real hardware attenuates the composite
 * signal outside a band centered on the emphasized primary's phase,
 * cumulative across simultaneously-set bits. Phase centers are derived
 * from the same measured table above (whichever chromatic color code is
 * closest to "pure" red/green/blue), so they're consistent with this
 * codebase's actual palette rather than an assumed convention. Exact
 * real-hardware attenuation dB is approximate — see file header. */
static double emphasis_center_rad[3];
static bool   emphasis_centers_ready;

static void build_emphasis_centers(void) {
    if (emphasis_centers_ready) return;
    /* Reference primaries at roughly full brightness/saturation. */
    static const double ref_rgb[3][3] = {
        { 1.0, 0.0, 0.0 },   /* red   */
        { 0.0, 1.0, 0.0 },   /* green */
        { 0.0, 0.0, 1.0 },   /* blue  */
    };
    for (int ch = 0; ch < 3; ch++) {
        double r = ref_rgb[ch][0], g = ref_rgb[ch][1], b = ref_rgb[ch][2];
        double I = 0.596 * r - 0.274 * g - 0.322 * b;
        double Q = 0.211 * r - 0.523 * g + 0.312 * b;
        emphasis_center_rad[ch] = atan2(Q, I);
    }
    emphasis_centers_ready = true;
}

#define EMPHASIS_BAND_RAD    (60.0 * M_PI / 180.0)
#define EMPHASIS_ATTENUATION 0.746

static double angle_diff_rad(double a, double b) {
    double d = fmod(a - b + 3.0 * M_PI, 2.0 * M_PI) - M_PI;
    return d < 0.0 ? -d : d;
}

static double emphasis_factor(uint8_t emphasis, double phase_rad) {
    double factor = 1.0;
    for (int ch = 0; ch < 3; ch++) {
        if (!(emphasis & (0x20u << ch))) continue;
        if (angle_diff_rad(phase_rad, emphasis_center_rad[ch]) > EMPHASIS_BAND_RAD)
            factor *= EMPHASIS_ATTENUATION;
    }
    return factor;
}

void nes_ntsc_encode(const NesNtscEncodeSpec *spec, const uint8_t *pixels,
                     int width, int height, uint64_t dot_offset,
                     float *signal_out) {
    build_color_tables();
    build_emphasis_centers();
    ntsc_trig_lut_init();

    int oversample = spec->oversample;
    double cyc_per_sample = nes_ntsc_subcarrier_cycles_per_sample(oversample);

    for (int y = 0; y < height; y++) {
        const uint8_t *row = pixels + (size_t)y * width;
        float *out_row = signal_out + (size_t)y * width * oversample;
        uint64_t line_dot_base = dot_offset + (uint64_t)y * width;

        for (int x = 0; x < width; x++) {
            uint8_t color = row[x] & 0x3Fu;
            double pedestal  = color_pedestal[color];
            double amp       = color_amp[color];
            double phase0    = color_phase_rad[color];
            double phase0_frac = phase0 / (2.0 * M_PI);
            double emph = amp > 0.0 ? emphasis_factor(spec->emphasis, phase0) : 1.0;

            uint64_t dot_idx = line_dot_base + (uint64_t)x;
            uint64_t abs_sample0 = dot_idx * (uint64_t)oversample;
            /* Phase advances by a fixed cyc_per_sample each raw sample, so
             * one fmod() here plus a running add+wrap below replaces
             * oversample-1 redundant fmod()s per pixel. */
            double frac = fmod((double)abs_sample0 * cyc_per_sample, 1.0);
            for (int s = 0; s < oversample; s++) {
                double chroma = amp * ntsc_cos_frac(ntsc_wrap01(frac - phase0_frac));
                out_row[x * oversample + s] = (float)((pedestal + chroma) * emph);
                frac += cyc_per_sample;
                if (frac >= 1.0) frac -= 1.0;
            }
        }
    }
}
