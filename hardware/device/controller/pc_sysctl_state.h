#pragma once
/* The system control port's save state (gemu/dev/state.h).  prefix goes
 * before every item name ("" for none); false if any registration failed. */
#include "controller/pc_sysctl.h"
#include "gemu/dev/device.h"
#include <stdbool.h>

bool pc_sysctl_state_add(GemuDevice *d, const char *prefix, PcSysctl *x);
