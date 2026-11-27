#pragma once
/* The 8259A's save state (gemu/dev/state.h), for any device that holds one
 * (i8259_dev.c's, a chipset's pair).  prefix goes before every item name
 * ("" for none); false if any registration failed. */
#include "controller/i8259.h"
#include "gemu/dev/device.h"
#include <stdbool.h>

bool i8259_state_add(GemuDevice *d, const char *prefix, I8259 *c);
/* The pair: the master's items under prefix + "master.", the slave's under
 * prefix + "slave.". */
bool i8259_pair_state_add(GemuDevice *d, const char *prefix, I8259Pair *p);
