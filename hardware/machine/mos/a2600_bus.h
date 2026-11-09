#pragma once
/*
 * Atari 2600 bus: the memory map, the WSYNC hold and power-on, with the
 * TIA run lazily.
 *
 * The TIA's state only matters to the CPU when the CPU touches a TIA
 * register (and to the machine at a frame boundary, which is itself a TIA
 * write: VSYNC), so it is caught up only then, rather than before every
 * bus access.  Everything the TIA computes is the same; it just happens in
 * fewer, longer runs, which lets its idle-span fast path skip ahead.
 * Cartridge reads of the common schemes (2K/4K, F8/F6/F4 without the
 * Superchip) take a direct path.  Anything else that looks at the TIA
 * between accesses (the debugger, the display) calls a26_bus_sync() first.
 *
 * From GEMU for Wii, where tests/a26_exact.c checks it against the eager
 * bus frame by frame and sample by sample.
 */
#include "atari2600.h"

void a26_bus_attach(Atari2600State *s);   /* CPU read/write callbacks, RAM access */
void a26_bus_step(Atari2600State *s);     /* one instruction + WSYNC hold */
void a26_bus_power_on(Atari2600State *s); /* power cycle; picks the cartridge fast path */
void a26_bus_sync(Atari2600State *s);     /* run the TIA up to the CPU */
