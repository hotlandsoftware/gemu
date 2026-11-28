#pragma once
/* The Z80's save state (stage Z1): the registers, both sets, the interrupt
 * state and the lines, the T-state count, as GEMU_STATE_MEMBER items. */
#include "z80.h"
#include "gemu/dev/device.h"
#include <stdbool.h>

bool z80_state_add(GemuDevice *d, const char *prefix, Z80 *x);
