#pragma once
/* The 6502's save state (gemu/dev/state.h), for any device that holds one.
 * prefix goes before every item name ("" for none); false if any
 * registration failed. */
#include "mos6502.h"
#include "gemu/dev/device.h"
#include <stdbool.h>

bool mos6502_state_add(GemuDevice *d, const char *prefix, Mos6502 *x);
