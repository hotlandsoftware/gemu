#pragma once
/* The 6821 PIA's save state (gemu/dev/state.h): its registers and control
 * lines.  prefix goes before every item name ("" for none); false if any
 * registration failed. */
#include "controller/pia6821.h"
#include "gemu/dev/device.h"
#include <stdbool.h>

bool pia6821_state_add(GemuDevice *d, const char *prefix, Pia6821 *x);
