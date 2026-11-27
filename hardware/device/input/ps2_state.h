#pragma once
/* Save state (gemu/dev/state.h) for the PS/2 models: the 8042 controller,
 * the keyboard and the mouse.  prefix goes before every item name ("" for
 * none); false if any registration failed.  The 8042 doesn't register its
 * keyboard and mouse: register each with its own function (and prefix). */
#include "gemu/dev/device.h"
#include "input/i8042.h"
#include "input/ps2_keyboard.h"
#include "input/ps2_mouse.h"
#include <stdbool.h>

bool i8042_state_add(GemuDevice *d, const char *prefix, I8042 *x);
bool ps2_keyboard_state_add(GemuDevice *d, const char *prefix, Ps2Keyboard *x);
bool ps2_mouse_state_add(GemuDevice *d, const char *prefix, Ps2Mouse *x);
