#pragma once
/*
 * The CDP1861 as a core device (stage R1): it owns its Cdp1861 and
 * registers its state.  The machine sets its DMA-out callback
 * (gemu_cdp1861_set_dma) and calls cdp1861_sync(gemu_cdp1861(d), cpu)
 * every machine cycle, as before; its interrupt and DMA requests go to the
 * CPU through the 1802's own handshake.
 */
#include "cdp1861.h"
#include "gemu/dev/device.h"

extern const GemuDeviceType gemu_cdp1861_type;

Cdp1861 *gemu_cdp1861(GemuDevice *d);
void     gemu_cdp1861_set_dma(GemuDevice *d, void (*cb)(uint8_t *byte, void *ud), void *ud);
