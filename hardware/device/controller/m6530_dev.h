#pragma once
/*
 * The 6530 RRIOT as a core device (stage M1): it owns its M6530 (I/O,
 * timer, 64 bytes of RAM; the mask ROM is the machine's) and registers its
 * state.  Line: output "irq" (active high: the timer's flag when loaded
 * through C-F).  The timer counts the CPU's cycles: the machine gives it
 * their count (gemu_m6530_set_cycles) and wires the ports
 * (gemu_m6530(d)->read_pa ... and ud).  It maps gemu_m6530_io (16
 * registers, A3-A0 = the offset's low bits; ud = the device) and the RAM
 * (gemu_m6530(d)->ram) where its board decodes them.  A reset is the
 * chip's (RAM kept).
 */
#include "controller/m6530.h"
#include "gemu/dev/device.h"
#include "gemu/dev/space.h"

extern const GemuDeviceType gemu_m6530_type;
extern const GemuHandlers   gemu_m6530_io;

M6530 *gemu_m6530(GemuDevice *d);
void   gemu_m6530_set_cycles(GemuDevice *d, uint64_t (*cycles)(void *ud), void *ud);
/* The timer's underflow since the last access, on the irq line (the
 * machine calls it after each instruction, as m6530_tick). */
void   gemu_m6530_tick(GemuDevice *d);
