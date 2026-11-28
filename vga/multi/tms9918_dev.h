#pragma once
/*
 * The TMS9918/9928/9128 VDP as a core device (stage Z1): it owns its
 * Tms9918 and registers its state.  gemu_tms9918_io is its two ports
 * (offset bit 0 = the MODE pin: 0 data, 1 control/status; ud = the
 * device).  The machine clocks it a scanline at a time
 * (tms9918_scanline) from its own beam, and reads int_out.
 * Not saved: the framebuffer (fb), redrawn by the next frame.
 */
#include "tms9918.h"
#include "gemu/dev/device.h"
#include "gemu/dev/space.h"

extern const GemuDeviceType gemu_tms9918_type;
extern const GemuHandlers   gemu_tms9918_io;

Tms9918 *gemu_tms9918(GemuDevice *d);
