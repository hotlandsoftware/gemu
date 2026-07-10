#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Generic NTSC composite decoder — chip-agnostic.
 *
 * Takes an oversampled composite luma+chroma signal (one array of
 * colorburst-locked samples per scanline) and decodes it into ARGB8888.
 * Two modes, matching two real TV design tiers:
 *
 *   comb_filter = false: a simple single-line synchronous decoder (the
 *   kind a cheap real TV used) — low-pass for luma, synchronous I/Q
 *   demodulation against the known subcarrier phase for chroma, then the
 *   standard YIQ→RGB matrix. This is what produces the frame-to-frame dot
 *   crawl cheap TVs are known for: luma and chroma share signal bandwidth
 *   within one line, so a purely intra-line decode can't fully separate
 *   them at sharp transitions.
 *
 *   comb_filter = true: a 3-line comb filter (see the .c file for why 3
 *   lines and not the classic broadcast-TV 2-line design) — pools raw
 *   chroma samples from the scanline above, the scanline itself, and the
 *   one below into one joint demodulation, using each sample's own exact
 *   subcarrier phase. Luma is still decoded from the current scanline so
 *   high-contrast text and sprite edges keep a sharper core while chroma
 *   dot crawl and cross-color fringing are reduced.
 *
 * Any chip that can produce a colorburst-referenced composite signal (see
 * crt/nes_ntsc_encode.h for the NES's encoder) can target this decoder —
 * it has no chip-specific knowledge.
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
    bool   comb_filter;                    /* false = single-line ("cheap"),
                                             * true = 3-line comb ("nicer TV") */
} NtscDecodeSpec;

/* signal: samples_per_line * lines samples, scanlines back to back, each a
 * normalized composite amplitude (roughly 0..1.3 — see the encoder for the
 * exact convention both sides must agree on).
 * out: out_width * lines ARGB8888 pixels, caller-allocated. */
void ntsc_decode(const NtscDecodeSpec *spec, const float *signal, uint32_t *out);
