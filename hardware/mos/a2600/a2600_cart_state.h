#pragma once
/* The 2600 cartridge's save state (gemu/dev/state.h): which cartridge it
 * is (a state from another scheme or size is refused), the bank registers,
 * the cartridge's RAM, and the DPC chip's and the Supercharger's state.
 * prefix goes before every item name ("" for none); false if any
 * registration failed. */
#include "a2600_cart.h"
#include "gemu/dev/device.h"
#include <stdbool.h>

bool a26_cart_state_add(GemuDevice *d, const char *prefix, A2600Cart *x);
