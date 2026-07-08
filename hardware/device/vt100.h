#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "gemu/display.h"   /* for GemuDisplayType */
#include "gemu/serial.h"    /* for GemuSerial */

typedef struct Vt100State Vt100State;

Vt100State *vt100_create(GemuDisplayType dtype, const char *title);
void        vt100_destroy(Vt100State *t);

/* Fill *out with callbacks wired to this terminal instance. */
void        vt100_as_serial(Vt100State *t, GemuSerial *out);
