#pragma once
/* The PC diskette adapter's uPD765 as a core device (type "upd765"): see
 * upd765.h for the chip.
 *
 * Lines: output int (IRQ 6 on the PC).  Ports: gemu_upd765_io, eight bytes
 * from 3F0h (DOR at +2, status +4, data +5), ud = the device.  The machine
 * gives it drives (gemu_upd765(d)->drive[]) and a DMA channel
 * (gemu_upd765_set_dma, an i8237 device), and clocks it with
 * gemu_upd765_tick() in CPU clocks. */
#include "storage/upd765.h"
#include "gemu/dev/device.h"
#include "gemu/dev/space.h"

extern const GemuDeviceType gemu_upd765_type;
extern const GemuHandlers   gemu_upd765_io;

Upd765 *gemu_upd765(GemuDevice *d);
void    gemu_upd765_set_dma(GemuDevice *d, GemuDevice *i8237, unsigned channel);
void    gemu_upd765_tick(GemuDevice *d, uint32_t cpu_clocks);
