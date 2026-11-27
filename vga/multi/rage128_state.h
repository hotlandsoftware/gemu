#pragma once

/* Save-state registration for the Rage 128 and its VGA core (state.h).
 * Every item name is `prefix` followed by the member's name ("" is fine).
 * The VRAM buffer (vga->vram) is not registered here: the machine owns it
 * and registers it.  True only if every registration succeeded. */

#include "gemu/dev/state.h"
#include "rage128.h"
#include "vga.h"

#include <stdbool.h>

bool rage128_state_add(GemuDevice *d, const char *prefix, Rage128 *r);
bool vga_state_add(GemuDevice *d, const char *prefix, Vga *v);
