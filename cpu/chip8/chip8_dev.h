#pragma once
/*
 * The CHIP-8 interpreter as a core device (stage K1): it owns its
 * Chip8State (registers, timers, the 4 KB memory, the display, the keys,
 * the translated-block cache) and registers its state; after a load the
 * cache is flushed and the display marked for drawing.
 *
 * gemu_chip8_exec() makes it the machine's executor at hz instructions a
 * second (a frame's worth times 60).  A run executes translated blocks
 * until its budget is spent or a key wait (Fx0A) stalls it; the rest of
 * the budget is then idle time.  A block that ends past the budget doesn't
 * take the next run's time (the run reports its budget): a frame runs its
 * instructions per frame, as before stage K.  With a stop_before hook set
 * (breakpoints), it runs an instruction at a time and checks the hook
 * before each; true stops the machine there.
 */
#include "chip8.h"
#include "gemu/dev/device.h"
#include "gemu/dev/scheduler.h"
#include <stdbool.h>
#include <stdint.h>

extern const GemuDeviceType gemu_chip8_type;

Chip8State *gemu_chip8(GemuDevice *d);

typedef enum {
    GEMU_CHIP8_BUDGET,    /* ran its budget (or idled in a key wait) */
    GEMU_CHIP8_STOPPED,   /* stop_before */
} GemuChip8End;

GemuExecutor *gemu_chip8_exec(GemuDevice *d, uint64_t hz);
void          gemu_chip8_set_stop_before(GemuDevice *d, bool (*fn)(void *ud), void *ud);
GemuChip8End  gemu_chip8_ended(GemuDevice *d);
