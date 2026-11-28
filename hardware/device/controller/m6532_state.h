#pragma once
/* The 6532 RIOT's save state (gemu/dev/state.h): its RAM, ports and timer.
 * prefix goes before every item name ("" for none); false if any
 * registration failed. */
#include "controller/m6532.h"
#include "gemu/dev/device.h"
#include <stdbool.h>

bool m6532_state_add(GemuDevice *d, const char *prefix, M6532 *x);
