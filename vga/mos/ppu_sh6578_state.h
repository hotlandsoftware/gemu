#pragma once
/* The SH6578 PPU's save state (gemu/dev/state.h): VRAM, OAM, palette, the
 * scroll and register state, the beam, and the frame (indices and ARGB).
 * prefix goes before every item name ("" for none); false if any
 * registration failed. */
#include "ppu_sh6578.h"
#include "gemu/dev/device.h"
#include <stdbool.h>

bool ppu_sh6578_state_add(GemuDevice *d, const char *prefix, Sh6578Ppu *x);
