#pragma once
/* The CDP1802's save state (gemu/dev/state.h): its registers, the machine
 * cycle it is in, the pending requests and the DMA queue's position.  The
 * queue's entries are callbacks, not saved: the cdp1802 device refills them
 * from its DMA target after a load (cdp1802_dev.h).  prefix goes before
 * every item name ("" for none); false if any registration failed. */
#include "cdp1802.h"
#include "gemu/dev/device.h"
#include <stdbool.h>

bool cdp1802_state_add(GemuDevice *d, const char *prefix, Cdp1802 *x);
