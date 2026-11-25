#pragma once
/* The 8253 PIT as a core device (type "i8253"): see i8253.h for the chip.
 *
 * Lines: inputs gate0-gate2 (undriven: high, as on the PC's channels 0 and
 * 1), outputs out0-out2.  Ports: gemu_i8253_io, four bytes, ud = the device.
 * The machine clocks it with gemu_i8253_tick() (the scheduler takes that
 * over in stage D3). */
#include "timer/i8253.h"
#include "gemu/dev/device.h"
#include "gemu/dev/space.h"

extern const GemuDeviceType gemu_i8253_type;
extern const GemuHandlers   gemu_i8253_io;

I8253 *gemu_i8253(GemuDevice *d);
void   gemu_i8253_tick(GemuDevice *d, uint32_t ticks);
