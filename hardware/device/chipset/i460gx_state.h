#pragma once

/* Save-state registration for the 460GX chipset model (state.h). Every item
 * name is `prefix` followed by the member's name ("" is fine). The flash
 * image behind the firmware hubs belongs to the board and is not registered
 * here. True only if every registration succeeded. */

#include "gemu/dev/state.h"
#include "i460gx.h"

#include <stdbool.h>

bool i460gx_state_add(GemuDevice *d, const char *prefix, I460GX *s);
