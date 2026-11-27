#pragma once
/* The 8253/8254's save state (gemu/dev/state.h), for any device that holds
 * one (i8253_dev.c's, a chipset's).  prefix goes before every item name ("" for
 * none); false if any registration failed. */
#include "gemu/dev/device.h"
#include "timer/i8253.h"
#include <stdbool.h>

bool i8253_state_add(GemuDevice *d, const char *prefix, I8253 *p);
