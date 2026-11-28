#pragma once
/*
 * A CPU's cache of a 16-bit space's decode (stage M, moved to the core in
 * stage R): one entry per 256-byte page
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
    struct GemuSpaceCacheSlot {
        uint8_t     *ram;     /* host memory for the page's byte 0 (indexed by the low byte) */
        GemuMapping *map;     /* or the handler, at offset off + the low byte */
        int64_t      off;
        uint8_t      lo, hi;  /* the run: low bytes lo..hi */
        uint8_t      kind;    /* 0 = not looked up, 1 = ram, 2 = map, 3 = the space */
    } slot[2][256];
    uint64_t   gen;
    GemuSpace *space;
} GemuSpaceCache;

/* Set space (a 16-bit one) before the first access; zeroed otherwise. */

struct GemuSpaceCacheSlot *gemu_space_cache_fill(GemuSpaceCache *b, uint16_t addr, bool write);

static inline struct GemuSpaceCacheSlot *gemu_space_cache_slot(GemuSpaceCache *b, uint16_t addr, bool write) {
    struct GemuSpaceCacheSlot *sl = &b->slot[write][addr >> 8];
    uint8_t lo = (uint8_t)addr;
    if (__builtin_expect(!sl->kind || lo < sl->lo || lo > sl->hi ||
                         b->gen != gemu_space_generation(b->space), 0))
        sl = gemu_space_cache_fill(b, addr, write);
    return sl;
}
static inline uint8_t gemu_space_cache_read8(GemuSpaceCache *b, uint16_t addr) {
    struct GemuSpaceCacheSlot *sl = gemu_space_cache_slot(b, addr, false);
    if (sl->kind == 1) return sl->ram[addr & 0xFFu];
    if (sl->kind == 2) return sl->map->h.r8(sl->map->ud, (uint64_t)(sl->off + (addr & 0xFFu)));
    return gemu_space_read8(b->space, addr);
}
static inline void gemu_space_cache_write8(GemuSpaceCache *b, uint16_t addr, uint8_t v) {
    struct GemuSpaceCacheSlot *sl = gemu_space_cache_slot(b, addr, true);
    if (sl->kind == 1)      sl->ram[addr & 0xFFu] = v;
    else if (sl->kind == 2) sl->map->h.w8(sl->map->ud, (uint64_t)(sl->off + (addr & 0xFFu)), v);
    else                    gemu_space_write8(b->space, addr, v);
}
