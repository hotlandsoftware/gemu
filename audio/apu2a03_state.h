#pragma once
/* The 2A03 APU's save state (gemu/dev/state.h), for any device that holds
 * one.  prefix goes before every item name ("" for none); false if any
 * registration failed. */
#include "apu2a03.h"
#include "gemu/dev/device.h"
#include <stdbool.h>

bool apu2a03_state_add(GemuDevice *d, const char *prefix, Apu2a03 *x);
