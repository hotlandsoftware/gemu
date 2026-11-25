#pragma once
/* The 8237A DMA controller and the PC's page registers as a core device
 * (type "i8237"): see i8237.h for the chip.
 *
 * Lines: input dreq0 - a rising edge is one DRAM refresh request (PIT
 * channel 1 on the PC).  Ports: gemu_i8237_io (16 bytes, the controller)
 * and gemu_i8237_page_io (4 bytes at 80h-83h), ud = the device.
 * Transfers for channels 1-3 move a byte to or from the address space the
 * "space" option names (default "program"); a disk controller calls
 * gemu_i8237_to_mem / gemu_i8237_from_mem for each DMA cycle. */
#include "controller/i8237.h"
#include "gemu/dev/device.h"
#include "gemu/dev/space.h"

extern const GemuDeviceType gemu_i8237_type;
extern const GemuHandlers   gemu_i8237_io, gemu_i8237_page_io;

I8237 *gemu_i8237(GemuDevice *d);
/* One DMA cycle on channel ch: false if it isn't ready (masked, disabled,
 * no memory); *tc = this was the last byte. */
bool gemu_i8237_to_mem(GemuDevice *d, unsigned ch, uint8_t val, bool *tc);
bool gemu_i8237_from_mem(GemuDevice *d, unsigned ch, uint8_t *val, bool *tc);
