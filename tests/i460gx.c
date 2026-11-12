/* Standalone 460GX integration: no i2000, CPU, ROM, or host UI is linked.
 * Run: python3 tools/ia64/run_i460gx_test.py */
#include "chipset/i460gx.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

typedef struct {
    uint64_t ticks;
    uint8_t spd_value, apm_value;
    unsigned spd_reads, apm_writes;
} Board;

static uint64_t clock_read(void *opaque) {
    return ((Board *)opaque)->ticks;
}
static bool spd_read(void *opaque, unsigned card, unsigned row,
                      unsigned slave, unsigned reg, uint8_t *value) {
    Board *board = opaque;
    board->spd_reads++;
    if (card != 1 || row != 2 || slave != 0x56 || reg != 2)
        return false;
    *value = board->spd_value;
    return true;
}
static void apm_write(void *opaque, uint8_t value) {
    Board *board = opaque;
    board->apm_value = value;
    board->apm_writes++;
}
static uint64_t pci_read(I460GX *s, unsigned bus, unsigned dev,
                          unsigned fun, unsigned reg, unsigned size) {
    uint64_t value;
    CHECK(i460gx_pci_read(s, bus, dev, fun, reg, size, &value));
    return value;
}
static void pci_write(I460GX *s, unsigned bus, unsigned dev, unsigned fun,
                       unsigned reg, uint64_t value, unsigned size) {
    CHECK(i460gx_pci_write(s, bus, dev, fun, reg, value, size));
}
static void io_write(I460GX *s, unsigned fun, unsigned port,
                      uint64_t value, unsigned size) {
    CHECK(i460gx_ifb_io_write(s, fun, port, value, size));
}
static uint64_t io_read(I460GX *s, unsigned fun, unsigned port, unsigned size) {
    uint64_t value;
    CHECK(i460gx_ifb_io_read(s, fun, port, size, &value));
    return value;
}
static void smbus_read(I460GX *s) {
    io_write(s, 3, 0x1803, 2, 1);
    io_write(s, 3, 0x1804, 0xad, 1); /* slave56, read */
    io_write(s, 3, 0x1802, 0x48, 1); /* byte-data START */
}

