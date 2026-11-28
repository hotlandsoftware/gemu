#pragma once
/*
 * The plain Intel 8255 PPI as a core device (stage Z1): it owns its
 * Ppi8255 and registers its state (the control word, the output
 * latches).  gemu_ppi8255_bus is its four registers (offset bits 0-1 =
 * A0-A1; ud = the device).  The machine sets the pins (gemu_ppi8255(d)
 * ->read_pins, write_pins, ud).  (The IBM 5150's 8255, wired to its
 * motherboard, is the separate i8255-pc.)
 */
#include "controller/ppi8255.h"
#include "gemu/dev/device.h"
#include "gemu/dev/space.h"

extern const GemuDeviceType gemu_ppi8255_type;
extern const GemuHandlers   gemu_ppi8255_bus;

Ppi8255 *gemu_ppi8255(GemuDevice *d);
