#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * NES/RP2C02 composite (NTSC) encoder.
 *
 * Converts the PPU's raw per-dot hue+luma buffer (vga/mos/rp2c02.h's
 * Rp2c02.pixels[] — NOT pixels_argb, which is already lossy RGB) into an
 * oversampled composite signal ready for crt/ntsc_decode.h. Each pixels[]
 * byte is the same 6-bit value (hue in bits 0-3, luma tier in bits 4-5)
 * real RP2C02 hardware feeds its video DAC, so no PPU-internal surgery
 * was needed to get an encode-accurate source.
 *
 * Honors the NES's fixed dot-clock : colorburst ratio — 3 PPU dots per 2
 * colorburst cycles, derived from both clocks sharing one master
 * oscillator (NESdev "NTSC video") — with continuously-accumulating
 * chroma phase across the whole frame. That continuity, not a per-line
 * reset, is what produces authentic dot crawl.
 *
 * Calibration note: per-color pedestal/amplitude/phase are derived
 * directly from this codebase's own rp2c02_palette_rgb[] table (via
 * RGB->YIQ, the exact inverse of crt/ntsc_decode.c's YIQ->RGB) rather
 * than reconstructed from a hue-phase formula — an earlier attempt at
 * the latter produced colors essentially uncorrelated with the real
 * palette, since the formula's phase convention had no actual
 * relationship to how that table was generated. The emphasis-bit
 * attenuation *model* (band around each primary's phase, cumulative
 * across bits) follows the well-established community understanding of
 * RP2C02 behavior; its exact dB/degree window is a reasonable
 * reconstruction, not a transcription of measured hardware — see the .c
 * file.
 *
 * Known scope cut: phase only advances through each line's 256 *visible*
 * dots, not all 341 (real hardware's phase reference continues through
 * horizontal blanking too). This shifts the frame-to-frame phase
 * rotation rate slightly versus real hardware but still produces genuine
 * per-frame dot-crawl motion.
 */

#define NES_NTSC_OVERSAMPLE 8   /* raw signal samples per PPU dot */

typedef struct {
    int     oversample;   /* samples per dot; NES_NTSC_OVERSAMPLE by default */
    uint8_t emphasis;      /* PPUMASK bits 5-7 (0x20/0x40/0x80), sampled once
                             * per frame — see file header for why */
} NesNtscEncodeSpec;

/* pixels: width * height raw hue+luma values (0-63) — pass Rp2c02.pixels[].
 * signal_out: caller-allocated buffer of (width * spec->oversample) * height
 * floats.
 * dot_offset: running count of visible dots encoded so far (0 at the very
 * first frame); the caller should carry this forward across frames
 * (incrementing by width*height each call) so chroma phase — and the dot
 * crawl it produces — stays continuous. */
void nes_ntsc_encode(const NesNtscEncodeSpec *spec, const uint8_t *pixels,
                     int width, int height, uint64_t dot_offset,
                     float *signal_out);

/* The NTSC subcarrier phase advance per raw signal sample this encoder
 * uses, for the given oversample factor — pass straight through to
 * NtscDecodeSpec.subcarrier_cycles_per_sample so encode and decode never
 * drift out of sync on the 3:2 dot:colorburst ratio. */
static inline double nes_ntsc_subcarrier_cycles_per_sample(int oversample) {
    return 2.0 / (3.0 * (double)oversample);
}
