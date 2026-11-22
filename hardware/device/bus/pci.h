#pragma once
/*
 * A PCI bus: the functions a machine plugs in, each at a bus/device/function
 * number, with its config space and what it decodes.  The bus answers
 * config cycles for those numbers and dispatches memory and I/O accesses to
 * whichever function claims them; anything else is the caller's (a
 * chipset's own config space, unclaimed addresses).
 *
 * A function is a device's own state plus a PciFunctionOps table.  Members
 * may be NULL: no memory, no I/O, always present.  Config space is the
 * standard 256 bytes.
 */
#include <stdbool.h>
#include <stdint.h>

typedef struct PciFunctionOps {
    const char *name;
    /* Whether the function is there now (a card that can be swapped). */
    bool     (*present)(const void *dev);
    uint32_t (*cfg_read)(const void *dev, unsigned reg, unsigned size);
    void     (*cfg_write)(void *dev, unsigned reg, unsigned size, uint32_t val);
    /* Whether a memory address falls in one of its enabled windows. */
    bool     (*mem_claims)(const void *dev, uint64_t addr);
    bool     (*mem_access)(void *dev, bool write, uint64_t addr, unsigned size, uint64_t *val);
    bool     (*io_access)(void *dev, bool write, uint16_t port, unsigned size, uint64_t *val);
} PciFunctionOps;

typedef struct {
    uint8_t  bus, dev, fn;
    const PciFunctionOps *ops;
    void    *opaque;
} PciFunction;

#define PCI_BUS_MAX_FUNCTIONS 16

typedef struct {
    PciFunction fn[PCI_BUS_MAX_FUNCTIONS];
    unsigned    n;
} PciBus;

void pci_bus_init(PciBus *b);
bool pci_bus_add(PciBus *b, unsigned bus, unsigned dev, unsigned fn,
                 const PciFunctionOps *ops, void *opaque);
/* The function at that address, present, or NULL. */
const PciFunction *pci_bus_find(const PciBus *b, unsigned bus, unsigned dev, unsigned fn);

/* Config cycles: false if no function there (the caller's to answer). */
bool pci_bus_cfg_read(const PciBus *b, unsigned bus, unsigned dev, unsigned fn,
                      unsigned reg, unsigned size, uint32_t *val);
bool pci_bus_cfg_write(PciBus *b, unsigned bus, unsigned dev, unsigned fn,
                       unsigned reg, unsigned size, uint32_t val);

/* Memory and I/O: the first present function to claim the access takes it. */
bool pci_bus_mem_claims(const PciBus *b, uint64_t addr);
bool pci_bus_mem_access(PciBus *b, bool write, uint64_t addr, unsigned size, uint64_t *val);
bool pci_bus_io_access(PciBus *b, bool write, uint16_t port, unsigned size, uint64_t *val);
