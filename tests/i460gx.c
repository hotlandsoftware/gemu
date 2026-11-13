/* Standalone 460GX integration: no i2000, CPU, ROM, or host UI is linked.
 * Run: python3 tools/ia64/run_i460gx_test.py */
#include "chipset/i460gx.h"
#include "i2c/at24.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

/* A board with one EEPROM at slave 0x56 on its SMBus segment and one on
 * memory card 1's row-2 segment. Byte 2 of each differs. */
typedef struct {
    uint64_t ticks;
    bool smi;
    unsigned smi_assertions;
    int sci_irq;
    bool sci_level;
    I460GXPmState sleep_state;
    unsigned sleeps, wakes;
    I2cBus smbus, memcard;
    At24 smbus_eeprom, spd;
} Board;

static void board_init(Board *board, uint8_t smbus_byte, uint8_t spd_byte) {
    *board = (Board){0};
    i2c_bus_init(&board->smbus, "smbus");
    i2c_bus_init(&board->memcard, "memcard");
    at24_init(&board->smbus_eeprom);
    at24_init(&board->spd);
    board->smbus_eeprom.mem[2] = smbus_byte;
    board->spd.mem[2] = spd_byte;
    I2cDevice device = at24_device(&board->smbus_eeprom, 0x56);
    i2c_attach(&board->smbus, &device);
    device = at24_device(&board->spd, 0x56);
    i2c_attach(&board->memcard, &device);
}

