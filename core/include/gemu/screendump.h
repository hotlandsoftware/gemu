#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * Screendump helpers.
 *
 * gemu_screendump() takes a packed 24-bit RGB buffer (3 bytes per pixel,
 * row-major, top-to-bottom) and dispatches on the file extension:
 *   *.png  → PNG (uncompressed deflate - valid but larger than gzip)
 *   anything else → PPM (P6 binary, trivial to open everywhere)
 *
 * The _argb and _mono variants convert common emulator framebuffer formats
 * (0xAARRGGBB words / zero-vs-nonzero bytes) before dispatching.
 */
bool gemu_screendump     (const char *path, const uint8_t  *rgb,    int w, int h);
bool gemu_screendump_argb(const char *path, const uint32_t *argb,   int w, int h);
bool gemu_screendump_mono(const char *path, const uint8_t  *pixels, int w, int h);

/*
 * Live preview for the Machine Manager.  When gemu runs with
 * GEMU_PREVIEW_FD=N in its environment (a pipe the manager reads), the
 * monitor asks the machine's screendump callback for a frame a second after
 * start and every GEMU_PREVIEW_INTERVAL_MS after; gemu_screendump() sees
 * GEMU_PREVIEW_PATH instead of a file name and sends the frame, scaled to
 * fit GEMU_PREVIEW_MAX_W x _H, down the pipe - nothing touches the disk.
 * A writer thread does the I/O, so a slow reader never stalls the machine;
 * frames it has no time for are dropped.  Each frame is:
 *   "GEMUPRV1", uint32 width, uint32 height (little-endian), width*height*3 RGB
 */
#define GEMU_PREVIEW_PATH        "\001gemu-preview"
#define GEMU_PREVIEW_MAGIC       "GEMUPRV1"
#define GEMU_PREVIEW_MAX_W       480
#define GEMU_PREVIEW_MAX_H       360
#define GEMU_PREVIEW_INTERVAL_MS 5000

/* True (once set up) if this process has a preview pipe. */
bool gemu_preview_enabled(void);
/* Whether a preview frame is due now; the caller then captures one. */
bool gemu_preview_due(void);
