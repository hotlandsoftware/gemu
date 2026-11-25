#pragma once
/* The INS8250 UART as a core device (type "ins8250"): see ins8250.h.
 *
 * Options: model (8250 or 16450), clock - the rate gemu_ins8250_tick()
 * counts in Hz (0: one tick per character, see ins8250.h).  Lines: output
 * int (through OUT2, as the PC adapter routes it).  Ports: gemu_ins8250_io,
 * eight bytes, ud = the device.  What's on the other end of the cable is
 * set through gemu_ins8250(d) (dev_tx/dev_lines/dev_rx/dev, modem_in). */
#include "serial/ins8250.h"
#include "gemu/dev/device.h"
#include "gemu/dev/space.h"

extern const GemuDeviceType gemu_ins8250_type;
extern const GemuHandlers   gemu_ins8250_io;

Ins8250 *gemu_ins8250(GemuDevice *d);
void     gemu_ins8250_tick(GemuDevice *d, uint32_t clocks);
