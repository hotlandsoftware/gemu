#pragma once
/* The 8259A PIC as a core device (type "i8259"): see i8259.h for the chip.
 *
 * Lines: inputs ir0-ir7 (the IRQ pins), output int (INTR to the CPU, kept
 * up to date after every access, request and acknowledge).  Ports:
 * gemu_i8259_io, two bytes (A0 = the offset), ud = the device.  The CPU's
 * acknowledge cycle is gemu_i8259_ack(). */
#include "controller/i8259.h"
#include "gemu/dev/device.h"
#include "gemu/dev/space.h"

extern const GemuDeviceType gemu_i8259_type;
extern const GemuHandlers   gemu_i8259_io;

I8259  *gemu_i8259(GemuDevice *d);
uint8_t gemu_i8259_ack(GemuDevice *d);          /* INTA: the vector */
