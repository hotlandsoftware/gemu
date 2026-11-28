#pragma once
/* ANTIC and GTIA's save state (gemu/dev/state.h): their registers, the
 * scanline and the frame (as colour codes; the ARGB frame is worked out
 * again with antic_refresh_argb()).  prefix goes before every item name
 * ("" for none); false if any registration failed. */
#include "antic.h"
#include "gemu/dev/device.h"
#include <stdbool.h>

bool antic_state_add(GemuDevice *d, const char *prefix, Antic *x);
