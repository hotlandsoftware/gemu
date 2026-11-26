/*
 * A stand-in for 86Box v6.0's src/cpu/cpu.h, for GEMU's 86Box layer
 * (compat/86box): drivers include it as "cpu.h".
 *
 * GEMU's CPUs are its own; this gives a driver only what can mean
 * something without them.  Anything else from 86Box's CPU (cpu_state, CS,
 * the CPU type, ...) isn't here, so a driver that reads it doesn't compile
 * and its port must decide what to do.
 *
 *   cycles, isa_timing: some drivers charge the 86Box CPU for a slow bus
 *   access (cycles -= isa_timing * n).  GEMU's CPU cores time their own bus
 *   cycles, so this goes nowhere - and a port switches it off where the
 *   driver can (the OPL's set_do_cycles(0)), so it doesn't even happen.
 */
#ifndef GEMU_86BOX_CPU_H
#define GEMU_86BOX_CPU_H

extern int    gemu_86box_cycles;
extern double isa_timing;
#define cycles gemu_86box_cycles

#endif /*GEMU_86BOX_CPU_H*/
