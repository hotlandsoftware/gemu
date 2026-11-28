#pragma once
/*
 * The Z80 as a core device (stage Z1): it owns its Z80 and registers its
 * state.  The machine wires the bus and I/O (gemu_z80(d)->read, write, in,
 * out, ud) after adding it.  /INT and /NMI stay the chip's fields
 * (int_line, nmi_line): the boards set them from their own devices, and a
 * line would cost a call for nothing.
 *
 * gemu_z80_exec() makes it the machine's executor at hz T-states a second:
 * a run executes instructions until its budget is spent, or a hook or the
 * machine ends it.  The hooks (the 6502's):
 *   stop_before  before each instruction (a breakpoint at the PC): true
 *                stops the machine without running it
 *   insn         one instruction with the machine's extras (chips clocked
 *                alongside); NULL = z80_step
 *   stop_after   after each (the monitor paused): true stops the machine
 *   done         after each (a frame finished): true ends the run and
 *                stops the machine
 * Any may be NULL; with none, the run is a plain loop of z80_step, as fast
 * as a machine's own loop.  gemu_z80_set_hooks() changes them between runs
 * (a machine sets stop_before only while breakpoints are set).
 * gemu_z80_end_run(), from inside an instruction (an I/O write, a
 * watchpoint), ends the run after it, as a hook would.
 * The device counts the instructions it runs (gemu_z80_insns, saved).
 * gemu_z80_ended() says why the last run ended.
 */
#include "z80.h"
#include "gemu/dev/device.h"
#include "gemu/dev/scheduler.h"
#include <stdbool.h>
#include <stdint.h>

extern const GemuDeviceType gemu_z80_type;

Z80 *gemu_z80(GemuDevice *d);

typedef struct {
    bool (*stop_before)(void *ud);
    void (*insn)(void *ud);
    bool (*stop_after)(void *ud);
    bool (*done)(void *ud);
    void *ud;
} GemuZ80Hooks;

typedef enum {
    GEMU_Z80_BUDGET,    /* ran its budget */
    GEMU_Z80_STOPPED,   /* stop_before or stop_after */
    GEMU_Z80_DONE,      /* done */
} GemuZ80End;

GemuExecutor *gemu_z80_exec(GemuDevice *d, uint64_t hz, const GemuZ80Hooks *h);
void          gemu_z80_set_hooks(GemuDevice *d, const GemuZ80Hooks *h);
void          gemu_z80_end_run(GemuDevice *d, GemuZ80End why);
GemuZ80End    gemu_z80_ended(GemuDevice *d);
uint64_t      gemu_z80_insns(GemuDevice *d);
