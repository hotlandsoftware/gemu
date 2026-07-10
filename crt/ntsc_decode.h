#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * Generic NTSC composite decoder — chip-agnostic.
 *
 * Takes an oversampled composite luma+chroma signal (one array of
 * colorburst-locked samples per scanline) and decodes it into ARGB8888
 * the same way a simple single-line synchronous NTSC decoder (the kind a
 * cheap real TV used, without a comb filter) would: a low-pass to recover
 * luma, synchronous I/Q demodulation against the known subcarrier phase
 * to recover chroma, then the standard YIQ→RGB matrix. Any chip that can
 * produce a colorburst-referenced composite signal (see
 * crt/nes_ntsc_encode.h for the NES's encoder) can target this decoder —
 * it has no chip-specific knowledge.
 *
 * Not doing a multi-line comb filter is a deliberate choice, not a
 * shortcut: single-line decode is what produces the frame-to-frame dot
 * crawl real cheap TVs are known for, which is the whole point here.
 *
 * NTSC only — PAL's line-alternating chroma phase needs a genuinely
 * different decode (not just different constants) and isn't implemented.
 */

typedef struct {
    int    samples_per_line;              /* raw signal samples per scanline */
    int    lines;
    double subcarrier_cycles_per_sample;   /* chroma subcarrier cycles advanced
                                             * per raw sample; phase is tracked
                                             * continuously across the whole
                                             * signal buffer (not reset per
                                             * line), which is what makes
                                             * dot crawl fall out naturally */
    int    out_width;                      /* decoded output width in pixels */
} NtscDecodeSpec;

/* signal: samples_per_line * lines samples, scanlines back to back, each a
 * normalized composite amplitude (roughly 0..1.3 — see the encoder for the
 * exact convention both sides must agree on).
 * out: out_width * lines ARGB8888 pixels, caller-allocated. */
void ntsc_decode(const NtscDecodeSpec *spec, const float *signal, uint32_t *out);
