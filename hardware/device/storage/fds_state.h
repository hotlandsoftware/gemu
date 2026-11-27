#pragma once
/* The Famicom Disk System's save state (gemu/dev/state.h): the RAM
 * adapter's registers, work RAM and sound, and the disk in the drive.
 * prefix goes before every item name ("" for none); false if any
 * registration failed. */
#include "storage/fds.h"
#include "gemu/dev/device.h"
#include <stdbool.h>

bool fds_state_add(GemuDevice *d, const char *prefix, FdsState *x);
