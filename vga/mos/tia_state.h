#pragma once
/* The TIA's save state (gemu/dev/state.h): registers, objects, the delayed
 * writes, both frame buffers, the frame statistics and the sound channels.
 * prefix goes before every item name ("" for none); false if any
 * registration failed. */
#include "tia.h"
#include "gemu/dev/device.h"
#include <stdbool.h>

bool tia_state_add(GemuDevice *d, const char *prefix, Tia *x);
