#pragma once
/* The MC146818-style RTC's save state (gemu/dev/state.h).  prefix goes before
 * every item name ("" for none); false if any registration failed. */
#include "gemu/dev/device.h"
#include "timer/mc146818.h"
#include <stdbool.h>

bool mc146818_state_add(GemuDevice *d, const char *prefix, Mc146818 *x);
