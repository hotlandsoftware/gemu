#pragma once
/* The 6530 RRIOT's save state (gemu/dev/state.h): its RAM, ports and
 * timer.  prefix goes before every item name ("" for none); false if any
 * registration failed. */
#include "controller/m6530.h"
#include "gemu/dev/device.h"
#include <stdbool.h>

bool m6530_state_add(GemuDevice *d, const char *prefix, M6530 *x);