int main(void) {
    Board first = {.spd_value = 0x5a}, second = {.spd_value = 0xa5};
    I460GXConfig config = {
        .ifb_bus = 2, .ifb_dev = 7, .cbn_bus = 3, .cbn_dev = 8,
        .link_count = 2, .expander = {[0] = I460GX_BRIDGE_GXB},
        .clock_hz = 1000000, .clock = clock_read, .spd_read = spd_read,
        .smbus_card = 1, .smbus_row = 2, .apm_write = apm_write,
        .opaque = &first,
    };
    I460GX a, b;
    i460gx_init(&a, &config);
    config.ifb_bus = 4; config.ifb_dev = 9;
    config.cbn_bus = 5; config.cbn_dev = 10;
    config.link_count = 1;
    config.expander[0] = I460GX_BRIDGE_PXB;
    config.opaque = &second;
    i460gx_init(&b, &config);
    CHECK(pci_read(&a, 2, 7, 0, 0, 4) == UINT32_C(0x76008086));
    CHECK(pci_read(&b, 4, 9, 0, 0, 4) == UINT32_C(0x76008086));
    uint64_t value;
    CHECK(!i460gx_pci_read(&a, 0, 3, 0, 0, 4, &value));
    CHECK(!i460gx_pci_read(&b, 2, 7, 0, 0, 4, &value));
    pci_write(&a, 3, 8, 0, 0x40, 0x30, 1);
    CHECK(pci_read(&a, 0x30, 0, 0, 0x60, 1) == 2);
    CHECK(pci_read(&b, 0xff, 0, 0, 0x60, 1) == 1);
    CHECK(pci_read(&a, 0x30, 0x10, 1, 0, 4) == UINT32_C(0x84ea8086));
    CHECK(pci_read(&a, 0x30, 0x10, 2, 0, 4) == UINT32_C(0x84e28086));
    CHECK(pci_read(&b, 0xff, 0x10, 1, 0, 4) == UINT32_C(0x84cb8086));
    CHECK(!i460gx_pci_read(&a, 0xff, 0, 0, 0, 4, &value));

    /* Board-supplied memory card 1 / row 2 / slave56 reaches the same SPD
     * callback through both SAC I2C translation and IFB SMBus. */
    pci_write(&a, 0x30, 0, 0, 0x68, 0xd6, 1);
    pci_write(&a, 0x30, 6, 6, 0x48, 1, 1);
    CHECK(pci_read(&a, 0x30, 6, 2, 2, 1) == 0x5a);
    pci_write(&a, 0x30, 6, 4, 0x48, 1, 1);
    CHECK(pci_read(&a, 0x30, 6, 2, 2, 1) == 0xff); /* ambiguous row */
    pci_write(&a, 2, 7, 3, 0x20, 0x1801, 4);
    pci_write(&a, 2, 7, 3, 4, 1, 1);
    pci_write(&b, 4, 9, 3, 0x20, 0x1801, 4);
    pci_write(&b, 4, 9, 3, 4, 1, 1);
    smbus_read(&a); smbus_read(&b);
    CHECK(io_read(&a, 3, 0x1805, 1) == 0x5a);
    CHECK(io_read(&b, 3, 0x1805, 1) == 0xa5);
    CHECK(first.spd_reads == 2 && second.spd_reads == 1);
    io_write(&a, 0, 0xb2, 0x37, 1);
    CHECK(first.apm_writes == 1 && first.apm_value == 0x37);
    CHECK(second.apm_writes == 0);
    CHECK(io_read(&a, 0, 0xb2, 1) == 0x37);

    /* Device timing follows the supplied frequency, independent of Merced. */
    pci_write(&a, 2, 7, 0, 0x40, 0xa01, 4);
    pci_write(&a, 2, 7, 0, 0x44, 1, 1);
    first.ticks = 1000000;
    CHECK(io_read(&a, 0, 0xa08, 4) == 3579545);
    pci_write(&a, 2, 7, 2, 0x20, 0x1201, 4);
    pci_write(&a, 2, 7, 2, 4, 1, 1);
    io_write(&a, 2, 0x1206, 0x7ff, 2);
    io_write(&a, 2, 0x1200, 1, 2);
    first.ticks += 2000;
    CHECK(io_read(&a, 2, 0x1206, 2) == 1); /* two 1ms frames, wrap */

    uint8_t vector = 0;
    bool extint = false;
    CHECK(!i460gx_interrupt(&a, 9, &vector, &extint));
    CHECK(i460gx_mmio_write(&a, I460GX_IOSAPIC_BASE, 0x22, 4));
    CHECK(i460gx_mmio_write(&a, I460GX_IOSAPIC_BASE + 0x10, 0x71, 4));
    CHECK(i460gx_interrupt(&a, 9, &vector, &extint) && vector == 0x71 && !extint);
    CHECK(!i460gx_interrupt(&b, 9, &vector, &extint));
    CHECK(i460gx_mmio_write(&a, I460GX_IOSAPIC_BASE + 0x10, 0x700, 4));
    CHECK(i460gx_interrupt(&a, 9, &vector, &extint) && extint);

    CHECK(i460gx_mmio_write(&a, I460GX_SAC_CBNR, 0x81, 4));
    CHECK(i460gx_mmio_write(&a, I460GX_SAC_CCSR, 0x80, 4));
    pci_write(&a, 0x30, 0, 0, 0x44, 0x680, 2);
    i460gx_warm_reset(&a);
    CHECK(a.sac_cbnr == 0x81 && a.sac_ccsr == 0x80);
    CHECK(pci_read(&a, 0xff, 0, 0, 0x44, 2) == 0x680);
    CHECK(!i460gx_interrupt(&a, 9, &vector, &extint));
    CHECK(!i460gx_ifb_io_read(&a, 3, 0x1805, 1, &value));
    i460gx_reset(&a);
    CHECK(!a.sac_cbnr && !a.sac_ccsr);
    CHECK(pci_read(&a, 0xff, 0, 0, 0x44, 2) == 0);
    CHECK(a.config.opaque == &first && b.config.opaque == &second);
    io_write(&a, 0, 0xb2, 0x73, 1);
    CHECK(first.apm_writes == 2 && first.apm_value == 0x73);
    CHECK(io_read(&b, 3, 0x1805, 1) == 0xa5);
    printf("460GX standalone: %u checks passed\n", checks);
    return 0;
}