static uint64_t clock_read(void *opaque) {
    return ((Board *)opaque)->ticks;
}
static void board_smi(void *opaque, bool asserted) {
    Board *board = opaque;
    board->smi = asserted;
    board->smi_assertions += asserted;
}
static void board_sci(void *opaque, unsigned irq, bool level) {
    Board *board = opaque;
    board->sci_irq = (int)irq;
    board->sci_level = level;
}
static void board_sleep(void *opaque, I460GXPmState state, bool override) {
    (void)override;
    Board *board = opaque;
    board->sleep_state = state;
    board->sleeps++;
}
static void board_wake(void *opaque, I460GXPmState from) {
    (void)from;
    ((Board *)opaque)->wakes++;
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
    Board first, second;
    board_init(&first, 0x5a, 0x5a);
    board_init(&second, 0xa5, 0x11);
    I460GXConfig config = {
        .ifb_bus = 2, .ifb_dev = 7, .pid_bus = 1, .pid_dev = 2,
        .cbn_bus = 3, .cbn_dev = 8,
        .link_count = 2, .expander = {[0] = I460GX_BRIDGE_GXB},
        .clock_hz = 1000000, .clock = clock_read,
        .smbus = &first.smbus, .memcard_i2c = {[1][2] = &first.memcard},
        .smi = board_smi, .sci = board_sci, .sleep = board_sleep,
        .wake = board_wake, .opaque = &first,
    };
    I460GX a, b;
    i460gx_init(&a, &config);
    config.ifb_bus = 4; config.ifb_dev = 9;
    config.cbn_bus = 5; config.cbn_dev = 10;
    config.pid_bus = 0; config.pid_dev = 0;
    config.link_count = 1;
    config.expander[0] = I460GX_BRIDGE_PXB;
    config.smbus = &second.smbus;
    config.memcard_i2c[1][2] = &second.memcard;
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

    /* The PID sits where the board wires it, has fixed identity and no BARs. */
    CHECK(pci_read(&a, 1, 2, 0, 0, 4) == UINT32_C(0x123d8086));
    CHECK(pci_read(&b, 0, 0, 0, 0, 4) == UINT32_C(0x123d8086));
    CHECK(!i460gx_pci_read(&a, 0, 0, 0, 0, 4, &value));
    CHECK(pci_read(&a, 1, 2, 0, 8, 4) == UINT32_C(0x08000001));
    pci_write(&a, 1, 2, 0, 0, 0, 4);
    pci_write(&a, 1, 2, 0, 0x10, UINT32_C(0xffffffff), 4);
    pci_write(&a, 1, 2, 0, 0x48, 0x5a, 1);
    CHECK(pci_read(&a, 1, 2, 0, 0, 4) == UINT32_C(0x123d8086));
    CHECK(pci_read(&a, 1, 2, 0, 0x10, 4) == 0);
    CHECK(pci_read(&a, 1, 2, 0, 0x48, 1) == 0x5a);

    /* The SAC answers every function of devices 0 and 1; other chipset
     * devices answer only the functions they implement. */
    CHECK(pci_read(&a, 0x30, 0, 7, 0, 4) == UINT32_C(0x84e08086));
    CHECK(pci_read(&a, 0x30, 1, 3, 0, 4) == UINT32_C(0x84e08086));
    CHECK(pci_read(&a, 0x30, 0, 0, 0x0e, 1) == 0x80);
    CHECK(pci_read(&a, 0x30, 0, 1, 0x0e, 1) == 0);
    CHECK(pci_read(&a, 0x30, 4, 0, 0, 4) == UINT32_C(0x84e18086));
    CHECK(pci_read(&a, 0x30, 4, 1, 0, 4) == UINT32_C(0xffffffff));
    CHECK(pci_read(&a, 0x30, 0x11, 1, 0, 4) == UINT32_C(0xffffffff));
    CHECK(pci_read(&b, 0xff, 0x10, 2, 0, 4) == UINT32_C(0xffffffff));
    CHECK(pci_read(&a, 0x30, 0x10, 0, 0x0e, 1) == 0x80);
    CHECK(pci_read(&a, 0x30, 0x11, 0, 0x0e, 1) == 0);

    /* Expander window defaults, and the programming function's registers. */
    CHECK(pci_read(&a, 0x30, 0x10, 0, 0x84, 1) == 0x40);
    CHECK(pci_read(&a, 0x30, 0x10, 0, 0x8c, 1) == 0x0f);
    CHECK(pci_read(&a, 3, 8, 0, 0x40, 1) == 0x30);
    CHECK(pci_read(&a, 3, 8, 0, 0x44, 1) == 1);
    pci_write(&a, 3, 8, 0, 0x44, 0x7f, 1);
    CHECK(pci_read(&a, 3, 8, 0, 0x44, 1) == 1);

    /* Memory card identity is fixed. */
    pci_write(&a, 0x30, 6, 0, 0, UINT32_C(0x12345678), 4);
    CHECK(pci_read(&a, 0x30, 6, 0, 0, 4) == UINT32_C(0x84e38086));

    /* Memory card 1 / row 2 / slave 0x56 is reached through the SAC's I2C
     * translation. */
    pci_write(&a, 0x30, 0, 0, 0x68, 0xd6, 1);
    pci_write(&a, 0x30, 6, 6, 0x48, 1, 1);
    CHECK(pci_read(&a, 0x30, 6, 2, 2, 1) == 0x5a);
    pci_write(&a, 0x30, 6, 4, 0x48, 1, 1);
    CHECK(pci_read(&a, 0x30, 6, 2, 2, 1) == 0xff); /* ambiguous row */

    /* The IFB SMBus runs on the board's separate segment, and executes a
     * START only once the host controller is enabled. */
    pci_write(&a, 2, 7, 3, 0x20, 0x1801, 4);
    pci_write(&a, 2, 7, 3, 4, 1, 1);
    pci_write(&b, 4, 9, 3, 0x20, 0x1801, 4);
    pci_write(&b, 4, 9, 3, 4, 1, 1);
    smbus_read(&a);
    CHECK(io_read(&a, 3, 0x1800, 1) == 0);
    pci_write(&a, 2, 7, 3, 0x40, 1, 1);
    pci_write(&b, 4, 9, 3, 0x40, 1, 1);
    smbus_read(&a); smbus_read(&b);
    CHECK(io_read(&a, 3, 0x1800, 1) == 0x02);      /* INTER */
    CHECK(io_read(&a, 3, 0x1805, 1) == 0x5a);
    CHECK(io_read(&b, 3, 0x1805, 1) == 0xa5);
    io_write(&a, 3, 0x1800, 0x02, 1);              /* write one to clear */
    CHECK(io_read(&a, 3, 0x1800, 1) == 0);
    io_write(&a, 3, 0x1804, 0xa1, 1);              /* nothing at slave 0x50 */
    io_write(&a, 3, 0x1802, 0x48, 1);
    CHECK(io_read(&a, 3, 0x1800, 1) == 0x04);      /* DEV_ERR */
    io_write(&a, 3, 0x1800, 0x04, 1);

    /* Word-data read. */
    first.smbus_eeprom.mem[3] = 0x77;
    smbus_read(&a);
    io_write(&a, 3, 0x1804, 0xad, 1);
    io_write(&a, 3, 0x1802, 0x4c, 1);
    CHECK(io_read(&a, 3, 0x1805, 2) == 0x775a);
    io_write(&a, 3, 0x1800, 0x02, 1);

    /* Block write: DAT0 is the count, the data comes from the block array. */
    io_write(&a, 3, 0x1803, 0x10, 1);
    io_write(&a, 3, 0x1804, 0xac, 1);
    io_write(&a, 3, 0x1805, 3, 1);
    CHECK(io_read(&a, 3, 0x1802, 1) == 0x0c);      /* START never reads back */
    io_write(&a, 3, 0x1807, 0xaa, 1);
    io_write(&a, 3, 0x1807, 0xbb, 1);
    io_write(&a, 3, 0x1807, 0xcc, 1);
    io_write(&a, 3, 0x1802, 0x50, 1);
    CHECK(io_read(&a, 3, 0x1800, 1) == 0x02);
    CHECK(first.smbus_eeprom.mem[0x10] == 3 && first.smbus_eeprom.mem[0x11] == 0xaa &&
          first.smbus_eeprom.mem[0x12] == 0xbb && first.smbus_eeprom.mem[0x13] == 0xcc);
    io_write(&a, 3, 0x1800, 0x02, 1);

    /* APM ports are always decoded; a command only raises SMI# once the
     * firmware has enabled it, and the handler's EOS releases it. */
    io_write(&a, 0, 0xb2, 0x37, 1);
    CHECK(io_read(&a, 0, 0xb2, 1) == 0x37);
    CHECK(!first.smi && !second.smi);
    pci_write(&a, 2, 7, 0, 0x40, 0xa01, 4);
    pci_write(&a, 2, 7, 0, 0x44, 1, 1);
    CHECK(io_read(&a, 0, 0xa0c, 2) == 0x0800);     /* PWR_FAIL after power-on */
    io_write(&a, 0, 0xa0c, 0x0800, 2);
    CHECK(io_read(&a, 0, 0xa1a, 2) == 0x0008);     /* reset: EOS only */
    io_write(&a, 0, 0xa1a, 0x0409, 2);             /* APMC_EN, EOS, SMI_EN */
    io_write(&a, 0, 0xb2, 0xa0, 1);
    CHECK(first.smi && first.smi_assertions == 1);
    CHECK(io_read(&a, 0, 0xa1c, 2) == 0x0008);     /* APM_STS */
    CHECK(io_read(&a, 0, 0xa1a, 2) == 0x0401);     /* assertion cleared EOS */
    io_write(&a, 0, 0xa1c, 0x0008, 2);             /* handler clears APM_STS */
    io_write(&a, 0, 0xa1a, 0x0409, 2);             /* ...then sets EOS */
    CHECK(!first.smi && io_read(&a, 0, 0xa1c, 2) == 0);

    /* The power button raises SCI on the line 45h selects once SCI_EN and
     * PWRBTN_EN are set, and the level follows PWRBTN_STS. */
    io_write(&a, 0, 0xa02, 0x0100, 2);             /* PWRBTN_EN */
    io_write(&a, 0, 0xa04, 0x0001, 2);             /* SCI_EN */
    i460gx_pm_power_button(&a, true);
    i460gx_pm_power_button(&a, false);
    CHECK(first.sci_level && first.sci_irq == 9);
    CHECK(io_read(&a, 0, 0xa00, 2) == 0x0100);
    io_write(&a, 0, 0xa00, 0x0100, 2);
    CHECK(!first.sci_level);
    pci_write(&a, 2, 7, 0, 0x45, 3, 1);            /* route SCI to IRQ8 */
    i460gx_pm_power_button(&a, true);
    i460gx_pm_power_button(&a, false);
    CHECK(first.sci_level && first.sci_irq == 8);
    io_write(&a, 0, 0xa00, 0x0100, 2);

    /* Sleep states: SLP_EN with SLP_TYP asks the board to sleep; the power
     * button wakes from any of them and sets WAK_STS. */
    io_write(&a, 0, 0xa04, 0x0001 | (4 << 10) | 0x2000, 2);
    CHECK(first.sleeps == 1 && first.sleep_state == I460GX_PM_S4S5);
    i460gx_pm_power_button(&a, true);
    CHECK(first.wakes == 1 && (io_read(&a, 0, 0xa00, 2) & 0x8000));
    i460gx_pm_power_button(&a, false);
    io_write(&a, 0, 0xa00, 0x8100, 2);

    /* Device timing follows the supplied frequency, independent of Merced.
     * The PM timer counts at exactly three times the 8254 clock. */
    first.ticks = 1000000;
    CHECK(io_read(&a, 0, 0xa08, 4) == 3579546);
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
    CHECK(io_read(&a, 0, 0xb2, 1) == 0x73);
    CHECK(io_read(&b, 3, 0x1805, 1) == 0xa5);

    /* Firmware hubs: four 1 MB parts, ID n holding megabyte 3-n of the
     * board's 4 MB image, selected by FWHDE/FWHS. */
    static uint8_t flash[4u << 20];
    for (size_t i = 0; i < sizeof(flash); i++)
        flash[i] = (uint8_t)(i * 7 + 3);
    a.config.flash = flash;
    const uint64_t window = 0xffc00000;
    CHECK(i460gx_flash_access(&a, false, window, 1, &value) && value == flash[0]);
    CHECK(i460gx_flash_access(&a, false, 0xfffffff0, 4, &value));
    CHECK(value == (uint32_t)(flash[0x3ffff0] | flash[0x3ffff1] << 8 |
                              flash[0x3ffff2] << 16 | (uint32_t)flash[0x3ffff3] << 24));
    CHECK(!i460gx_flash_access(&a, false, 0x80000000, 1, &value));

    /* A write is master-aborted until BIOS_WEN is set. */
    value = 0x90;
    CHECK(i460gx_flash_access(&a, true, window + 0x10000, 1, &value));
    CHECK(i460gx_flash_access(&a, false, window + 0x10000, 1, &value) &&
          value == flash[0x10000]);
    pci_write(&a, 2, 7, 0, 0x4f, 0x87, 1);         /* BLE */
    pci_write(&a, 2, 7, 0, 0x4e, 0xc5, 1);         /* BIOS_WEN */
    CHECK(first.smi);                              /* BLE ignores SMI_EN */
    value = 0x90;
    i460gx_flash_access(&a, true, window + 0x10000, 1, &value);
    i460gx_flash_access(&a, false, window + 0x10000, 1, &value);
    CHECK(value == 0x89);
    i460gx_flash_access(&a, false, window + 0x10001, 1, &value);
    CHECK(value == 0xac);
    value = 0xff;
    i460gx_flash_access(&a, true, window + 0x10000, 1, &value);
    i460gx_flash_access(&a, false, window + 0x10000, 1, &value);
    CHECK(value == flash[0x10000]);

    /* Blocks start write-locked: a program fails with DPS and PS set. */
    value = 0x40;
    i460gx_flash_access(&a, true, window + 0x20000, 1, &value);
    value = 0x00;
    i460gx_flash_access(&a, true, window + 0x20000, 1, &value);
    i460gx_flash_access(&a, false, window + 0x20000, 1, &value);
    CHECK(value == 0x92);
    value = 0x50;                                  /* clear status */
    i460gx_flash_access(&a, true, window + 0x20000, 1, &value);
    value = 0xff;                                  /* read array */
    i460gx_flash_access(&a, true, window + 0x20000, 1, &value);

    /* Unlock with 91h and the lock register at block+2, then program. */
    uint32_t offset, length;
    CHECK(!i460gx_flash_take_changes(&a, &offset, &length));
    flash[0x20000] = 0xf0;
    value = 0x91;
    i460gx_flash_access(&a, true, window + 0x20000, 1, &value);
    value = 0x00;
    i460gx_flash_access(&a, true, window + 0x20002, 1, &value);
    value = 0xff;
    i460gx_flash_access(&a, true, window + 0x20000, 1, &value);
    value = 0x40;
    i460gx_flash_access(&a, true, window + 0x20000, 1, &value);
    value = 0x3c;
    i460gx_flash_access(&a, true, window + 0x20000, 1, &value);
    CHECK(flash[0x20000] == 0x30);                 /* programming clears bits */
    CHECK(i460gx_flash_take_changes(&a, &offset, &length));
    CHECK(offset == 0x20000 && length == 1);
    i460gx_flash_access(&a, false, window + 0x20000, 1, &value);
    CHECK(value == 0x00);                          /* busy for 17 us */
    first.ticks += 20;
    i460gx_flash_access(&a, false, window + 0x20000, 1, &value);
    CHECK(value == 0x80);
    value = 0xff;
    i460gx_flash_access(&a, true, window + 0x20000, 1, &value);

    /* Block erase; the register space four MB below holds FGPI. */
    value = 0x20;
    i460gx_flash_access(&a, true, window + 0x20000, 1, &value);
    value = 0xd0;
    i460gx_flash_access(&a, true, window + 0x20000, 1, &value);
    CHECK(flash[0x20000] == 0xff && flash[0x2ffff] == 0xff);
    CHECK(i460gx_flash_take_changes(&a, &offset, &length));
    CHECK(offset == 0x20000 && length == 0x10000);
    CHECK(i460gx_flash_access(&a, false, 0xff8c0100, 1, &value) && value == 0x1f);

    /* FWHDE masks a granule: it master-aborts and reads all ones. */
    first.ticks += 1000000;                        /* the erase finishes (0.8 s) */
    value = 0xff;                                  /* read array again */
    i460gx_flash_access(&a, true, window + 0x20000, 1, &value);
    pci_write(&a, 2, 7, 0, 0xe3, 0xfe, 1);
    value = 0;
    CHECK(i460gx_flash_access(&a, false, window, 1, &value) && value == 0xff);
    CHECK(i460gx_flash_access(&a, false, window + 0x80000, 1, &value) &&
          value == flash[0x80000]);
    printf("460GX standalone: %u checks passed\n", checks);
    return 0;
}
