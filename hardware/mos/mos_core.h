#pragma once
/*
 * Shared pieces for the mos family's machines on the core (stage M).
 *
 * MosBus: a CPU's cache of its space's decode, one entry per 256-byte page
 * for reads and one for writes: the run of addresses around the last one
 * looked up there (gemu_space_run()) that plain memory or one handler
 * serves, so a page shared by a chip and unmapped space (the Apple I's PIA
 * at $D010) still calls the chip directly.  Kept while the space's
 * generation holds.  Reads and writes through it are what
 * gemu_space_read8/write8 would do.
 */
#include "gemu/dev/space.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    struct MosBusSlot {
        uint8_t     *ram;     /* host memory for the page's byte 0 (indexed by the low byte) */
        GemuMapping *map;     /* or the handler, at offset off + the low byte */
        int64_t      off;
        uint8_t      lo, hi;  /* the run: low bytes lo..hi */
        uint8_t      kind;    /* 0 = not looked up, 1 = ram, 2 = map, 3 = the space */
    } slot[2][256];
    uint64_t   gen;
    GemuSpace *space;
} MosBus;

struct MosBusSlot *mos_bus_fill(MosBus *b, uint16_t addr, bool write);

static inline struct MosBusSlot *mos_bus_slot(MosBus *b, uint16_t addr, bool write) {
    struct MosBusSlot *sl = &b->slot[write][addr >> 8];
    uint8_t lo = (uint8_t)addr;
    if (__builtin_expect(!sl->kind || lo < sl->lo || lo > sl->hi ||
                         b->gen != gemu_space_generation(b->space), 0))
        sl = mos_bus_fill(b, addr, write);
    return sl;
}
static inline uint8_t mos_bus_read(MosBus *b, uint16_t addr) {
    struct MosBusSlot *sl = mos_bus_slot(b, addr, false);
    if (sl->kind == 1) return sl->ram[addr & 0xFFu];
    if (sl->kind == 2) return sl->map->h.r8(sl->map->ud, (uint64_t)(sl->off + (addr & 0xFFu)));
    return gemu_space_read8(b->space, addr);
}
static inline void mos_bus_write(MosBus *b, uint16_t addr, uint8_t v) {
    struct MosBusSlot *sl = mos_bus_slot(b, addr, true);
    if (sl->kind == 1)      sl->ram[addr & 0xFFu] = v;
    else if (sl->kind == 2) sl->map->h.w8(sl->map->ud, (uint64_t)(sl->off + (addr & 0xFFu)), v);
    else                    gemu_space_write8(b->space, addr, v);
}
