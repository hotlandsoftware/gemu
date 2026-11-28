#pragma once
/* POKEY's save state (gemu/dev/state.h): audio registers (stored, not
 * played), IRQs, keyboard, serial and timers, the random generator.
 * prefix goes before every item name ("" for none); false if any
 * registration failed. */
#include "pokey.h"
#include "gemu/dev/device.h"
#include <stdbool.h>

bool pokey_state_add(GemuDevice *d, const char *prefix, Pokey *x);
