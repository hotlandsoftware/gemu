#pragma once
/* A ClockDiv's save state (gemu/dev/state.h).  prefix goes before every item
 * name ("" for none); false if any registration failed. */
#include "gemu/dev/device.h"
#include "timer/clock_div.h"
#include <stdbool.h>

bool clock_div_state_add(GemuDevice *d, const char *prefix, ClockDiv *x);
