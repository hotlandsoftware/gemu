/* Shared pieces for the mos family's machines on the core: see mos_core.h. */
#include "mos_core.h"
#include <string.h>

struct MosBusSlot *mos_bus_fill(MosBus *b, uint16_t addr, bool write) {
    uint64_t gen = gemu_space_generation(b->space);
    if (b->gen != gen) {
        memset(b->slot, 0, sizeof b->slot);
        b->gen = gen;
    }
    struct MosBusSlot *sl = &b->slot[write][addr >> 8];
    unsigned lo = addr & 0xFFu;
    GemuSpaceRun r;
    if (gemu_space_run(b->space, addr, write, &r)) {
        unsigned hi = r.avail > 0xFFu - lo ? 0xFFu : lo + (unsigned)r.avail - 1u;
        sl->lo = (uint8_t)lo;
        sl->hi = (uint8_t)hi;
        if (r.ram) { sl->ram = r.ram - lo; sl->map = NULL; sl->kind = 1; }
        else       { sl->map = r.map; sl->off = (int64_t)r.off - (int64_t)lo; sl->ram = NULL; sl->kind = 2; }
    } else {
        sl->lo = sl->hi = (uint8_t)lo;   /* this address: the space */
        sl->kind = 3;
    }
    return sl;
}
