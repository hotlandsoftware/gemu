#pragma once

/*
 * Save-state registration for the Merced core (merced_state.c): every piece
 * of architectural and emulation state in struct Merced, including the
 * cache-array model, as named items of a core device (gemu/dev/state.h).
 */

#include <stdbool.h>
#include "gemu/dev/device.h"

typedef struct Merced Merced;

/* Registers m's state on d (call from d's init hook).  True only if every
 * item was registered. */
bool merced_state_add(GemuDevice *d, Merced *m);
