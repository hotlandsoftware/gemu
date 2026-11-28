#pragma once
/* The OKI MSM6295's save state (gemu/dev/state.h): its four channels, the
 * command in progress and the resampler.  prefix goes before every item
 * name ("" for none); false if any registration failed. */
#include "okim6295.h"
#include "gemu/dev/device.h"
#include <stdbool.h>

bool okim6295_state_add(GemuDevice *d, const char *prefix, Okim6295 *x);
