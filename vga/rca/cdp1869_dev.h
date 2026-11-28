#pragma once
/*
 * The CDP1869/CDP1870 (VIS) as a core device (stage R1): it owns its
 * Cdp1869 and registers its state (page, character and PCB RAM, the
 * registers, the beam).  The machine sets its geometry after adding it
 * (cdp1869_set_page_ram_mask ...), maps its RAM and ports as before, and
 * renders the bitmap (cdp1869_render).
 */
#include "cdp1869.h"
#include "gemu/dev/device.h"

extern const GemuDeviceType gemu_cdp1869_type;

Cdp1869 *gemu_cdp1869(GemuDevice *d);
