/* PCI bus: see pci.h. */
#include "bus/pci.h"
#include <stddef.h>
#include <string.h>

void pci_bus_init(PciBus *b) {
    memset(b, 0, sizeof *b);
}

bool pci_bus_add(PciBus *b, unsigned bus, unsigned dev, unsigned fn,
                 const PciFunctionOps *ops, void *opaque) {
    if (b->n == PCI_BUS_MAX_FUNCTIONS || bus > 255 || dev > 31 || fn > 7)
        return false;
    b->fn[b->n++] = (PciFunction){
        .bus = (uint8_t)bus, .dev = (uint8_t)dev, .fn = (uint8_t)fn,
        .ops = ops, .opaque = opaque,
    };
    return true;
}

static bool present(const PciFunction *f) {
    return !f->ops->present || f->ops->present(f->opaque);
}

const PciFunction *pci_bus_find(const PciBus *b, unsigned bus, unsigned dev, unsigned fn) {
    for (unsigned i = 0; i < b->n; i++) {
        const PciFunction *f = &b->fn[i];
        if (f->bus == bus && f->dev == dev && f->fn == fn && present(f))
            return f;
    }
    return NULL;
}

bool pci_bus_cfg_read(const PciBus *b, unsigned bus, unsigned dev, unsigned fn,
                      unsigned reg, unsigned size, uint32_t *val) {
    const PciFunction *f = pci_bus_find(b, bus, dev, fn);
    if (!f || reg + size > 256 || !f->ops->cfg_read)
        return false;
    *val = f->ops->cfg_read(f->opaque, reg, size);
    return true;
}

bool pci_bus_cfg_write(PciBus *b, unsigned bus, unsigned dev, unsigned fn,
                       unsigned reg, unsigned size, uint32_t val) {
    const PciFunction *f = pci_bus_find(b, bus, dev, fn);
    if (!f || reg + size > 256 || !f->ops->cfg_write)
        return false;
    f->ops->cfg_write(f->opaque, reg, size, val);
    return true;
}

bool pci_bus_mem_claims(const PciBus *b, uint64_t addr) {
    for (unsigned i = 0; i < b->n; i++) {
        const PciFunction *f = &b->fn[i];
        if (f->ops->mem_claims && present(f) && f->ops->mem_claims(f->opaque, addr))
            return true;
    }
    return false;
}

bool pci_bus_mem_access(PciBus *b, bool write, uint64_t addr, unsigned size, uint64_t *val) {
    for (unsigned i = 0; i < b->n; i++) {
        const PciFunction *f = &b->fn[i];
        if (f->ops->mem_access && present(f) &&
            f->ops->mem_access(f->opaque, write, addr, size, val))
            return true;
    }
    return false;
}

bool pci_bus_io_access(PciBus *b, bool write, uint16_t port, unsigned size, uint64_t *val) {
    for (unsigned i = 0; i < b->n; i++) {
        const PciFunction *f = &b->fn[i];
        if (f->ops->io_access && present(f) &&
            f->ops->io_access(f->opaque, write, port, size, val))
            return true;
    }
    return false;
}

/* ── Decode as space mappings ─────────────────────────────────────────── */

static uint64_t win_ones(unsigned n) { return n >= 8 ? ~0ull : (1ull << (8 * n)) - 1; }

static uint64_t win_read(PciMapped *pm, uint64_t off, unsigned n) {
    uint64_t v = 0, a = pm->w.base + off;
    bool ok = pm->w.io ? pm->ops->io_access(pm->opaque, false, (uint16_t)a, n, &v)
                       : pm->ops->mem_access(pm->opaque, false, a, n, &v);
    if (ok)
        return v;
    if (pm->d->miss)
        pm->d->miss(pm->d->ud, pm->w.io, false, a, n, 0);
    return win_ones(n);
}

static void win_write(PciMapped *pm, uint64_t off, unsigned n, uint64_t v) {
    uint64_t a = pm->w.base + off;
    bool ok = pm->w.io ? pm->ops->io_access(pm->opaque, true, (uint16_t)a, n, &v)
                       : pm->ops->mem_access(pm->opaque, true, a, n, &v);
    if (!ok && pm->d->miss)
        pm->d->miss(pm->d->ud, pm->w.io, true, a, n, v);
}

