#pragma once
/*
 * The 2600's core machine (stage T), shared by machine_atari2600.c (the
 * devices, state, executor, screen and stream) and a2600_bus.c (the bus).
 */
#include "atari2600.h"
#include "gemu/dev/device.h"
#include "gemu/dev/scheduler.h"
#include "gemu/dev/screen.h"
#include "gemu/dev/space.h"
#include "gemu/dev/stream.h"

typedef struct A26Core {
    GemuMachine *machine;
    GemuDevice  *board, *cpu, *tia, *riot, *cart;
    /* T2: the 6507's bus, 13 bits */
    GemuSpace   *bus;
    GemuMapping *rom_map[2];   /* the cartridge's ROM as plain memory (fast schemes) */
    uint16_t     rom_bank;     /* bank[0] when rom_map[0] was pointed (F8/F6/F4) */
    /* reads, writes: how each 64 bytes are served (a2600_bus.c, bus_slot()) */
    struct A26Slot {
        uint8_t     *ram;      /* host memory for the slot's first byte */
        GemuMapping *map;      /* or the one handler that serves it */
        uint32_t     off;      /* the slot's offset into map */
        uint8_t      kind;     /* 0 = not looked up, 1 = ram, 2 = map, 3 = the space */
    } slot[2][128];
    uint64_t     slot_gen;
    /* T3: the 6507 runs the machine, in colour clocks (a26_exec_run()) */
    GemuExecutor *exec;
    uint64_t     run_start;    /* cpu.cycle_count when this run began */
    bool         stopped;      /* the last run ended at a breakpoint, a debugger stop or a pause */
    /* T4: the TIA's picture, the 240-line window as the TV shows it */
    GemuScreen  *screen;
    int          screen_lines, screen_top;   /* the raw timing as last set */
    /* T5: the TIA's sound */
    GemuStream  *sound;
} A26Core;

bool a26_bus_build(Atari2600State *s);   /* the space and its mappings */
void a26_maps_sync(Atari2600State *s);   /* the ROM windows, after a cartridge change or a load */
