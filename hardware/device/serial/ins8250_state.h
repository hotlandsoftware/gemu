#pragma once
/* The INS8250's save state (gemu/dev/state.h), for any device that holds
 * one (ins8250_dev.c's, a chipset's).  prefix goes before every item name
 * ("" for none); false if any registration failed. */
#include "gemu/dev/device.h"
#include "serial/ins8250.h"
#include <stdbool.h>

bool ins8250_state_add(GemuDevice *d, const char *prefix, Ins8250 *u);
