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
