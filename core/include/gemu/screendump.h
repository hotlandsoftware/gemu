#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * Screendump helpers.
 *
 * gemu_screendump() takes a packed 24-bit RGB buffer (3 bytes per pixel,
 * row-major, top-to-bottom) and dispatches on the file extension:
 *   *.png  → PNG (uncompressed deflate — valid but larger than gzip)
 *   anything else → PPM (P6 binary, trivial to open everywhere)
 *
 * The _argb and _mono variants convert common emulator framebuffer formats
 * (0xAARRGGBB words / zero-vs-nonzero bytes) before dispatching.
 */
bool gemu_screendump     (const char *path, const uint8_t  *rgb,    int w, int h);
bool gemu_screendump_argb(const char *path, const uint32_t *argb,   int w, int h);
bool gemu_screendump_mono(const char *path, const uint8_t  *pixels, int w, int h);
