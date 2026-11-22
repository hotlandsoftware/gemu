/* The PCI bus's dispatch.  Run: ninja check-pci */
#include "bus/pci.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

/* A card with a 256-byte config space, one memory window and one port. */
typedef struct {
    uint8_t  cfg[256];
    bool     there;
    uint64_t mem_base, mem_size;
    uint16_t port;
    unsigned mem_hits, io_hits;
} Card;

static bool present(const void *d) { return ((const Card *)d)->there; }
static uint32_t cfg_read(const void *d, unsigned reg, unsigned size) {
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v |= (uint32_t)((const Card *)d)->cfg[reg + i] << (i * 8);
    return v;
}
static void cfg_write(void *d, unsigned reg, unsigned size, uint32_t val) {
    for (unsigned i = 0; i < size; i++) ((Card *)d)->cfg[reg + i] = (uint8_t)(val >> (i * 8));
}
static bool claims(const void *d, uint64_t addr) {
    const Card *c = d;
    return addr - c->mem_base < c->mem_size;
}
static bool mem(void *d, bool write, uint64_t addr, unsigned size, uint64_t *val) {
    Card *c = d;
    (void)size;
    if (!claims(c, addr)) return false;
    c->mem_hits++;
    if (!write) *val = addr - c->mem_base;
    return true;
}
static bool io(void *d, bool write, uint16_t port, unsigned size, uint64_t *val) {
    Card *c = d;
    (void)write; (void)size;
    if (port != c->port) return false;
    c->io_hits++;
    *val = 0x5A;
    return true;
}
static const PciFunctionOps ops = {
    .name = "card", .present = present, .cfg_read = cfg_read, .cfg_write = cfg_write,
    .mem_claims = claims, .mem_access = mem, .io_access = io,
};
static const PciFunctionOps cfg_only = { .name = "cfg", .cfg_read = cfg_read };

int main(void) {
    PciBus b;
    pci_bus_init(&b);
    Card a = { .there = true, .mem_base = 0x80000000, .mem_size = 0x1000, .port = 0x1000 };
    Card c = { .there = true, .mem_base = 0x80000800, .mem_size = 0x1000, .port = 0x2000 };
    Card d = {0};
    a.cfg[0] = 0x11; c.cfg[0] = 0x22; d.cfg[0] = 0x33;
    CHECK(pci_bus_add(&b, 0, 2, 0, &ops, &a));
    CHECK(pci_bus_add(&b, 0, 4, 1, &ops, &c));
    CHECK(pci_bus_add(&b, 1, 0, 0, &cfg_only, &d));
    CHECK(!pci_bus_add(&b, 0, 32, 0, &ops, &a) && !pci_bus_add(&b, 0, 0, 8, &ops, &a));

    /* Config cycles go to the function at that bus/device/function. */
    uint32_t v = 0;
    CHECK(pci_bus_cfg_read(&b, 0, 2, 0, 0, 1, &v) && v == 0x11);
    CHECK(pci_bus_cfg_read(&b, 0, 4, 1, 0, 1, &v) && v == 0x22);
    CHECK(pci_bus_cfg_read(&b, 1, 0, 0, 0, 1, &v) && v == 0x33);
    CHECK(!pci_bus_cfg_read(&b, 0, 4, 0, 0, 1, &v));            /* nothing there */
    CHECK(!pci_bus_cfg_read(&b, 0, 2, 0, 0xFE, 4, &v));          /* past 256 bytes */
    CHECK(pci_bus_cfg_write(&b, 0, 2, 0, 4, 2, 0x0107) && a.cfg[4] == 7 && a.cfg[5] == 1);
    CHECK(!pci_bus_cfg_write(&b, 1, 0, 0, 4, 1, 0xFF) && d.cfg[4] == 0);   /* read-only */

    /* An absent function answers nothing. */
    c.there = false;
    CHECK(!pci_bus_cfg_read(&b, 0, 4, 1, 0, 1, &v) && !pci_bus_find(&b, 0, 4, 1));
    CHECK(!pci_bus_mem_claims(&b, 0x80001000));
    c.there = true;
    CHECK(pci_bus_find(&b, 0, 4, 1)->opaque == &c);

    /* Memory: claims, and the first function claiming an overlap wins. */
    CHECK(pci_bus_mem_claims(&b, 0x80000000) && pci_bus_mem_claims(&b, 0x800017FF));
    CHECK(!pci_bus_mem_claims(&b, 0x80001800) && !pci_bus_mem_claims(&b, 0x7FFFFFFF));
    uint64_t r = 99;
    CHECK(pci_bus_mem_access(&b, false, 0x80000900, 4, &r) && r == 0x900);
    CHECK(a.mem_hits == 1 && c.mem_hits == 0);
    CHECK(pci_bus_mem_access(&b, false, 0x80001100, 4, &r) && r == 0x900 && c.mem_hits == 1);
    CHECK(!pci_bus_mem_access(&b, true, 0x90000000, 4, &r));

    /* I/O. */
    CHECK(pci_bus_io_access(&b, false, 0x2000, 1, &r) && r == 0x5A && c.io_hits == 1);
    CHECK(!pci_bus_io_access(&b, false, 0x3000, 1, &r));

    printf("pci: %u checks passed\n", checks);
    return 0;
}
