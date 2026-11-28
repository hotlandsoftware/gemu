#pragma once
/*
 * The 6502 as a core device (stage M1): it owns its Mos6502, registers its
 * state, and has two input lines, "nmi" (an edge: a rise latches an NMI)
 * and "irq" (the level, wired-OR by the core).  Option "variant" (6502,
 * 2a03, 2a07, 6501, 6507) applies the pin and ALU differences
 * (mos_cpu_apply).  The machine wires the bus (gemu_mos6502(d)->mem_read,
 * mem_write, mem_ud) after adding it.
 *
 * gemu_mos6502_exec() makes it the machine's executor (or one of them) at
 * hz: a run executes instructions until its budget is spent, or a hook
 * stops it:
 *   stop_before  before each instruction (a breakpoint at the PC): true
 *                stops the machine without running it
 *   insn         one instruction with the machine's extras (traps, ticks
 *                of chips that count CPU cycles); NULL = mos6502_step
 *   stop_after   after each (the monitor paused): true stops the machine
 *   done         after each (a frame finished): true ends the run and
 *                stops the machine, so a frame loop can come back
 * Any may be NULL.  gemu_mos6502_ended() says why the last run ended.
 */
#include "mos6502.h"
#include "gemu/dev/device.h"
#include "gemu/dev/scheduler.h"
#include <stdbool.h>
#include <stdint.h>

extern const GemuDeviceType gemu_mos6502_type;

Mos6502 *gemu_mos6502(GemuDevice *d);

typedef struct {
    bool (*stop_before)(void *ud);
    void (*insn)(void *ud);
    bool (*stop_after)(void *ud);
    bool (*done)(void *ud);
    void *ud;
} GemuMos6502Hooks;

typedef enum {
    GEMU_MOS6502_BUDGET,    /* ran its budget */
    GEMU_MOS6502_STOPPED,   /* stop_before or stop_after */
    GEMU_MOS6502_DONE,      /* done */
} GemuMos6502End;

GemuExecutor  *gemu_mos6502_exec(GemuDevice *d, uint64_t hz, const GemuMos6502Hooks *h);
GemuExecutor  *gemu_mos6502_executor(GemuDevice *d);
GemuMos6502End gemu_mos6502_ended(GemuDevice *d);
