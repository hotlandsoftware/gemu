#include "crt_tube.h"
#include <math.h>
#include <stddef.h>

static const CrtTubeProfile generic_crt = {
    .name = "generic-crt",
    .scanline_strength = 0.24f,
    .mask_strength = 0.10f,
    .bloom_strength = 0.08f,
    .bloom_threshold = 0.70f,
    .gamma = 1.12f,
};

const CrtTubeProfile *crt_tube_profile_generic(void) {
    return &generic_crt;
}

static float clamp01(float v) {
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

static float srgb_to_light(uint8_t c) {
    float v = (float)c / 255.0f;
    return v * v;
}

static uint8_t light_to_srgb(float v, float gamma) {
    v = clamp01(v);
    if (gamma > 0.0f && gamma != 1.0f)
        v = powf(v, 1.0f / gamma);
    return (uint8_t)(v * 255.0f + 0.5f);
}

static float pixel_luma(uint32_t argb) {
    float r = srgb_to_light((uint8_t)(argb >> 16));
    float g = srgb_to_light((uint8_t)(argb >> 8));
    float b = srgb_to_light((uint8_t)argb);
    return 0.299f * r + 0.587f * g + 0.114f * b;
}

static float neighbor_bloom(const uint32_t *src, int width, int height, int x, int y,
                            float threshold) {
    static const int dx[4] = { -1, 1, 0, 0 };
    static const int dy[4] = { 0, 0, -1, 1 };
    float bloom = 0.0f;
    int n = 0;
    for (int i = 0; i < 4; i++) {
        int nx = x + dx[i], ny = y + dy[i];
        if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
        float l = pixel_luma(src[(size_t)ny * (size_t)width + (size_t)nx]);
        if (l > threshold)
            bloom += (l - threshold) / (1.0f - threshold);
        n++;
    }
    return n ? bloom / (float)n : 0.0f;
}

void crt_tube_apply(const CrtTubeProfile *profile, const uint32_t *src,
                    uint32_t *dst, int width, int height) {
    if (!profile || !src || !dst || width <= 0 || height <= 0) return;

    for (int y = 0; y < height; y++) {
        /* Native 240p content lights one beam row at a time.  At native
         * output resolution this is necessarily stylized, but it gives the
         * integer-scaled image a visible aperture instead of a flat capture. */
        float scan = (y & 1) ? (1.0f - profile->scanline_strength) : 1.0f;

        for (int x = 0; x < width; x++) {
            uint32_t p = src[(size_t)y * (size_t)width + (size_t)x];
            float r = srgb_to_light((uint8_t)(p >> 16));
            float g = srgb_to_light((uint8_t)(p >> 8));
            float b = srgb_to_light((uint8_t)p);

            float bloom = neighbor_bloom(src, width, height, x, y,
                                         profile->bloom_threshold) *
                          profile->bloom_strength;
            r += bloom;
            g += bloom;
            b += bloom;

            r *= scan;
            g *= scan;
            b *= scan;

            /* Coarse RGB triad mask.  This is deliberately subtle because
             * the frame is still native-resolution; at scale 3 it becomes a
             * visible texture without dominating the composite artifacts. */
            float mask_dark = 1.0f - profile->mask_strength;
            switch (x % 3) {
            case 0: g *= mask_dark; b *= mask_dark; break;
            case 1: r *= mask_dark; b *= mask_dark; break;
            case 2: r *= mask_dark; g *= mask_dark; break;
            }

            uint8_t R = light_to_srgb(r, profile->gamma);
            uint8_t G = light_to_srgb(g, profile->gamma);
            uint8_t B = light_to_srgb(b, profile->gamma);
            dst[(size_t)y * (size_t)width + (size_t)x] =
                0xFF000000u | ((uint32_t)R << 16) | ((uint32_t)G << 8) | B;
        }
    }
}
