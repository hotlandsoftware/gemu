#pragma once
/* The IBM Fixed Disk Adapter as a core device (type "ibm-fixed-disk", the
 * -device of the same name): see ibm_hdc.h for the card.
 *
 * Options: romfile, type0, type1, interleave, irq, rom - the card's
 * -device options; until the 5150 is built on the core (stage D2) the
 * machine still reads them, loads the ROM and attaches the drives.
 * Lines: output int.  Ports: gemu_ibm_hdc_io, four bytes from 320h, ud =
 * the device.  The machine sets the wiring (gemu_ibm_hdc(d): drives,
 * geometry, jumpers, clock) before the machine starts, gives it a DMA
 * channel (gemu_ibm_hdc_set_dma) and clocks it with gemu_ibm_hdc_tick(). */
#include "storage/ibm_hdc.h"
#include "gemu/dev/device.h"
#include "gemu/dev/space.h"

extern const GemuDeviceType gemu_ibm_hdc_type;
extern const GemuHandlers   gemu_ibm_hdc_io;

IbmHdc *gemu_ibm_hdc(GemuDevice *d);
void    gemu_ibm_hdc_set_dma(GemuDevice *d, GemuDevice *i8237, unsigned channel);
void    gemu_ibm_hdc_tick(GemuDevice *d, uint32_t clocks);
