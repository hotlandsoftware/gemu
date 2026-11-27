#pragma once

/* Save-state registration for the 82559 Ethernet controller (state.h).
 * Every item name is `prefix` followed by the member's name ("" is fine).
 * The flash image is borrowed from the board and not registered here. True
 * only if every registration succeeded. */

#include "gemu/dev/state.h"
#include "e100.h"

#include <stdbool.h>

bool e100_state_add(GemuDevice *d, const char *prefix, E100 *n);
