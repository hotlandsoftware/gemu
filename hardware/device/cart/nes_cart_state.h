#pragma once
/* The NES cartridge's save state (gemu/dev/state.h): which cartridge it is
 * (a state from a different one is refused), its CHR RAM, PRG-RAM, the
 * banking windows and the board's registers.  prefix goes before every
 * item name ("" for none); false if any registration failed. */
#include "cart/nes_cart.h"
#include "gemu/dev/device.h"
#include <stdbool.h>

bool nes_cart_state_add(GemuDevice *d, const char *prefix, NesCart *x);