static uint8_t  win_r8 (void *ud, uint64_t o) { return (uint8_t)win_read(ud, o, 1); }
static uint16_t win_r16(void *ud, uint64_t o) { return (uint16_t)win_read(ud, o, 2); }
static uint32_t win_r32(void *ud, uint64_t o) { return (uint32_t)win_read(ud, o, 4); }
static uint64_t win_r64(void *ud, uint64_t o) { return win_read(ud, o, 8); }
static void win_w8 (void *ud, uint64_t o, uint8_t v)  { win_write(ud, o, 1, v); }
static void win_w16(void *ud, uint64_t o, uint16_t v) { win_write(ud, o, 2, v); }
static void win_w32(void *ud, uint64_t o, uint32_t v) { win_write(ud, o, 4, v); }
static void win_w64(void *ud, uint64_t o, uint64_t v) { win_write(ud, o, 8, v); }
static const GemuHandlers win_handlers = {
    .r8 = win_r8, .r16 = win_r16, .r32 = win_r32, .r64 = win_r64,
    .w8 = win_w8, .w16 = win_w16, .w32 = win_w32, .w64 = win_w64,
};

void pci_decode_init(PciDecode *d, PciBus *bus, GemuSpace *mem, GemuSpace *io,
                     uint64_t mem_lo, uint64_t mem_hi, int priority) {
    memset(d, 0, sizeof *d);
    d->bus = bus;
    d->mem = mem;
    d->io = io;
    d->mem_lo = mem_lo;
    d->mem_hi = mem_hi;
    d->priority = priority;
}

void pci_decode_clear(PciDecode *d) {
    for (unsigned i = 0; i < d->n; i++)
        if (d->map[i].m)
            gemu_mapping_remove(d->map[i].m);
    d->n = 0;
}

/* A window as it may be mapped: clipped to where the bus decodes, and to
 * the space; false if nothing is left. */
static bool clip(const PciDecode *d, PciWindow *w) {
    GemuSpace *sp = w->io ? d->io : d->mem;
    if (!sp || !w->size)
        return false;
    uint64_t lo = w->base, hi = w->base + w->size;   /* exclusive */
    if (hi < lo)
        hi = UINT64_MAX;
    if (!w->io) {
        if (lo < d->mem_lo) lo = d->mem_lo;
        if (hi > d->mem_hi) hi = d->mem_hi;
    }
    unsigned bits = gemu_space_addr_bits(sp);
    uint64_t end = bits >= 64 ? UINT64_MAX : 1ull << bits;
    if (hi > end) hi = end;
    if (lo >= hi)
        return false;
    w->base = lo;
    w->size = hi - lo;
    return true;
}

void pci_decode_update(PciDecode *d) {
    PciMapped want[PCI_DECODE_MAX];
    unsigned n = 0;
    /* Last function first: at one priority the most recent mapping wins, so
     * the bus's first function takes an overlap. */
    for (unsigned i = d->bus->n; i-- > 0;) {
        const PciFunction *f = &d->bus->fn[i];
        if (!f->ops->windows || !present(f))
            continue;
        PciWindow w[PCI_WINDOWS_MAX];
        unsigned k = f->ops->windows(f->opaque, w);
        for (unsigned j = 0; j < k && j < PCI_WINDOWS_MAX && n < PCI_DECODE_MAX; j++) {
            if (!clip(d, &w[j]))
                continue;
            if (w[j].io ? !f->ops->io_access : !f->ops->mem_access)
                continue;
            want[n++] = (PciMapped){ .d = d, .ops = f->ops, .opaque = f->opaque, .w = w[j] };
        }
    }
    bool same = n == d->n;
    for (unsigned i = 0; same && i < n; i++)
        same = want[i].ops == d->map[i].ops && want[i].opaque == d->map[i].opaque &&
               want[i].w.io == d->map[i].w.io && want[i].w.base == d->map[i].w.base &&
               want[i].w.size == d->map[i].w.size;
    if (same)
        return;
    pci_decode_clear(d);
    for (unsigned i = 0; i < n; i++) {
        PciMapped *pm = &d->map[i];
        *pm = want[i];
        pm->m = gemu_space_map(pm->w.io ? d->io : d->mem, pm->w.base, pm->w.size,
                               &win_handlers, pm, d->priority, 0);
        if (pm->m && pm->ops->name)
            gemu_mapping_set_name(pm->m, pm->ops->name);
    }
    d->n = n;
}
