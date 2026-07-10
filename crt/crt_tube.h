#pragma once
#include <stdint.h>

/*
 * Generic CRT tube/display pass.
 *
 * This intentionally sits after signal decode (NTSC composite, later PAL/RF,
 * etc.) so display profiles can be reused across video standards.  The
 * profile describes the tube/display side: beam aperture, phosphor mask,
 * bloom, and light-response shaping.  Named real-set profiles can be added
 * later without changing machine code.
 */

typedef struct {
    const char *name;
    float scanline_strength;  /* 0..1 darkening between lit scanlines */
    float mask_strength;      /* 0..1 phosphor/shadow-mask tint amount */
    float bloom_strength;     /* small neighbor light bleed */
    float bloom_threshold;    /* source luma where bloom starts */
    float gamma;              /* output light response, 1.0 = linear */
} CrtTubeProfile;

const CrtTubeProfile *crt_tube_profile_generic(void);

void crt_tube_apply(const CrtTubeProfile *profile, const uint32_t *src,
                    uint32_t *dst, int width, int height);
