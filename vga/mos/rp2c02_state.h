#pragma once
/* The 2C02 PPU's save state (gemu/dev/state.h), for any device that holds
 * one.  prefix goes before every item name ("" for none); false if any
 * registration failed. */
#include "rp2c02.h"
#include "gemu/dev/device.h"
#include <stdbool.h>

bool rp2c02_state_add(GemuDevice *d, const char *prefix, Rp2c02 *x);
