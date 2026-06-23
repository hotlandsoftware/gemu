#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "gemu/display.h"   /* for GemuDisplayType */
#include "gemu/serial.h"    /* for GemuSerial */

typedef struct Vt100State Vt100State;

Vt100State *vt100_create(GemuDisplayType dtype, const char *title);
void        vt100_destroy(Vt100State *t);
void        vt100_write(Vt100State *t, uint8_t ch);   /* host → terminal */
uint8_t     vt100_read_key(Vt100State *t);             /* returns 0 if no key */
bool        vt100_key_available(Vt100State *t);
void        vt100_poll(Vt100State *t);                 /* pump SDL events + blink */
bool        vt100_should_quit(Vt100State *t);
uint32_t    vt100_pop_fwd_key(Vt100State *t);  /* chars intercepted from host window */

/* Fill *out with callbacks wired to this terminal instance. */
void        vt100_as_serial(Vt100State *t, GemuSerial *out);
