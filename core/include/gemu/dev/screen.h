/*
 * Screens: what a video device shows.  (.work/modular-devices, 2.5)
 *
 * A screen belongs to a device and has a bitmap (ARGB 0xFFRRGGBB with a
 * stride - MAME's bitmap_rgb32 and 86Box's buffer32 are the same shape)
 * filled by the device's update callback.  Front ends show screens as the
 * heads of a GemuDisplay; gemu_screen_render() gives them a finished frame.
 *
 * Timing, one of two ways:
 *   - self-timed (MAME's set_raw): give the pixel clock and the totals; the
 *     scheduler starts each frame and drives the vblank line (high from line
 *     vbstart until line vbend of the next frame; with vbend 0, until the
 *     frame starts), exactly, from timers;
 *   - device-timed: a device that counts its own beam calls
 *     gemu_screen_frame_begin() at its frame boundary.
 * Either way gemu_screen_vpos()/hpos() give the beam position at
 * gemu_now() - in raw timing's pixels and lines (set_raw, which a
 * device-timed screen may also call to describe itself).
 *
 * Rendering: gemu_screen_update_partial(s, line) draws lines up to `line`
 * now, for effects that change mid-frame; they are kept until the next
 * frame begins.  gemu_screen_render() draws every line not drawn that way
 * from the device's present state and returns the bitmap - so a device
 * that never draws partially is simply rendered whole, on demand.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "gemu/dev/device.h"
#include "gemu/dev/scheduler.h"

typedef struct GemuLine   GemuLine;
typedef struct GemuScreen GemuScreen;

typedef struct {
    uint32_t *pix;           /* 0xFFRRGGBB */
    int       width, height;
    int       stride;        /* pixels from one row to the next */
} GemuBitmap;

typedef struct { int x0, y0, x1, y1; } GemuRect;   /* x1, y1 exclusive */

/* name: unique per device ("screen" usually).  The device's vblank output
 * line is "<name>-vblank".  NULL on a bad or duplicate name. */
GemuScreen *gemu_screen_new(GemuDevice *d, const char *name);
const char *gemu_screen_name(const GemuScreen *s);
GemuDevice *gemu_screen_device(const GemuScreen *s);

/* The bitmap's size (cleared, black) and the shape it is shown at: aspect
 * w:h of the whole picture, 0:0 for square pixels. */
bool gemu_screen_set_size(GemuScreen *s, int width, int height);
void gemu_screen_set_aspect(GemuScreen *s, int aspect_w, int aspect_h);
void gemu_screen_get_aspect(const GemuScreen *s, int *aspect_w, int *aspect_h);

/* Raw timing.  Self-timed screens: pass self_timed true (frames start
 * one frame period from now).  Lines and pixels count from the frame start. */
bool gemu_screen_set_raw(GemuScreen *s, uint64_t pixel_hz, int htotal, int hbend, int hbstart,
                         int vtotal, int vbend, int vbstart, bool self_timed);

void gemu_screen_set_update(GemuScreen *s,
        void (*update)(void *ud, GemuBitmap *bmp, const GemuRect *clip), void *ud);

/* Device-timed screens: a frame begins now. */
void gemu_screen_frame_begin(GemuScreen *s);
/* ... and their vblank line, if the device knows it. */
void gemu_screen_set_vblank(GemuScreen *s, bool level);

uint64_t gemu_screen_frame_number(const GemuScreen *s);
GemuTime gemu_screen_frame_start(const GemuScreen *s);
/* The beam at gemu_now(): line and pixel within the frame (needs raw
 * timing; 0 without). */
int  gemu_screen_vpos(GemuScreen *s);
int  gemu_screen_hpos(GemuScreen *s);
GemuLine *gemu_screen_vblank(GemuScreen *s);

void gemu_screen_update_partial(GemuScreen *s, int line);   /* lines up to and including */
const GemuBitmap *gemu_screen_render(GemuScreen *s);

/* A machine's screens, in the order they were made. */
GemuScreen *gemu_screen_first(GemuMachine *m);
GemuScreen *gemu_screen_next(GemuScreen *s);
