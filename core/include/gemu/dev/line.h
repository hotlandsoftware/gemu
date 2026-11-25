/*
 * Lines: interrupts, DMA requests, resets, chip selects - any 1-bit signal.
 * (.work/modular-devices, 2.4)
 *
 * A device has named outputs (GemuLine) and named inputs (GemuLineIn, with
 * a callback).  Connections go from an output to an input:
 *   - an output can drive any number of inputs (fan-out);
 *   - a connection can invert;
 *   - an input driven by several outputs combines them: wired-OR by default
 *     (any driver asserted asserts it - shared ISA lines, PCI INTx counted
 *     as "asserted"), or wired-AND; with no drivers it sits at its default
 *     level (false unless set).
 * An input's callback runs when its combined level changes, synchronously,
 * from inside gemu_line_set().  Before the machine starts, levels are kept
 * but no callbacks run: a device reads its inputs' levels in reset.  A
 * callback may set lines (even the one that called it) but not connect or
 * disconnect them.
 *
 * Levels are logical (true = asserted); an active-low pin is an inverted
 * connection or a device that says so.  Destroying a device removes its
 * lines and their connections (inputs left undriven go to their default).
 */
#pragma once
#include <stdbool.h>
#include "gemu/dev/device.h"

typedef struct GemuLine   GemuLine;     /* an output */
typedef struct GemuLineIn GemuLineIn;   /* an input */

enum {
    GEMU_LINE_INVERT = 1 << 0,   /* connection: the input sees the opposite */
};

typedef enum {
    GEMU_LINE_WIRED_OR,          /* asserted if any driver is (the default) */
    GEMU_LINE_WIRED_AND,         /* asserted only if every driver is */
} GemuLineMode;

/* Names are unique per device and direction; NULL on a duplicate. */
GemuLine   *gemu_line_out(GemuDevice *d, const char *name);
GemuLineIn *gemu_line_in(GemuDevice *d, const char *name,
                         void (*cb)(void *ud, int idx, bool level), void *ud, int idx);
void gemu_line_in_set_mode(GemuLineIn *in, GemuLineMode mode);
void gemu_line_in_set_default(GemuLineIn *in, bool level);

GemuLine   *gemu_line_find_out(GemuDevice *d, const char *name);
GemuLineIn *gemu_line_find_in(GemuDevice *d, const char *name);

/* Connect out to an input (by pointer, or by name on device `to`). */
bool gemu_line_connect_in(GemuLine *out, GemuLineIn *in, unsigned flags);
bool gemu_line_connect(GemuLine *out, GemuDevice *to, const char *in_name, unsigned flags);
/* By path: "board/pit:out0" -> "board/pic:ir0".  Errors (no such device or
 * line) return false with a message in gemu_machine_error(). */
bool gemu_lines_connect(GemuMachine *m, const char *out_path, const char *in_path,
                        unsigned flags);
/* Remove every connection from out to in; returns how many. */
int  gemu_line_disconnect(GemuLine *out, GemuLineIn *in);

void gemu_line_set(GemuLine *l, bool level);
void gemu_line_pulse(GemuLine *l);              /* assert, then release */
bool gemu_line_get(const GemuLine *l);          /* the output's own level */
bool gemu_line_in_level(const GemuLineIn *in);  /* the combined input level */
unsigned gemu_line_in_drivers(const GemuLineIn *in);
const char *gemu_line_name(const GemuLine *l);
const char *gemu_line_in_name(const GemuLineIn *in);
