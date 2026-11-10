/* Regression coverage for the 460GX memory-card SPD configuration window.
 * Uses the public physical bus interface; no firmware image is required.
 * Run with: python3 tools/ia64/run_i2000_memory_test.py */
#include "i2000.h"
#include "merced.h"
#include "gemu/monitor.h"
#include <inttypes.h>
#include <stdio.h>

#define IO_BASE UINT64_C(0x00000ffffc000000)
#define CHIPSET_BUS 0xffu
#define SPD_SELECT 0x68u

static unsigned failures;

static uint64_t io_address(unsigned port) {
    return IO_BASE + (((uint64_t)port >> 2) << 12 | (port & 0xfff));
}

static void config_select_bus(Ia64I2000State *s, unsigned bus, unsigned dev,
                              unsigned fun, unsigned reg) {
    uint32_t address = UINT32_C(0x80000000) | (bus << 16) |
                       (dev << 11) | (fun << 8) | (reg & 0xfc);
    ia64_i2000_phys_write32(s, io_address(0xcf8), address);
}

static void config_select(Ia64I2000State *s, unsigned dev, unsigned fun,
                          unsigned reg) {
    config_select_bus(s, CHIPSET_BUS, dev, fun, reg);
}

static uint8_t config_read8(Ia64I2000State *s, unsigned dev, unsigned fun,
                            unsigned reg) {
    config_select(s, dev, fun, reg);
    return ia64_i2000_phys_read8(s, io_address(0xcfc + (reg & 3)));
}

static uint32_t config_read32(Ia64I2000State *s, unsigned dev, unsigned fun,
                              unsigned reg) {
    config_select(s, dev, fun, reg);
    return ia64_i2000_phys_read32(s, io_address(0xcfc + (reg & 3)));
}

static void config_write8(Ia64I2000State *s, unsigned dev, unsigned fun,
                           unsigned reg, uint8_t value) {
    config_select(s, dev, fun, reg);
    ia64_i2000_phys_write8(s, io_address(0xcfc + (reg & 3)), value);
}

static void config_write32(Ia64I2000State *s, unsigned dev, unsigned fun,
                            unsigned reg, uint32_t value) {
    config_select(s, dev, fun, reg);
    ia64_i2000_phys_write32(s, io_address(0xcfc + (reg & 3)), value);
}

static void select_row(Ia64I2000State *s, unsigned row) {
    for (unsigned fun = 4; fun < 8; fun++)
        config_write8(s, 5, fun, 0x48, 0);
    config_write8(s, 5, 4 + row, 0x48, 1);
}

static void expect(uint64_t actual, uint64_t expected, const char *what,
                   unsigned ram_mb, unsigned row) {
    if (actual == expected)
        return;
    fprintf(stderr, "%u MiB row %u: %s: got 0x%" PRIx64
                    ", expected 0x%" PRIx64 "\n",
            ram_mb, row, what, actual, expected);
    failures++;
}

static void cmd649_select(Ia64I2000State *s, unsigned reg) {
    ia64_i2000_phys_write32(s, io_address(0xcf8),
                           UINT32_C(0x80001900) | (reg & 0xfc));
}

static uint32_t cmd649_read(Ia64I2000State *s, unsigned reg) {
    cmd649_select(s, reg);
    return ia64_i2000_phys_read32(s, io_address(0xcfc));
}

static void cmd649_write(Ia64I2000State *s, unsigned reg, uint32_t value) {
    cmd649_select(s, reg);
    ia64_i2000_phys_write32(s, io_address(0xcfc), value);
}

static void check_ifb_config(void) {
    Ia64Config cfg = {.ram_size = UINT64_C(64) << 20,
                      .display_type = GEMU_DISPLAY_NONE};
    Ia64I2000State *s = ia64_i2000_create(&cfg);
    if (!s) { failures++; return; }
    const unsigned functions[] = {0, 2, 3};
    for (unsigned f = 0; f < 3; f++) {
        unsigned fun = functions[f];
        for (unsigned bar = 0; bar < 6; bar++) {
            unsigned reg = 0x10 + 4 * bar;
            bool implemented = fun != 0 && bar == 4;
            config_select_bus(s, 0, 3, fun, reg);
            expect(ia64_i2000_phys_read32(s, io_address(0xcfc)), implemented,
                   "IFB reset BAR layout", 64, fun);
            ia64_i2000_phys_write32(s, io_address(0xcfc), UINT32_MAX);
            uint32_t expected = implemented ? (fun == 2 ? 0xffe1 : 0xfff1) : 0;
            expect(ia64_i2000_phys_read32(s, io_address(0xcfc)), expected,
                   "IFB BAR sizing and 16-bit I/O address mask", 64, fun);
            ia64_i2000_phys_write32(s, io_address(0xcfc), 0xdead103f);
            expected = implemented ? (fun == 2 ? 0x1021 : 0x1031) : 0;
            uint32_t bytes = 0;
            for (unsigned i = 0; i < 4; i++)
                bytes |= (uint32_t)ia64_i2000_phys_read8(s, io_address(0xcfc + i)) << (i * 8);
            expect(bytes, expected, "IFB assigned BAR alignment and byte lanes", 64, fun);
            ia64_i2000_phys_write8(s, io_address(0xcfc), 0);
            expect(ia64_i2000_phys_read32(s, io_address(0xcfc)), implemented ? 0x1001 : 0,
                   "IFB BAR type survives byte writes", 64, fun);
        }
        config_select_bus(s, 0, 3, fun, 0x30);
        ia64_i2000_phys_write32(s, io_address(0xcfc), UINT32_MAX);
        expect(ia64_i2000_phys_read32(s, io_address(0xcfc)), 0,
               "IFB has no expansion ROM resource", 64, fun);
        config_select_bus(s, 0, 3, fun, 0x0c);
        ia64_i2000_phys_write8(s, io_address(0xcfe), 0xff);
        expect(ia64_i2000_phys_read8(s, io_address(0xcfe)), fun == 0 ? 0x80 : 0,
               "IFB header type is read-only", 64, fun);
        if (fun) {
            config_select_bus(s, 0, 3, fun, 4);
            ia64_i2000_phys_write32(s, io_address(0xcfc), 0xffff);
            for (unsigned i = 0; i < 3; i++)
                expect(ia64_i2000_phys_read32(s, io_address(0xcfc)) & 0xffff,
                       fun == 2 ? 5 : 1, "IFB command enable is a stable latch", 64, fun);
        }
    }
    /* Use two non-default addresses so the SMBus transaction cannot be
     * satisfied by the former fixed-address idle-status shims. */
    config_select_bus(s, 0, 3, 3, 0x20);
    ia64_i2000_phys_write32(s, io_address(0xcfc), 0x1a00);
    expect(ia64_i2000_phys_read8(s, io_address(0x1a00)), 0,
           "SMBus status follows assigned I/O BAR", 64, 3);
    ia64_i2000_phys_write8(s, io_address(0x1a03), 2); /* SPD memory type */
    ia64_i2000_phys_write8(s, io_address(0x1a04), 0xa1); /* slave50 read */
    ia64_i2000_phys_write8(s, io_address(0x1a02), 0x48); /* byte-data start */
    expect(ia64_i2000_phys_read8(s, io_address(0x1a00)), 2,
           "SMBus transaction completes at assigned base", 64, 3);
    expect(ia64_i2000_phys_read8(s, io_address(0x1a05)), 4,
           "SMBus reads populated SDRAM EEPROM", 64, 3);
    ia64_i2000_phys_write8(s, io_address(0x1a00), 2);
    expect(ia64_i2000_phys_read8(s, io_address(0x1a00)), 0,
           "SMBus completion is write-one-to-clear", 64, 3);
    config_select_bus(s, 0, 3, 3, 0x20);
    ia64_i2000_phys_write32(s, io_address(0xcfc), 0x1b00);
    expect(ia64_i2000_phys_read8(s, io_address(0x1a00)), 0xff,
           "SMBus old I/O address is unmapped", 64, 3);
    expect(ia64_i2000_phys_read8(s, io_address(0x1b05)), 4,
           "SMBus relocation preserves register state", 64, 3);
    config_select_bus(s, 0, 3, 3, 4);
    ia64_i2000_phys_write8(s, io_address(0xcfc), 0);
    expect(ia64_i2000_phys_read8(s, io_address(0x1b00)), 0xff,
           "SMBus PCI I/O disable gates decode", 64, 3);
    config_select_bus(s, 0, 3, 2, 4);
    ia64_i2000_phys_write8(s, io_address(0xcfc), 0);
    expect(ia64_i2000_phys_read8(s, io_address(0x1000)), 0xff,
           "unassigned former SMBus fallback is unmapped", 64, 3);
    ia64_i2000_destroy(s);
}

static void check_cmd649_config(void) {
    Ia64Config cfg = {.ram_size = UINT64_C(64) << 20,
                      .display_type = GEMU_DISPLAY_NONE};
    Ia64I2000State *s = ia64_i2000_create(&cfg);
    if (!s) { failures++; return; }
    static const unsigned sizes[] = {8, 4, 8, 4, 16};
    expect((cmd649_read(s, 0x50) >> 8) & 0x0c, 0x0c,
           "both IDE channels are enabled at reset", 64, 0);
    /* CmdIde reads the channel-enable bits before publishing channel
     * devices. The secondary channel must exist even without an HDD on
     * the primary channel; the bits remain independently writable. */
    cmd649_select(s, 0x50);
    ia64_i2000_phys_write8(s, io_address(0xcfd), 8);
    expect((cmd649_read(s, 0x50) >> 8) & 0x0c, 8,
           "secondary IDE enable is independent of primary", 64, 1);
    cmd649_select(s, 0x50);
    ia64_i2000_phys_write8(s, io_address(0xcfd), 4);
    expect((cmd649_read(s, 0x50) >> 8) & 0x0c, 4,
           "primary IDE enable is independent of secondary", 64, 0);
    static const uint32_t assigned[] = {
        0x12345679, 0x1234567d, 0x12345679, 0x1234567d, 0x12345671,
    };
    for (unsigned bar = 0; bar < 5; bar++) {
        unsigned reg = 0x10 + bar * 4;
        expect(cmd649_read(s, reg), 1, "CMD649 reset I/O BAR", 64, bar);
        cmd649_write(s, reg, UINT32_MAX);
        uint32_t probe = cmd649_read(s, reg);
        expect(probe & 3, 1, "CMD649 hardwired I/O BAR type", 64, bar);
        expect(~(probe & ~UINT32_C(3)) + 1, sizes[bar],
               "CMD649 PCI resource sizing", 64, bar);
        cmd649_write(s, reg, 0x1234567f);
        expect(cmd649_read(s, reg), assigned[bar],
               "CMD649 I/O base alignment", 64, bar);
        cmd649_select(s, reg);
        uint32_t bytes = 0;
        for (unsigned i = 0; i < 4; i++)
            bytes |= (uint32_t)ia64_i2000_phys_read8(s, io_address(0xcfc + i)) << (i * 8);
        expect(bytes, assigned[bar], "CMD649 byte/dword BAR agreement", 64, bar);
        ia64_i2000_phys_write8(s, io_address(0xcfc), 0);
        expect(cmd649_read(s, reg), 0x12345601,
               "CMD649 byte write preserves I/O type", 64, bar);
        cmd649_select(s, reg);
        ia64_i2000_phys_write8(s, io_address(0xcfd), 0xab);
        expect(cmd649_read(s, reg), 0x1234ab01,
               "CMD649 partial address write preserves other lanes", 64, bar);
        cmd649_write(s, reg, 0);
        expect(cmd649_read(s, reg), 1, "CMD649 zero write preserves type", 64, bar);
    }
    cmd649_write(s, 0x24, UINT32_MAX);
    expect(cmd649_read(s, 0x24), 0, "CMD649 unimplemented BAR5 sizing", 64, 5);
    cmd649_select(s, 0x24);
    ia64_i2000_phys_write8(s, io_address(0xcfc), 0xff);
    expect(cmd649_read(s, 0x24), 0, "CMD649 unimplemented BAR5 byte write", 64, 5);
    cmd649_write(s, 0, UINT32_MAX);
    expect(cmd649_read(s, 0), 0x06491095, "CMD649 read-only identity", 64, 0);
    ia64_i2000_destroy(s);
}

static void check_chipset_bus_discovery(void) {
    Ia64Config cfg = {.ram_size = UINT64_C(64) << 20,
                      .display_type = GEMU_DISPLAY_NONE};
    Ia64I2000State *s = ia64_i2000_create(&cfg);
    if (!s) { failures++; return; }
    uint32_t identity = config_read32(s, 0, 0, 0);
    /* F16NUM counts physical expander links, each with two bus windows.
     * Firmware combines it with the active-low DEVNPRES device bitmap.
     * Advertising 32 links makes the walk run past that 32-bit bitmap
     * and wrap the PCI device number, inventing 48 extra root buses. */
    expect(config_read8(s, 0, 0, 0x60), 4, "SAC physical expander count", 64, 0);
    expect(config_read32(s, 0, 0, 0x60), 4, "SAC expander count dword access", 64, 0);
    config_write8(s, 0, 0, 0x60, 32);
    expect(config_read8(s, 0, 0, 0x60), 4, "physical expander count is fixed", 64, 0);
    config_write32(s, 0, 0, 0x70, UINT32_C(0xfffcffec));
    unsigned roots = 0;
    unsigned count = config_read8(s, 0, 0, 0x60);
    uint64_t absent = config_read32(s, 0, 0, 0x70);
    for (unsigned link = 0; link < count; link++) {
        unsigned dev = 0x10 + 2 * link;
        if (dev >= 32 || !(absent & (UINT64_C(1) << dev)))
            roots += 2;
    }
    expect(roots, 2, "populated expander yields exactly two root buses", 64, 0);
    config_select_bus(s, 0, 0x10, 0, 0);
    expect(ia64_i2000_phys_read32(s, io_address(0xcfc)), identity,
           "fixed CBN function has SAC identity", 64, 0);
    config_select_bus(s, 0, 0x10, 0, 0x0c);
    expect(ia64_i2000_phys_read8(s, io_address(0xcfe)), 0,
           "fixed CBN function has valid header", 64, 0);
    config_select_bus(s, 0, 0x10, 0, 0x40);
    expect(ia64_i2000_phys_read32(s, io_address(0xcfc)), CHIPSET_BUS,
           "CBN register supports dword reads", 64, 0);
    ia64_i2000_phys_write8(s, io_address(0xcfc), 0x32);
    expect(ia64_i2000_phys_read32(s, io_address(0xcfc)), 0x32,
           "CBN byte relocation remains discoverable", 64, 0);
    config_select_bus(s, 0x32, 0, 0, 0);
    expect(ia64_i2000_phys_read32(s, io_address(0xcfc)), identity,
           "chipset identity follows relocated bus", 64, 0);
    expect(config_read32(s, 0, 0, 0), UINT32_MAX,
           "old chipset bus is absent after relocation", 64, 0);
    /* The OS checks the fixed function's identity before reading CBNO. */
    config_select_bus(s, 0, 0x10, 0, 0);
    ia64_i2000_phys_write32(s, io_address(0xcfc), UINT32_MAX);
    expect(ia64_i2000_phys_read32(s, io_address(0xcfc)), identity,
           "fixed CBN identity is read-only after relocation", 64, 0);
    config_select_bus(s, 0, 0x10, 0, 0x10);
    ia64_i2000_phys_write32(s, io_address(0xcfc), UINT32_MAX);
    expect(ia64_i2000_phys_read32(s, io_address(0xcfc)), 0,
           "fixed CBN function has no BAR resource", 64, 0);
    config_select_bus(s, 0, 0x10, 0, 0x40);
    ia64_i2000_phys_write32(s, io_address(0xcfc), UINT32_C(0xabcdef41));
    expect(ia64_i2000_phys_read32(s, io_address(0xcfc)), 0x41,
           "CBN dword relocation only changes the bus byte", 64, 0);
    config_select_bus(s, 0x41, 0, 0, 0);
    expect(ia64_i2000_phys_read32(s, io_address(0xcfc)), identity,
           "chipset follows dword CBN write", 64, 0);
    ia64_i2000_destroy(s);
}

static void check_chipset_topology(void) {
    Ia64Config cfg = {.ram_size = UINT64_C(64) << 20,
                      .display_type = GEMU_DISPLAY_NONE};
    Ia64I2000State *s = ia64_i2000_create(&cfg);
    if (!s) { failures++; return; }
    /* The i2000 has one 64-bit PXB bus, two WXB buses, and one AGP bus.
     * Their SAC windows live at 10h, 12h/13h, and 14h on bus CBN. */
    expect(config_read32(s, 4, 0, 0), UINT32_C(0x84e18086),
           "SDC identity", 64, 4);
    for (unsigned dev = 0x10; dev <= 0x17; dev++)
        expect(config_read32(s, dev, 0, 0), UINT32_C(0x84e08086),
               "expander function zero is a SAC window", 64, dev);
    static const struct { unsigned dev, fun; uint32_t identity; } bridges[] = {
        {0x10, 1, UINT32_C(0x84cb8086)}, /* PXB */
        {0x12, 1, UINT32_C(0x84e68086)}, /* WXB bus A */
        {0x13, 1, UINT32_C(0x84e68086)}, /* WXB bus B */
        {0x14, 1, UINT32_C(0x84ea8086)}, /* GXB */
        {0x14, 2, UINT32_C(0x84e28086)}, /* GXB GART */
    };
    for (unsigned i = 0; i < sizeof(bridges) / sizeof(bridges[0]); i++) {
        unsigned dev = bridges[i].dev, fun = bridges[i].fun;
        expect(config_read32(s, dev, fun, 0), bridges[i].identity,
               "populated expander identity", 64, dev);
        config_write32(s, dev, fun, 0, UINT32_MAX);
        expect(config_read32(s, dev, fun, 0), bridges[i].identity,
               "expander identity is read-only", 64, dev);
        config_write32(s, dev, fun, 8, UINT32_MAX);
        expect(config_read32(s, dev, fun, 8) >> 8,
               fun == 2 ? 0x068000 : 0x060000,
               "bridge and GART classes are read-only", 64, dev);
    }
    for (unsigned link = 0; link < 6; link++) {
        config_write8(s, 0, 0, 0x64, (uint8_t)link);
        unsigned status = link | 0x80 | (link < 4 ? 0x40 : 0);
        expect(config_read8(s, 0, 0, 0x64), status,
               "link probe completes and reports physical presence", 64, link);
        expect(config_read32(s, 0, 0, 0x64), status,
               "link probe byte and dword access agree", 64, link);
    }
    const unsigned ports[] = {0x10, 0x12, 0x14};
    const unsigned active[] = {1, 2, 1};
    for (unsigned i = 0; i < 3; i++) {
        unsigned dev = ports[i];
        config_write32(s, dev, 0, 0x40, UINT32_C(0xa5fc5a00));
        expect(config_read32(s, dev, 0, 0x40), UINT32_C(0xa5fe5a00),
               "SAC capacity preserves other byte lanes and writable bits", 64, dev);
        config_write8(s, dev, 0, 0x42, 4);
        expect(config_read8(s, dev, 0, 0x42), 6,
               "unused-window disable preserves two-window capacity", 64, dev);
        config_write32(s, dev, 1, 0x80, UINT32_C(0x123456fc));
        expect(config_read32(s, dev, 1, 0x80), UINT32_C(0x123456fc) | active[i],
               "bridge active bus count preserves writable bits", 64, dev);
    }
    ia64_i2000_destroy(s);
}

static void check_acpi_enable(void) {
    Ia64Config cfg = {.ram_size = UINT64_C(64) << 20,
                     .display_type = GEMU_DISPLAY_NONE};
    Ia64I2000State *s = ia64_i2000_create(&cfg);
    if (!s) { failures++; return; }
    /* IFB 00:03.0 PMBASE and ACPI_CNTL, matching the firmware's FADT. */
    ia64_i2000_phys_write32(s, io_address(0xcf8), 0x80001840);
    ia64_i2000_phys_write32(s, io_address(0xcfc), 0x0a01);
    expect(ia64_i2000_phys_read32(s, io_address(0xa04)), UINT32_MAX,
           "ACPI decode is initially disabled", 64, 0);
    ia64_i2000_phys_write32(s, io_address(0xcf8), 0x80001844);
    ia64_i2000_phys_write8(s, io_address(0xcfc), 1);
    expect(ia64_i2000_phys_read32(s, io_address(0xa04)), 0,
           "ACPI begins in legacy mode", 64, 0);
    ia64_i2000_phys_write8(s, io_address(0xb2), 0x55);
    expect(ia64_i2000_phys_read8(s, io_address(0xa04)), 0,
           "unrelated APM command does not enable SCI", 64, 0);
    ia64_i2000_phys_write8(s, io_address(0xb2), 0xa0);
    expect(ia64_i2000_phys_read8(s, io_address(0xa04)), 1,
           "FADT enable command sets SCI_EN", 64, 0);
    expect(ia64_i2000_phys_read8(s, io_address(0xb2)), 0xa0,
           "APM command latch", 64, 0);
    expect(ia64_i2000_phys_read32(s, io_address(0x404)), 0x01000000,
           "ACPI control does not alias GPIO strap", 64, 0);
    ia64_i2000_phys_write8(s, io_address(0xa05), 4);
    ia64_i2000_phys_write8(s, io_address(0xb2), 0xa1);
    expect(ia64_i2000_phys_read32(s, io_address(0xa04)), 0x400,
           "FADT disable preserves other PM1 control bits", 64, 0);
    ia64_i2000_phys_write32(s, io_address(0xa00), UINT32_MAX);
    expect(ia64_i2000_phys_read32(s, io_address(0xa00)), 0xffff0000,
           "PM1 status is write-one-to-clear; enables are retained", 64, 0);
    uint32_t timer = ia64_i2000_phys_read32(s, io_address(0xa08));
    ia64_i2000_phys_write32(s, io_address(0xa08), UINT32_MAX);
    expect(ia64_i2000_phys_read32(s, io_address(0xa08)), timer,
           "PM timer ignores writes while guest time is stopped", 64, 0);
    ia64_i2000_phys_write32(s, io_address(0xcf8), 0x80001840);
    ia64_i2000_phys_write32(s, io_address(0xcfc), 0x0c01);
    expect(ia64_i2000_phys_read32(s, io_address(0xc04)), 0x400,
           "PMBASE relocation retains register state", 64, 0);
    expect(ia64_i2000_phys_read32(s, io_address(0xa04)), UINT32_MAX,
           "old ACPI port range no longer decodes", 64, 0);
    ia64_i2000_destroy(s);
}

static void check_uhci_empty(void) {
    Ia64Config cfg = {.ram_size = UINT64_C(64) << 20,
                     .display_type = GEMU_DISPLAY_NONE};
    Ia64I2000State *s = ia64_i2000_create(&cfg);
    if (!s) { failures++; return; }
    ia64_i2000_phys_write32(s, io_address(0xcf8), 0x80001a20);
    ia64_i2000_phys_write32(s, io_address(0xcfc), 0x1201);
    ia64_i2000_phys_write32(s, io_address(0xcf8), 0x80001a04);
    ia64_i2000_phys_write8(s, io_address(0xcfc), 1);
    expect(ia64_i2000_phys_read32(s, io_address(0x1200)), 0x00200000,
           "UHCI resets stopped without pending events", 64, 0);
    expect(ia64_i2000_phys_read32(s, io_address(0x1210)), 0x00800080,
           "UHCI has two disconnected ports", 64, 0);
    expect(ia64_i2000_phys_read8(s, io_address(0x120c)), 0x40,
           "UHCI reset SOFMOD", 64, 0);
    ia64_i2000_phys_write32(s, io_address(0x1200), 0x41);
    expect(ia64_i2000_phys_read32(s, io_address(0x1200)), 0x41,
           "UHCI run clears halted status", 64, 0);
    ia64_i2000_phys_write32(s, io_address(0x1200), 0x40);
    expect(ia64_i2000_phys_read32(s, io_address(0x1200)), 0x00200040,
           "UHCI stop reports halted status", 64, 0);
    ia64_i2000_phys_write32(s, io_address(0x1206), 0x7ff);
    expect(ia64_i2000_phys_read32(s, io_address(0x1206)) & 0xffff, 0x7ff,
           "UHCI stopped frame number is programmable", 64, 0);
    ia64_i2000_phys_write32(s, io_address(0x1208), 0x12345fff);
    expect(ia64_i2000_phys_read32(s, io_address(0x1208)), 0x12345000,
           "UHCI frame list base is page aligned", 64, 0);
    ia64_i2000_phys_write32(s, io_address(0x1200), 2);
    expect(ia64_i2000_phys_read32(s, io_address(0x1200)), 0x00200000,
           "UHCI host reset completes and self-clears", 64, 0);
    expect(ia64_i2000_phys_read32(s, io_address(0x1208)), 0,
           "UHCI host reset clears schedule base", 64, 0);
    ia64_i2000_phys_write32(s, io_address(0xcf8), 0x80001a20);
    ia64_i2000_phys_write32(s, io_address(0xcfc), 0x1401);
    expect(ia64_i2000_phys_read32(s, io_address(0x1400)), 0x00200000,
           "UHCI follows relocated I/O BAR", 64, 0);
    expect(ia64_i2000_phys_read32(s, io_address(0x1200)), UINT32_MAX,
           "UHCI no longer decodes old BAR", 64, 0);
    ia64_i2000_phys_write32(s, io_address(0xcf8), 0x80001a04);
    ia64_i2000_phys_write8(s, io_address(0xcfc), 0);
    expect(ia64_i2000_phys_read32(s, io_address(0x1400)), UINT32_MAX,
           "UHCI honors PCI I/O enable", 64, 0);
    ia64_i2000_destroy(s);
}

static void check_memory(unsigned ram_mb) {
    Ia64Config cfg = {
        .ram_size = (uint64_t)ram_mb << 20,
        .display_type = GEMU_DISPLAY_NONE,
    };
    Ia64I2000State *s = ia64_i2000_create(&cfg);
    if (!s) {
        fprintf(stderr, "%u MiB: machine creation failed\n", ram_mb);
        failures++;
        return;
    }

    uint64_t installed = 0;
    unsigned populated = 0;
    for (unsigned selector = 0; selector < 32; selector++) {
        unsigned row = selector / 8;
        unsigned slot = selector % 8;
        select_row(s, row);
        config_write8(s, 0, 0, SPD_SELECT, (uint8_t)(0xd0 + slot));
        uint8_t spd[64];
        for (unsigned i = 0; i < sizeof(spd); i++)
            spd[i] = config_read8(s, 5, 2, i);
        for (unsigned i = 0; i < sizeof(spd); i += 4) {
            uint32_t bytes = (uint32_t)spd[i] | (uint32_t)spd[i + 1] << 8 |
                             (uint32_t)spd[i + 2] << 16 |
                             (uint32_t)spd[i + 3] << 24;
            expect(config_read32(s, 5, 2, i), bytes,
                   "PCI byte/dword read agreement", ram_mb, row);
        }
        uint8_t absent_type = config_read8(s, 6, 2, 2);
        expect(absent_type == 0 || absent_type == 0xff, 1,
               "unpopulated second memory card", ram_mb, row);
        if (spd[2] != 4) {
            expect(spd[2] == 0 || spd[2] == 0xff, 1,
                   "absent row has no SDRAM identity", ram_mb, row);
            continue;
        }

        populated++;
        unsigned sum = 0;
        for (unsigned i = 0; i < 63; i++)
            sum += spd[i];
        expect(spd[63], sum & 0xff, "SPD checksum", ram_mb, row);

        /* Decode JEDEC module geometry independently of the platform's
         * SPD encoder, then count each populated module exactly once. */
        unsigned address_bits = spd[3] + spd[4];
        if (address_bits >= 32) {
            fprintf(stderr, "%u MiB row %u: invalid SPD address geometry\n",
                    ram_mb, row);
            failures++;
            continue;
        }
        unsigned width = (unsigned)spd[6] | (unsigned)spd[7] << 8;
        uint64_t module = (UINT64_C(1) << address_bits) *
                          (width / 8) * spd[17] * spd[5];
        /* BIOS 1.17c recognizes these device organizations.  Capacity
         * alone is insufficient: the former 13-row/9-column/four-bank
         * encoding also describes 128 MiB, but SAL leaves its geometry
         * descriptor unconverted and treats it as a memory size. */
        unsigned geometry = (unsigned)spd[3] << 16 |
                            (unsigned)spd[4] << 8 | spd[17];
        expect(geometry == 0x0b0902 || geometry == 0x0b0a02 ||
               geometry == 0x0c0904 || geometry == 0x0c0a04 ||
               geometry == 0x0d0a04, 1,
               "firmware-supported SDRAM organization", ram_mb, row);
        installed += module;
    }
    expect(installed, cfg.ram_size, "firmware-visible installed RAM",
           ram_mb, 0);
    expect(populated > 0 && populated <= 8 && populated % 4 == 0, 1,
           "populated DIMM count supports quartets",
           ram_mb, 0);

    /* Re-select the first row after probing absent rows: its contents must
     * follow IIADR, rather than remain stuck on the last PCI function. */
    select_row(s, 0);
    config_write8(s, 0, 0, SPD_SELECT, 0xd0);
    expect(config_read8(s, 5, 2, 2), 4,
           "SPD selector round trip", ram_mb, 0);

    /* Disable IIADR without changing its slave address.  Re-enabling must
     * expose the EEPROM, while writes to that read-only window must not
     * alter either the SPD or the underlying configuration latches. */
    config_write8(s, 0, 0, SPD_SELECT, 0x50);
    uint32_t disabled = config_read32(s, 5, 2, 0);
    expect(config_read8(s, 5, 2, 2) != 4, 1,
           "disabled I2C selector hides SPD", ram_mb, 0);
    config_write8(s, 0, 0, SPD_SELECT, 0xd0);
    uint8_t original[64];
    for (unsigned i = 0; i < sizeof(original); i++)
        original[i] = config_read8(s, 5, 2, i);
    config_write32(s, 5, 2, 0, ~config_read32(s, 5, 2, 0));
    config_write8(s, 5, 2, 63, (uint8_t)~original[63]);
    for (unsigned i = 0; i < sizeof(original); i++)
        expect(config_read8(s, 5, 2, i), original[i],
               "SPD EEPROM is read-only", ram_mb, 0);
    config_write8(s, 0, 0, SPD_SELECT, 0x50);
    expect(config_read32(s, 5, 2, 0), disabled,
           "EEPROM writes do not reach configuration latches", ram_mb, 0);
    config_write8(s, 0, 0, SPD_SELECT, 0xd8);
    expect(config_read8(s, 5, 2, 2) != 4, 1,
           "unassigned I2C slave hides SPD", ram_mb, 0);

    config_write8(s, 0, 0, SPD_SELECT, 0xd0);
    for (unsigned fun = 4; fun < 8; fun++)
        config_write8(s, 5, fun, 0x48, 0);
    expect(config_read8(s, 5, 2, 2) != 4, 1,
           "disabled row selectors hide SPD", ram_mb, 0);
    config_write8(s, 5, 4, 0x48, 1);
    config_write8(s, 5, 5, 0x48, 1);
    expect(config_read8(s, 5, 2, 2) != 4, 1,
           "conflicting row selectors hide SPD", ram_mb, 0);
    select_row(s, 0);
    expect(config_read8(s, 5, 2, 2), 4,
           "restored row selector exposes SPD", ram_mb, 0);

    ia64_i2000_phys_write8(s, UINT64_C(0x10000), 0xa5);
    ia64_i2000_phys_write8(s, cfg.ram_size - 1, 0x5a);
    expect(ia64_i2000_phys_read8(s, UINT64_C(0x10000)), 0xa5,
           "low RAM backing", ram_mb, 0);
    expect(ia64_i2000_phys_read8(s, cfg.ram_size - 1), 0x5a,
           "last installed RAM byte", ram_mb, 0);
    expect(ia64_i2000_phys_read8(s, cfg.ram_size), 0xff,
           "installed RAM boundary", ram_mb, 0);

    uint64_t scratch = cfg.ram_size - UINT64_C(0x10000) + 0xa030;
    ia64_i2000_phys_write8(s, scratch, 0x6d);
    expect(ia64_i2000_phys_read8(s, UINT64_C(0xffffffffffffa030)), 0x6d,
           "SAL scratch alias follows installed RAM", ram_mb, 0);
    ia64_i2000_destroy(s);
}

/* An MII bundle containing three nops and an end-of-bundle stop.  Every
 * address returns valid code so these tests isolate interrupt recognition
 * from instruction decoding and physical-memory allocation. */
static uint64_t handoff_read(void *ud, uint64_t addr, unsigned size) {
    (void)ud;
    const __uint128_t nop = UINT64_C(0x08000000);
    __uint128_t bundle = 1 | (nop << 5) | (nop << 46) | (nop << 87);
    uint64_t value = (uint64_t)(bundle >> ((addr & 15) * 8));
    return size == 8 ? value : value & ((UINT64_C(1) << (size * 8)) - 1);
}

static void handoff_write(void *ud, uint64_t addr, uint64_t value,
                          unsigned size) {
    (void)ud; (void)addr; (void)value; (void)size;
}

static MercedStatus handoff_step(Merced *cpu, bool fast) {
    if (!fast)
        return merced_step(cpu);
    unsigned executed;
    return merced_run_fast(cpu, 1, &executed, 0, 0);
}

static void handoff_expect(uint64_t actual, uint64_t expected,
                           const char *what, unsigned ram_mb,
                           bool fast, bool timer) {
    if (actual == expected)
        return;
    fprintf(stderr, "%u MiB %s %s: %s: got 0x%" PRIx64
                    ", expected 0x%" PRIx64 "\n",
            ram_mb, fast ? "fast" : "slow", timer ? "timer" : "external",
            what, actual, expected);
    failures++;
}

static void check_efi_handoff(unsigned ram_mb, bool fast, bool timer,
                              uint64_t entry) {
    MercedBus bus = {
        .ram_size = (uint64_t)ram_mb << 20,
        .read = handoff_read,
        .write = handoff_write,
    };
    Merced *cpu = merced_create(&bus);
    if (!cpu) {
        failures++;
        return;
    }
    cpu->ip = entry;
    cpu->cr[72] = UINT64_C(1) << 16; /* ITV: mask timer during cache warmup. */
    for (unsigned slot = 0; slot < 3; slot++)
        handoff_expect(handoff_step(cpu, fast), MERCED_OK, "cache warmup",
                       ram_mb, fast, timer);

    cpu->ip = entry;
    cpu->group_start = 1;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 14); /* IC, I */
    if (timer) {
        cpu->cr[72] = 0x40;
        cpu->timer_pending = true;
    } else {
        merced_raise_external(cpu, 0); /* Legacy ExtINT, as at EFI CD entry. */
    }
    handoff_expect(handoff_step(cpu, fast), MERCED_OK, "EFI entry executes",
                   ram_mb, fast, timer);
    handoff_expect(cpu->ip, entry + 1, "IRQ does not vector through unset IVA",
                   ram_mb, fast, timer);
    handoff_expect(cpu->nfaults, 0, "no entry fault", ram_mb, fast, timer);
    handoff_expect(timer ? cpu->timer_pending : cpu->ext_pending_count, 1,
                   "interrupt remains pending", ram_mb, fast, timer);

    /* Installing the loader's IVT must make the same pending interrupt
     * deliverable; the handoff guard must not swallow or permanently mask it. */
    cpu->cr[2] = 0x10000; /* IVA */
    cpu->ip = entry;
    cpu->group_start = 1;
    handoff_expect(handoff_step(cpu, fast), MERCED_OK, "IRQ after IVT install",
                   ram_mb, fast, timer);
    handoff_expect(cpu->ip, 0x13000, "external-interrupt vector",
                   ram_mb, fast, timer);
    handoff_expect(cpu->nfaults, 1, "one delivered interrupt",
                   ram_mb, fast, timer);
    handoff_expect(cpu->cr[19], entry, "saved interrupted IP",
                   ram_mb, fast, timer);
    merced_destroy(cpu);
}

static void check_efi_range_stop(unsigned ram_mb) {
    MercedBus bus = {
        .ram_size = (uint64_t)ram_mb << 20,
        .read = handoff_read,
        .write = handoff_write,
    };
    Merced *cpu = merced_create(&bus);
    if (!cpu) {
        failures++;
        return;
    }
    uint64_t entry = bus.ram_size - UINT64_C(0x0106e000);
    cpu->ip = entry - 16;
    unsigned executed = 0;
    handoff_expect(merced_run_fast_range(cpu, 16, &executed, 0, 0,
                                        entry, entry + 0x1000),
                   MERCED_OK, "range stop status", ram_mb, true, false);
    handoff_expect(executed, 3, "stop before application instruction",
                   ram_mb, true, false);
    handoff_expect(cpu->ip, entry, "range stop entry IP", ram_mb, true, false);
    handoff_expect(handoff_step(cpu, true), MERCED_OK,
                   "ordinary fast API resumes", ram_mb, true, false);
    handoff_expect(cpu->ip, entry + 1, "disabled range permits execution",
                   ram_mb, true, false);
    merced_destroy(cpu);
}

static void check_rejected_memory(unsigned ram_mb) {
    Ia64Config cfg = {
        .ram_size = (uint64_t)ram_mb << 20,
        .display_type = GEMU_DISPLAY_NONE,
    };
    Ia64I2000State *s = ia64_i2000_create(&cfg);
    expect(s == NULL, 1, "unsupported DIMM population is rejected", ram_mb, 0);
    ia64_i2000_destroy(s);
}

typedef struct {
    uint64_t entry_pa;
    uint64_t pte, itir, tag;
    unsigned entry_reads;
} VhptFixture;

static uint64_t vhpt_read(void *ud, uint64_t addr, unsigned size) {
    VhptFixture *fixture = ud;
    if (size == 8 && addr == fixture->entry_pa) {
        fixture->entry_reads++;
        return fixture->pte;
    }
    if (size == 8 && addr == fixture->entry_pa + 8)
        return fixture->itir;
    if (size == 8 && addr == fixture->entry_pa + 16)
        return fixture->tag;
    if (size == 8 && addr == UINT64_C(0x40020))
        return UINT64_C(0x123456789abcdef0);
    return UINT64_MAX;
}

static void vhpt_expect(uint64_t actual, uint64_t expected, const char *what,
                        bool long_format, bool ifetch, const char *reserved) {
    if (actual == expected)
        return;
    fprintf(stderr, "VHPT %s %s %s: %s: got 0x%" PRIx64
                    ", expected 0x%" PRIx64 "\n",
            long_format ? "long" : "short", ifetch ? "instruction" : "data",
            reserved, what, actual, expected);
    failures++;
}

/* Exercise the actual walker through the public memory-access hook shared
 * by IA-64 and IA-32.  A reserved entry must miss without occupying the TC;
 * repairing that entry must then let the same reference succeed. */
static void check_reserved_vhpt(bool long_format, bool ifetch,
                                uint64_t bad_pte, uint64_t bad_itir,
                                const char *reserved) {
    const uint64_t va = UINT64_C(0x4000000000400020);
    const uint64_t iva = UINT64_C(0xe000000000100000);
    const uint64_t pte = UINT64_C(0x40661); /* P, A, D, PL0, RWX, WB */
    const uint64_t itir = (UINT64_C(3) << 8) | (13 << 2);
    const uint64_t psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 17) |
                         (UINT64_C(1) << 36); /* IC, DT, IT */
    VhptFixture fixture = {
        .entry_pa = long_format ? 0x240e0 : 0x21000,
        .pte = pte | bad_pte,
        .itir = itir | bad_itir,
        .tag = (UINT64_C(3) << 48) | 0x200,
    };
    MercedBus bus = {.ud = &fixture, .read = vhpt_read, .write = handoff_write};
    Merced *cpu = merced_create(&bus);
    if (!cpu) {
        failures++;
        return;
    }
    cpu->ip = 0x4000;
    cpu->psr = psr;
    cpu->cr[2] = iva;
    cpu->cr[8] = 0x10000 | (15 << 2) | 1 | (long_format ? 0x100 : 0);
    cpu->rr[0] = cpu->rr[2] = itir | 1;
    uint64_t table_va = long_format ? UINT64_C(0x10000)
                                   : UINT64_C(0x4000000000010000);
    cpu->dtr[0] = (MercedTlbEntry) {
        .va_start = table_va, .va_end = table_va + 0xffff,
        .pfn_base = 0x20000, .rid = 3, .ps = 16, .valid = 1,
        .itir = (3 << 8) | (16 << 2), .pte = 0x20461,
    };

    uint64_t value = 0;
    vhpt_expect(merced_ia32_read(cpu, va, 8, ifetch, &value), false,
                "reserved entry faults", long_format, ifetch, reserved);
    vhpt_expect(fixture.entry_reads, 1, "hardware read the VHPT entry",
                long_format, ifetch, reserved);
    vhpt_expect(cpu->ip, iva + (ifetch ? 0x400 : 0x800), "TLB miss vector",
                long_format, ifetch, reserved);
    vhpt_expect(cpu->cr[20], va, "fault address", long_format, ifetch, reserved);
    vhpt_expect(ifetch ? cpu->itc_next : cpu->dtc_next, 0,
                "reserved entry does not consume a TC slot",
                long_format, ifetch, reserved);
    MercedTlbEntry *tc = ifetch ? cpu->itc : cpu->dtc;
    unsigned occupied = 0;
    for (unsigned i = 0; i < MERCED_N_TC; i++)
        occupied += tc[i].valid != 0;
    vhpt_expect(occupied, 0, "reserved entry is not cached",
                long_format, ifetch, reserved);

    fixture.pte = pte;
    fixture.itir = itir;
    cpu->ip = 0x4000;
    cpu->psr = psr;
    vhpt_expect(merced_ia32_read(cpu, va, 8, ifetch, &value), true,
                "repaired entry succeeds", long_format, ifetch, reserved);
    vhpt_expect(value, UINT64_C(0x123456789abcdef0), "translated backing data",
                long_format, ifetch, reserved);
    vhpt_expect(fixture.entry_reads, 2, "repair causes a fresh hardware walk",
                long_format, ifetch, reserved);
    vhpt_expect(tc[0].pte, pte, "only the repaired entry is cached",
                long_format, ifetch, reserved);
    merced_destroy(cpu);
}

typedef struct {
    __uint128_t bundle;
    uint64_t data[2];
    unsigned reads;
} FpCheckFixture;

typedef struct {
    uint64_t pte;
    unsigned table_reads, io_reads, alias_reads;
} Region7Fixture;

static uint64_t region7_read(void *ud, uint64_t addr, unsigned size) {
    Region7Fixture *f = ud;
    if (addr >= 0x4000 && addr < 0x4010) {
        /* The HAL's actual ld2.acq r32=[r32], followed by two nops. */
        __uint128_t bundle = ((__uint128_t)UINT64_C(0x08542000800) << 5) |
            ((__uint128_t)UINT64_C(0x08000000) << 46) |
            ((__uint128_t)UINT64_C(0x08000000) << 87);
        return (uint64_t)(bundle >> ((addr & 15) * 8));
    }
    if (addr == 0x20a00 && size == 8) {
        f->table_reads++;
        return f->pte;
    }
    if (addr == UINT64_C(0x00000ffffc281a04) && size == 2) {
        f->io_reads++;
        return 0x1235;
    }
    if (addr == UINT64_C(0x70281a04)) {
        f->alias_reads++;
        return 0;
    }
    return UINT64_MAX;
}

/* With 2 GiB installed, an invented RAM alias used to cover the HAL's
 * virtual I/O window, bypass its valid VHPT entry and read unrelated RAM.
 * Unmapped references in the same window must also reach the OS handler. */
static void check_region7_translation(bool fast, unsigned scenario) {
    const uint64_t va = UINT64_C(0xe0000000f0281a04);
    const uint64_t iva = UINT64_C(0xe000000000100000);
    Region7Fixture f = {.pte = UINT64_C(0x00100ffffc280671)};
    if (scenario == 1) f.pte &= ~UINT64_C(1); /* not present */
    if (scenario == 2) f.pte |= 2; /* reserved PTE: ordinary TLB miss */
    MercedBus bus = {.ud = &f, .read = region7_read, .write = handoff_write,
                     .ram_size = UINT64_C(2) << 30};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x4000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 17); /* IC, DT */
    cpu->cr[2] = iva;
    cpu->cr[8] = 0x10000 | (15 << 2) | (scenario != 3);
    cpu->rr[7] = (1 << 8) | (13 << 2) | 1;
    cpu->cfm = 1;
    cpu->gr_stack[0] = va;
    cpu->dtr[0] = (MercedTlbEntry) {
        .va_start = UINT64_C(0xe000000000010000),
        .va_end = UINT64_C(0xe00000000001ffff),
        .pfn_base = 0x20000, .rid = 1, .ps = 16, .valid = 1,
        .itir = (1 << 8) | (16 << 2), .pte = 0x20461,
    };
    handoff_expect(handoff_step(cpu, fast), MERCED_OK,
                   "region7 load status", 2048, fast, false);
    handoff_expect(f.alias_reads, 0, "OS virtual address never aliases RAM",
                   2048, fast, false);
    handoff_expect(f.table_reads, scenario != 3, "region7 consults VHPT",
                   2048, fast, false);
    handoff_expect(f.io_reads, scenario == 0, "only valid mapping reads I/O",
                   2048, fast, false);
    if (scenario == 0) {
        handoff_expect(cpu->gr_stack[0], 0x1235, "HAL reads mapped I/O value",
                       2048, fast, false);
        handoff_expect(cpu->nfaults, 0, "mapped I/O does not fault",
                       2048, fast, false);
    } else {
        uint64_t vector = scenario == 1 ? 0x5000 : scenario == 2 ? 0x800 : 0x1000;
        handoff_expect(cpu->ip, iva + vector, "unmapped OS reference faults",
                       2048, fast, false);
        handoff_expect(cpu->cr[20], va, "fault reports original virtual I/O",
                       2048, fast, false);
        handoff_expect(cpu->gr_stack[0], va, "fault preserves load destination",
                       2048, fast, false);
    }
    merced_destroy(cpu);
}

static uint64_t fpcheck_read(void *ud, uint64_t addr, unsigned size) {
    FpCheckFixture *fixture = ud;
    if (addr >= 0x1000 && addr < 0x1010)
        return (uint64_t)(fixture->bundle >> ((addr - 0x1000) * 8));
    if (addr == 0x2000 || addr == 0x2008) {
        fixture->reads++;
        uint64_t value = fixture->data[(addr - 0x2000) / 8];
        return size == 4 ? (uint32_t)value : value;
    }
    return UINT64_MAX;
}

static void fpcheck_expect(uint64_t actual, uint64_t expected, const char *what,
                           unsigned x6, unsigned form, unsigned scenario,
                           bool fast) {
    if (actual == expected)
        return;
    fprintf(stderr, "FP check-load x6=%02x form=%u scenario=%u %s: %s: "
                    "got 0x%" PRIx64 ", expected 0x%" PRIx64 "\n",
            x6, form, scenario, fast ? "fast" : "slow", what, actual, expected);
    failures++;
}

/* Both completers conservatively reload, as FP ALAT entries are not kept.
 * Exercise all four data formats, no/register/immediate update forms, and
 * the precise raw opcode encountered in HAL.dll. */
static void check_fp_check_load(unsigned x6, unsigned form, unsigned scenario,
                                bool fast) {
    uint64_t raw = (UINT64_C(6) << 37) | ((uint64_t)x6 << 30) |
                   (UINT64_C(29) << 20) | (9 << 6); /* f9=[r29] */
    if (x6 == 0x27 && form == 0)
        raw = UINT64_C(0x0C9C1D00240); /* HAL: ldfd.c.nc f9=[r29] */
    if (form == 1)
        raw |= (UINT64_C(1) << 36) | (28 << 13); /* +r28 */
    else if (form == 2)
        raw |= (UINT64_C(1) << 37) | (UINT64_C(1) << 36) |
               (1 << 27) | (0x70 << 13); /* major7, signed imm9=-16 */
    if (scenario == 1)
        raw |= 1; /* p1 is false, even with a NaT base and translation on. */
    const __uint128_t nop = UINT64_C(0x08000000);
    FpCheckFixture fixture = {
        .bundle = 9 | (nop << 5) | ((__uint128_t)raw << 46) | (nop << 87),
    };
    unsigned fmt = x6 & 3;
    static const uint64_t initial_data[] = {
        UINT64_C(0xc000000000000000), UINT64_C(0xfedcba9876543210),
        UINT64_C(0xc0400000), UINT64_C(0xc008000000000000),
    };
    static const uint64_t next_data[] = {
        UINT64_C(0xc000000000000000), UINT64_C(0xfedcba9876543310),
        UINT64_C(0xc0c00000), UINT64_C(0xc018000000000000),
    };
    fixture.data[0] = initial_data[fmt];
    fixture.data[1] = 0x30000; /* extended -3.0: sign1, exponent0x10000 */
    MercedBus bus = {.ud = &fixture, .read = fpcheck_read, .write = handoff_write};
    Merced *cpu = merced_create(&bus);
    if (!cpu) {
        failures++;
        return;
    }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 44); /* IC, BN */
    cpu->cr[72] = UINT64_C(1) << 16;
    cpu->cr[2] = UINT64_C(0xe000000000100000);
    cpu->gr_static[28] = 16;
    cpu->gr_static[29] = 0x2000;
    cpu->fr[9] = (MercedFpReg){.sig = 0x55, .exp = 0x123, .sign = 1, .nat = 1};
    /* An integer r9 association must neither satisfy an f9 check nor be
     * cleared by its .clr completer. */
    cpu->alat[0].reg = 9;
    cpu->alat[0].phys_addr = 0x2000;
    cpu->alat[0].size = 8;
    cpu->alat[0].valid = 1;
    cpu->alat_valid_mask = 1;
#define FP_EXPECT(actual, expected, what) \
    fpcheck_expect((actual), (expected), (what), x6, form, scenario, fast)
    FP_EXPECT(handoff_step(cpu, fast), MERCED_OK, "decode-cache warmup nop");
    if (scenario == 1 || scenario == 2)
        cpu->nat_static[29] = 1;
    if (scenario == 1 || scenario == 3)
        cpu->psr |= UINT64_C(1) << 17; /* DT, with no data translation. */
    FP_EXPECT(handoff_step(cpu, fast), MERCED_OK, "check-load status");
    if (scenario) {
        FP_EXPECT(fixture.reads, 0, "no backing read when predicated off or faulted");
        FP_EXPECT(cpu->fr[9].sig, 0x55, "destination preserved");
        FP_EXPECT(cpu->fr[9].nat, 1, "destination NaT preserved");
        FP_EXPECT(cpu->gr_static[29], 0x2000, "base update suppressed");
        FP_EXPECT(cpu->ip, scenario == 1 ? 0x1002 :
                  cpu->cr[2] + (scenario == 2 ? 0x5600 : 0x1000),
                  "predication or precise fault vector");
    } else {
        for (unsigned pass = 0; pass < 2; pass++) {
            FP_EXPECT(fixture.reads, (pass + 1) * (fmt == 0 ? 2 : 1),
                      "check-load reloads after an ALAT miss");
            FP_EXPECT(cpu->fr[9].sig, fmt == 1 ? fixture.data[0] :
                      UINT64_C(0xc000000000000000), "loaded significand");
            FP_EXPECT(cpu->fr[9].exp, fmt == 1 ? 0x1003e : 0x10000 + pass,
                      "loaded exponent");
            FP_EXPECT(cpu->fr[9].sign, fmt != 1, "loaded sign");
            FP_EXPECT(cpu->fr[9].nat, 0, "successful reload clears NaTVal");
            FP_EXPECT(cpu->gr_static[29], form == 1 ? 0x2010 :
                      form == 2 ? 0x1ff0 : 0x2000, "base update");
            if (!pass) {
                fixture.data[0] = next_data[fmt];
                fixture.data[1]++;
                cpu->ip = 0x1001;
                cpu->gr_static[29] = 0x2000;
                FP_EXPECT(handoff_step(cpu, fast), MERCED_OK, "repeat check-load");
            }
        }
    }
    FP_EXPECT(cpu->alat[0].valid, 1, "integer ALAT entry remains valid");
    FP_EXPECT(cpu->alat_valid_mask, 1, "integer ALAT occupancy is unchanged");
#undef FP_EXPECT
    merced_destroy(cpu);
}

typedef struct {
    __uint128_t load_bundle, handler_bundle;
    unsigned reads;
} LoadRestartFixture;

static uint64_t load_restart_fetch(void *ud, uint64_t addr, unsigned size) {
    LoadRestartFixture *fixture = ud;
    __uint128_t bundle;
    if (addr >= 0x1000 && addr < 0x1010)
        bundle = fixture->load_bundle;
    else if (addr >= 0x9000 && addr < 0x9010)
        bundle = fixture->handler_bundle;
    else
        return UINT64_MAX;
    uint64_t value = (uint64_t)(bundle >> ((addr & 15) * 8));
    return size == 8 ? value : value & ((UINT64_C(1) << (size * 8)) - 1);
}

static uint64_t load_restart_read(void *ud, uint64_t addr, unsigned size) {
    LoadRestartFixture *fixture = ud;
    if (addr == 0x40020 && size == 8) {
        fixture->reads++;
        return UINT64_C(0x8877665544332211);
    }
    return UINT64_MAX;
}

static void load_restart_expect(uint64_t actual, uint64_t expected,
                                const char *what, bool fast, bool reg_update,
                                unsigned scenario, unsigned initial_nat) {
    if (actual == expected)
        return;
    fprintf(stderr, "integer load restart %s %s scenario=%u initial_nat=%u: "
                    "%s: got 0x%" PRIx64 ", expected 0x%" PRIx64 "\n",
            fast ? "fast" : "slow", reg_update ? "register" : "immediate",
            scenario, initial_nat, what, actual, expected);
    failures++;
}

/* The PAL dispatcher walks three-word records with postincrement loads.
 * Advancing its pointer on a translation fault makes the retried load read
 * the next field, so it misses the sentinel and runs off the mapped table.
 * Exercise the complete fault -> itc.d -> rfi -> retry sequence, including
 * ld8.s with deferral disabled and enabled. */
static void check_load_restart(bool fast, bool reg_update, unsigned scenario,
                               unsigned initial_nat) {
    const uint64_t va = UINT64_C(0x4000000000400020);
    const uint64_t original = UINT64_C(0x1122334455667788);
    const __uint128_t nop = UINT64_C(0x08000000);
    unsigned x6 = scenario ? 0x07 : 0x03; /* ld8.s or ld8 */
    uint64_t raw = ((uint64_t)(reg_update ? 4 : 5) << 37) |
                   ((uint64_t)x6 << 30) | (10 << 20) | (8 << 6);
    if (reg_update)
        raw |= (UINT64_C(1) << 36) | (14 << 13); /* ld8 r8=[r10],r14 */
    else
        raw |= 8 << 13;                        /* ld8 r8=[r10],8 */
    uint64_t insert = (UINT64_C(1) << 37) | (UINT64_C(0x2e) << 27) |
                      (13 << 13);             /* itc.d r13 */
    uint64_t rfi = UINT64_C(8) << 27;
    LoadRestartFixture fixture = {
        .load_bundle = 9 | (nop << 5) | ((__uint128_t)raw << 46) |
                       (nop << 87),           /* MMI: nop; load; nop */
        .handler_bundle = 17 | ((__uint128_t)insert << 5) | (nop << 46) |
                          ((__uint128_t)rfi << 87), /* MIB: itc.d; nop; rfi */
    };
    MercedBus bus = {
        .ud = &fixture, .read = load_restart_read, .fetch = load_restart_fetch,
        .write = handoff_write,
    };
    Merced *cpu = merced_create(&bus);
    if (!cpu) {
        failures++;
        return;
    }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 17) |
               (UINT64_C(1) << 44);           /* IC, DT, BN; physical code */
    cpu->cr[2] = 0x8000;                       /* alternate DTLB vector=0x9000 */
    cpu->cr[72] = UINT64_C(1) << 16;            /* masked timer */
    cpu->cr[0] = scenario == 2 ? UINT64_C(1) << 8 : 0; /* DCR.dm */
    cpu->rr[2] = (3 << 8) | (13 << 2);
    cpu->gr_static[8] = original;
    cpu->nat_static[8] = initial_nat;
    cpu->gr_static[10] = va;
    cpu->gr_static[13] = 0x40661;               /* handler's present RWX PTE */
    cpu->gr_static[14] = 8;
#define LOAD_EXPECT(actual, expected, what) \
    load_restart_expect((actual), (expected), (what), fast, reg_update, \
                         scenario, initial_nat)
    LOAD_EXPECT(handoff_step(cpu, fast), MERCED_OK, "decode-cache warmup nop");
    LOAD_EXPECT(handoff_step(cpu, fast), MERCED_OK, "unmapped load status");
    LOAD_EXPECT(fixture.reads, 0, "fault/defer performs no backing read");
    if (scenario == 2) {
        LOAD_EXPECT(cpu->nfaults, 0, "speculative miss is deferred");
        LOAD_EXPECT(cpu->ip, 0x1002, "deferred load completes");
        LOAD_EXPECT(cpu->nat_static[8], 1, "deferred load produces NaT");
        LOAD_EXPECT(cpu->gr_static[10], va + 8, "deferred base update occurs once");
        LOAD_EXPECT(cpu->nat_static[10], 0, "deferred base keeps its NaT state");
    } else {
        LOAD_EXPECT(cpu->nfaults, 1, "one delivered translation fault");
        LOAD_EXPECT(cpu->ip, 0x9000, "alternate DTLB handler entered");
        LOAD_EXPECT(cpu->cr[19], 0x1000, "fault saves the load's bundle");
        LOAD_EXPECT((cpu->cr[16] >> 41) & 3, 1, "fault saves the load's slot");
        LOAD_EXPECT(cpu->cr[20], va, "fault address");
        LOAD_EXPECT(cpu->gr_static[8], original, "fault preserves destination");
        LOAD_EXPECT(cpu->nat_static[8], initial_nat, "fault preserves destination NaT");
        LOAD_EXPECT(cpu->gr_static[10], va, "fault suppresses base update");
        LOAD_EXPECT(cpu->nat_static[10], 0, "fault preserves base NaT");
        for (unsigned slot = 0; slot < 3; slot++)
            LOAD_EXPECT(handoff_step(cpu, fast), MERCED_OK, "handler maps and returns");
        LOAD_EXPECT(cpu->dtc_next, 1, "handler installed one data translation");
        LOAD_EXPECT(cpu->ip, 0x1001, "rfi retries the interrupted load");
        LOAD_EXPECT(handoff_step(cpu, fast), MERCED_OK, "mapped retry succeeds");
        LOAD_EXPECT(cpu->ip, 0x1002, "retry advances to the following slot");
        LOAD_EXPECT(cpu->nfaults, 1, "retry needs no further fault");
        LOAD_EXPECT(fixture.reads, 1, "retry reads backing memory once");
        LOAD_EXPECT(cpu->gr_static[8], UINT64_C(0x8877665544332211),
                    "retry loads the original address");
        LOAD_EXPECT(cpu->nat_static[8], 0, "successful retry clears destination NaT");
        LOAD_EXPECT(cpu->gr_static[10], va + 8, "retry advances base exactly once");
        LOAD_EXPECT(cpu->nat_static[10], 0, "retry preserves base NaT");
    }
#undef LOAD_EXPECT
    merced_destroy(cpu);
}

typedef struct {
    __uint128_t bundle[2];
} TestPredicateFixture;

static uint64_t test_predicate_read(void *ud, uint64_t addr, unsigned size) {
    TestPredicateFixture *fixture = ud;
    if (addr >= 0x1000 && addr < 0x1020)
        return (uint64_t)(fixture->bundle[(addr - 0x1000) / 16] >>
                          ((addr & 15) * 8));
    return handoff_read(NULL, addr, size);
}

/* Broadside predicate transfers use physical PR bits, even with a nonzero
 * RRB. Their qualifier still names a logical predicate. In particular, an
 * interrupt's mov-from-PR followed by a restore with RRB=0 must preserve the
 * original pipeline predicates (Intel SDM Vol.1 4.3.4, Vol.3 mov pr). */
static void check_predicate_transfer(unsigned form, unsigned rrb,
                                      bool qualified, bool nat, bool fast) {
    const __uint128_t nop = UINT64_C(0x08000000);
    const uint64_t source = UINT64_C(0xFEDCBA9876543210);
    const uint64_t old_dest = UINT64_C(0x0123456789ABCDEF);
    const unsigned qp = 17;
    uint64_t raw, mask = 0, rotated = 0;
    if (form == 0) { /* mov r8=pr */
        raw = (UINT64_C(0x33) << 27) | (8 << 6) | qp;
    } else if (form <= 2) { /* mov pr=r9, partial or complete mask */
        unsigned mask17 = form == 1 ? (1u << 2) | (1u << 9) : 0x1fffe;
        mask = form == 1 ? UINT64_C(0x204) : UINT64_MAX - 1;
        raw = (UINT64_C(3) << 33) | ((uint64_t)(mask17 >> 16) << 36) |
              ((uint64_t)((mask17 >> 8) & 0xff) << 24) |
              ((uint64_t)((mask17 >> 1) & 0x7f) << 6) | (9 << 13) | qp;
    } else { /* mov pr.rot=1<<16, or negative imm28's sign bit */
        uint64_t imm28 = form == 3 ? 1 : UINT64_C(0x8000000);
        rotated = form == 3 ? UINT64_C(0x10000)
                           : UINT64_C(0xFFFFF80000000000);
        raw = (UINT64_C(2) << 33) | ((imm28 >> 27) << 36) |
              ((imm28 & 0x7ffffff) << 6) | qp;
    }
    TestPredicateFixture fixture = {
        .bundle = {1 | (nop << 5) | ((__uint128_t)raw << 46) | (nop << 87), 0},
    };
    MercedBus bus = {.ud = &fixture, .read = test_predicate_read,
                     .fetch = test_predicate_read, .write = handoff_write};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 44);
    cpu->cr[2] = 0x8000;
    cpu->cr[72] = UINT64_C(1) << 16;
    cpu->cfm = (uint64_t)rrb << 32;
    uint64_t qualifier = UINT64_C(1) << (16 + ((qp - 16 + rrb) % 48));
    uint64_t original = (UINT64_C(0x13579BDF2468ACE1) & ~qualifier) |
                        (qualified ? qualifier : 0);
    cpu->pr = original;
    cpu->gr_static[8] = old_dest;
    cpu->nat_static[8] = 1;
    cpu->gr_static[9] = source;
    cpu->nat_static[9] = nat;
    bool fault = qualified && nat && (form == 1 || form == 2);
    uint64_t expected_pr = original;
    if (qualified && !fault && form > 0)
        expected_pr = form <= 2 ? ((original & ~mask) | (source & mask) | 1)
                               : ((original & 0xffff) | rotated);
    MercedStatus warm = handoff_step(cpu, fast);
    MercedStatus status = handoff_step(cpu, fast);
    uint64_t expected_dest = qualified && form == 0 ? original : old_dest;
    unsigned expected_nat = !(qualified && form == 0);
    if (warm != MERCED_OK || status != MERCED_OK ||
        cpu->pr != expected_pr || cpu->gr_static[8] != expected_dest ||
        cpu->nat_static[8] != expected_nat || cpu->nfaults != fault ||
        cpu->cfm != ((uint64_t)rrb << 32) ||
        (fault && cpu->ip != 0xd600)) {
        fprintf(stderr, "predicate transfer form=%u rrb=%u qp=%u nat=%u %s: "
                "PR=%016" PRIx64 "/%016" PRIx64 " GR=%016" PRIx64
                "/%016" PRIx64 " NaT=%u/%u faults=%" PRIu64 "/%u "
                "ip=%016" PRIx64 "\n", form, rrb, qualified, nat,
                fast ? "fast" : "slow", cpu->pr, expected_pr,
                cpu->gr_static[8], expected_dest, cpu->nat_static[8], expected_nat,
                cpu->nfaults, fault, cpu->ip);
        failures++;
    }
    merced_destroy(cpu);
}

static void test_predicate_expect(uint64_t actual, uint64_t expected,
                                  const char *what, uint64_t raw,
                                  bool qualified, bool nat, bool fast) {
    if (actual == expected)
        return;
    fprintf(stderr, "predicate test raw=%011" PRIx64 " qp=%u nat=%u %s: "
                    "%s: got 0x%" PRIx64 ", expected 0x%" PRIx64 "\n",
            raw, qualified, nat, fast ? "fast" : "slow", what, actual, expected);
    failures++;
}

static void check_fp_lanes(unsigned sample, unsigned op, unsigned nat,
                            bool qualified, bool fast) {
    static const struct {
        uint64_t a, b;
        uint64_t expected[4]; /* fmix.r, fmix.l, fsxt.r, fsxt.l */
    } cases[] = {
        {UINT64_C(0x1234567876543210), UINT64_C(0x89abcdef01234567),
         {UINT64_C(0x7654321001234567), UINT64_C(0x1234567889abcdef),
          UINT64_C(0x0000000001234567), UINT64_C(0x0000000089abcdef)}},
        {UINT64_C(0x12345678f6543210), UINT64_C(0x89abcdef01234567),
         {UINT64_C(0xf654321001234567), UINT64_C(0x1234567889abcdef),
          UINT64_C(0xffffffff01234567), UINT64_C(0x0000000089abcdef)}},
        {UINT64_C(0x9234567876543210), UINT64_C(0x89abcdef01234567),
         {UINT64_C(0x7654321001234567), UINT64_C(0x9234567889abcdef),
          UINT64_C(0x0000000001234567), UINT64_C(0xffffffff89abcdef)}},
        {UINT64_C(0x92345678f6543210), UINT64_C(0x89abcdef01234567),
         {UINT64_C(0xf654321001234567), UINT64_C(0x9234567889abcdef),
          UINT64_C(0xffffffff01234567), UINT64_C(0xffffffff89abcdef)}},
        /* Live XP hash product: fsxt.r f6=f0,f13 must discard its high
         * word. Keeping that word changes the bucket from 0x507 to 0x6f2. */
        {0, UINT64_C(0x0174f266b1335915),
         {UINT64_C(0x00000000b1335915), UINT64_C(0x000000000174f266),
          UINT64_C(0x00000000b1335915), UINT64_C(0x000000000174f266)}},
    };
    unsigned a_reg = sample == 4 ? 0 : 12;
    uint64_t raw = ((uint64_t)(0x3a + op) << 27) | (13 << 20) |
                   (a_reg << 13) | (6 << 6);
    if (!qualified)
        raw |= 1; /* false p1 */
    const __uint128_t nop = UINT64_C(0x08000000);
    FpCheckFixture fixture = {
        .bundle = 13 | (nop << 5) | ((__uint128_t)raw << 46) | (nop << 87),
    };
    MercedBus bus = {.ud = &fixture, .read = fpcheck_read, .write = handoff_write};
    Merced *cpu = merced_create(&bus);
    if (!cpu) {
        failures++;
        return;
    }
    cpu->ip = 0x1000;
    cpu->psr = UINT64_C(1) << 13;
    cpu->cr[72] = UINT64_C(1) << 16;
    cpu->fr[12] = (MercedFpReg){cases[sample].a, 0x1234, 1, nat == 1};
    cpu->fr[13] = (MercedFpReg){cases[sample].b, 0x5678, 1, nat == 2};
    cpu->fr[6] = (MercedFpReg){0x55, 0x123, 1, 1};
#define LANE_EXPECT(actual, expected, what) \
    test_predicate_expect((actual), (expected), (what), raw, qualified, nat, fast)
    LANE_EXPECT(handoff_step(cpu, fast), MERCED_OK, "FP lane warmup nop");
    LANE_EXPECT(handoff_step(cpu, fast), MERCED_OK, "FP lane status");
    if (!qualified) {
        LANE_EXPECT(cpu->fr[6].sig, 0x55, "predicated-off significand");
        LANE_EXPECT(cpu->fr[6].exp, 0x123, "predicated-off exponent");
        LANE_EXPECT(cpu->fr[6].sign, 1, "predicated-off sign");
        LANE_EXPECT(cpu->fr[6].nat, 1, "predicated-off NaT");
    } else {
        LANE_EXPECT(cpu->fr[6].nat, nat != 0, "FP lane propagates either NaTVal");
        if (!nat) {
            LANE_EXPECT(cpu->fr[6].sig, cases[sample].expected[op],
                        "FP lane selection, source ordering and sign extension");
            LANE_EXPECT(cpu->fr[6].exp, 0x1003e, "FP lane integer exponent");
            LANE_EXPECT(cpu->fr[6].sign, 0, "FP lane positive sign field");
        }
    }
    LANE_EXPECT(cpu->nfaults, 0, "FP lane does not consume NaTVal");
#undef LANE_EXPECT
    merced_destroy(cpu);
}

static void check_test_predicate(bool unconditional, bool tnat, bool qualified,
                                 unsigned bit, bool nat, bool fast) {
    /* Windows' (p1) tbit.z.unc p3,p2=r32,0 guards the pool-tracking path.
     * A false p1 must clear stale p3 before the following conditional branch. */
    uint64_t raw = UINT64_C(0x0A0120010C1);
    if (!unconditional)
        raw &= ~(UINT64_C(1) << 12);
    if (tnat)
        raw |= UINT64_C(1) << 13;
    const __uint128_t nop = UINT64_C(0x08000000);
    const __uint128_t branch = (UINT64_C(4) << 37) | (2 << 13) | 3;
    TestPredicateFixture fixture = {.bundle = {
        1 | (nop << 5) | (nop << 46) | ((__uint128_t)raw << 87),
        17 | (nop << 5) | (nop << 46) | (branch << 87),
    }};
    MercedBus bus = {.ud = &fixture, .read = test_predicate_read,
                     .write = handoff_write};
    Merced *cpu = merced_create(&bus);
    if (!cpu) {
        failures++;
        return;
    }
    cpu->ip = 0x1000;
    cpu->psr = UINT64_C(1) << 13;
    cpu->cr[72] = UINT64_C(1) << 16;
    cpu->cfm = 1;
    cpu->gr_stack[0] = bit;
    cpu->nat_stack[0] = nat;
    cpu->pr = 1 | ((uint64_t)qualified << 1) | (1 << 2) | (1 << 3);
#define PRED_EXPECT(actual, expected, what) \
    test_predicate_expect((actual), (expected), (what), raw, qualified, nat, fast)
    for (unsigned slot = 0; slot < 3; slot++)
        PRED_EXPECT(handoff_step(cpu, fast), MERCED_OK, "instruction status");
    unsigned first, second;
    if (!qualified) {
        first = second = !unconditional;
    } else if (!tnat && nat) {
        first = second = 0;
    } else {
        first = !(tnat ? nat : bit);
        second = !first;
    }
    PRED_EXPECT((cpu->pr >> 3) & 1, first, "first target p3");
    PRED_EXPECT((cpu->pr >> 2) & 1, second, "second target p2");
    PRED_EXPECT((cpu->pr >> 1) & 1, qualified, "qualifying predicate preserved");
    PRED_EXPECT(cpu->nfaults, 0, "test does not consume source NaT");
    for (unsigned slot = 0; slot < 3; slot++)
        PRED_EXPECT(handoff_step(cpu, fast), MERCED_OK, "guarded branch status");
    PRED_EXPECT(cpu->ip, first ? 0x1030 : 0x1020,
                "branch follows updated predicate, not stale tracking state");
#undef PRED_EXPECT
    merced_destroy(cpu);
}

static void check_mux1(unsigned mode, unsigned sample, bool nat,
                       bool qualified, bool fast) {
    const unsigned encoding[] = {0, 8, 9, 10, 11};
    const uint64_t source[] = {UINT64_C(0x78675645342312a5), 0xff};
    /* Intel SDM Vol.3 mux operation, listed as complete expected values so
     * an accidentally reversed byte numbering cannot duplicate itself in
     * the implementation and the regression. */
    const uint64_t expected[] = {
        UINT64_C(0xa5a5a5a5a5a5a5a5), UINT64_C(0x78345612672345a5),
        UINT64_C(0x78346723561245a5), UINT64_C(0x78563412674523a5),
        UINT64_C(0xa512233445566778),
    };
    /* Actual NT RtlFillMemory opcode: mux1 r25=r33,@brcst. RtlSetBits
     * passes 0xff here; broadcasting byte7 instead silently clears every
     * complete bitmap byte and permits already-used pages to be reused. */
    uint64_t raw = UINT64_C(0x0eca0042640) | ((uint64_t)encoding[mode] << 20);
    if (!qualified)
        raw |= 1; /* false p1, leaving the original p0 opcode in true cases */
    const __uint128_t nop = UINT64_C(0x08000000);
    TestPredicateFixture fixture = {.bundle = {
        1 | (nop << 5) | ((__uint128_t)raw << 46) | (nop << 87), 0,
    }};
    MercedBus bus = {.ud = &fixture, .read = test_predicate_read,
                     .write = handoff_write};
    Merced *cpu = merced_create(&bus);
    if (!cpu) {
        failures++;
        return;
    }
    const uint64_t original = UINT64_C(0x1122334455667788);
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 44);
    cpu->cr[72] = UINT64_C(1) << 16;
    cpu->pr = 1;
    cpu->cfm = 2;
    cpu->gr_stack[1] = source[sample];
    cpu->nat_stack[1] = nat;
    cpu->gr_static[25] = original;
    cpu->nat_static[25] = !nat;
#define MUX_EXPECT(actual, wanted, what) \
    test_predicate_expect((actual), (wanted), (what), raw, qualified, nat, fast)
    MUX_EXPECT(handoff_step(cpu, fast), MERCED_OK, "mux1 decode-cache warmup");
    MUX_EXPECT(handoff_step(cpu, fast), MERCED_OK, "mux1 instruction status");
    uint64_t result = sample == 0 ? expected[mode]
                    : mode == 0 ? UINT64_MAX
                    : mode == 4 ? UINT64_C(0xff00000000000000) : 0xff;
    MUX_EXPECT(cpu->gr_static[25], qualified ? result : original,
                "mux1 byte permutation");
    MUX_EXPECT(cpu->nat_static[25], qualified ? nat : !nat,
                "mux1 propagates NaT only when qualified");
    MUX_EXPECT(cpu->gr_stack[1], source[sample], "mux1 preserves source");
    MUX_EXPECT(cpu->nfaults, 0, "mux1 does not consume source NaT");
#undef MUX_EXPECT
    merced_destroy(cpu);
}

typedef struct {
    uint8_t memory[0x10000];
} RseFixture;

static uint64_t rse_test_read(void *ud, uint64_t addr, unsigned size) {
    RseFixture *fixture = ud;
    if (addr > sizeof(fixture->memory) - size)
        return UINT64_MAX;
    uint64_t value = 0;
    for (unsigned i = 0; i < size; i++)
        value |= (uint64_t)fixture->memory[addr + i] << (i * 8);
    return value;
}

static void rse_test_write(void *ud, uint64_t addr, uint64_t value,
                           unsigned size) {
    RseFixture *fixture = ud;
    if (addr > sizeof(fixture->memory) - size)
        return;
    for (unsigned i = 0; i < size; i++)
        fixture->memory[addr + i] = value >> (i * 8);
}

static void rse_test_bundle(RseFixture *fixture, uint64_t addr,
                            unsigned template, uint64_t a, uint64_t b,
                            uint64_t c) {
    __uint128_t bundle = template | ((__uint128_t)a << 5) |
                        ((__uint128_t)b << 46) | ((__uint128_t)c << 87);
    rse_test_write(fixture, addr, (uint64_t)bundle, 8);
    rse_test_write(fixture, addr + 8, (uint64_t)(bundle >> 64), 8);
}

static uint64_t rse_test_move(uint64_t addr, int slots) {
    while (slots < 0) {
        addr -= 8;
        if ((addr & 0x1f8) == 0x1f8)
            addr -= 8;
        slots++;
    }
    while (slots > 0) {
        addr += 8;
        if ((addr & 0x1f8) == 0x1f8)
            addr += 8;
        slots--;
    }
    return addr;
}

static void rse_test_expect(uint64_t actual, uint64_t expected,
                            const char *what, bool fast, unsigned dirty,
                            bool rewind, bool big_endian, bool fault) {
    if (actual == expected)
        return;
    fprintf(stderr, "RSE %s dirty=%u rewind=%u be=%u fault=%u: %s: "
                    "got 0x%" PRIx64 ", expected 0x%" PRIx64 "\n",
            fast ? "fast" : "slow", dirty, rewind, big_endian, fault,
            what, actual, expected);
    failures++;
}

/* NT switches BSPSTORE with no loadrs, then br.ret exposes an incoming
 * thread's saved return register.  The old thread's register ring must not
 * supply that return address.  Include a clean/dirty overlap, a nearby
 * backward switch, an RNAT collection boundary and a genuine fill fault. */
static void check_rse_context_switch(bool fast, unsigned dirty, bool rewind,
                                      bool big_endian, bool fault) {
    RseFixture fixture = {0};
    const uint64_t nop = UINT64_C(0x08000000);
    const uint64_t ret = UINT64_C(0x00108000100);
    const uint64_t mov_ar = (UINT64_C(1) << 37) | (UINT64_C(0x2a) << 27);
    rse_test_bundle(&fixture, 0x1000, 1,
                    mov_ar | (18 << 20) | (25 << 13), nop, nop);
    rse_test_bundle(&fixture, 0x1010, 1,
                    mov_ar | (19 << 20) | (24 << 13), nop, nop);
    rse_test_bundle(&fixture, 0x1020, 1,
                    mov_ar | (16 << 20) | (23 << 13), nop, nop);
    rse_test_bundle(&fixture, 0x1030, 17, nop, nop, ret);
    /* mov b0=r34; mov ar.pfs=r35; br.ret b0 */
    rse_test_bundle(&fixture, 0x2000, 1, nop,
                    UINT64_C(0x00e00144000), UINT64_C(0x00154046000));
    rse_test_bundle(&fixture, 0x2010, 17, nop, nop, ret);
    uint64_t insert = (UINT64_C(1) << 37) | (UINT64_C(0x2e) << 27) |
                      (13 << 13);
    rse_test_bundle(&fixture, 0x9000, 17, insert, nop, UINT64_C(8) << 27);

    uint64_t store_pa = rewind ? 0x2210 : 0x4410;
    uint64_t store = store_pa | (fault ? UINT64_C(0x4000000000000000) : 0);
    uint64_t incoming[] = {0x1111, 0x2222, 0x3000, 0};
    uint64_t partial_rnat = 0, first_rnat = 0;
    uint64_t first_addr = rse_test_move(store_pa, -(int)(4 - dirty));
    for (unsigned i = 0; i < 4 - dirty; i++) {
        uint64_t addr = rse_test_move(first_addr, i);
        uint64_t value = big_endian ? __builtin_bswap64(incoming[i]) : incoming[i];
        rse_test_write(&fixture, addr, value, 8);
        uint64_t bit = i < 2 ? UINT64_C(1) << ((addr >> 3) & 63) : 0;
        if ((addr | 0x1f8) < store_pa)
            first_rnat |= bit;
        else
            partial_rnat |= bit;
    }
    rse_test_write(&fixture, first_addr | 0x1f8,
                    big_endian ? __builtin_bswap64(first_rnat) : first_rnat, 8);

    MercedBus bus = {.ud = &fixture, .read = rse_test_read,
                     .fetch = rse_test_read, .write = rse_test_write};
    Merced *cpu = merced_create(&bus);
    if (!cpu) {
        failures++;
        return;
    }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 44) |
               (fault ? UINT64_C(1) << 27 : 0); /* IC, BN, optional RT */
    cpu->cr[2] = 0x8000;
    cpu->cr[72] = UINT64_C(1) << 16;
    cpu->rr[2] = (3 << 8) | (13 << 2);
    cpu->gr_static[13] = (store_pa & ~UINT64_C(0x1fff)) | 0x661;
    cpu->gr_static[23] = 3 | (big_endian ? 16 : 0);
    cpu->gr_static[24] = partial_rnat;
    cpu->gr_static[25] = store;
    cpu->bof = cpu->bof_total = 64;
    cpu->rse_flushed_regs = 64 - dirty;
    cpu->rse_anchor_regs = 64;
    cpu->rse_anchor_addr = 0x2400;
    cpu->cfm = 6 | (4 << 7);
    cpu->ar[64] = 5 | (4 << 7);
    cpu->br[0] = 0x2000;
    cpu->gr_stack[60] = 0xaaaa;
    cpu->gr_stack[61] = 0xbbbb;
    cpu->gr_stack[62] = 0x4000; /* outgoing thread's stale return address */
    cpu->nat_stack[62] = 1;    /* a fill also has to clear old NaT bits */
    cpu->gr_stack[63] = 0;
    cpu->gr_stack[64] = 0xfeed;
    cpu->nat_stack[64] = 1;
#define RSE_EXPECT(actual, expected, what) \
    rse_test_expect((actual), (expected), (what), fast, dirty, rewind, \
                     big_endian, fault)
    for (unsigned slot = 0; slot < 9; slot++)
        RSE_EXPECT(handoff_step(cpu, fast), MERCED_OK, "switch instruction");
    RSE_EXPECT(cpu->bof_total, 64, "BSPSTORE preserves current frame position");
    RSE_EXPECT(cpu->rse_flushed_regs, 64 - dirty, "BSPSTORE preserves dirty count");
    RSE_EXPECT(cpu->ar[17], rse_test_move(store, dirty), "BSP follows dirty count");
    RSE_EXPECT(cpu->gr_stack[cpu->bof], 0xfeed, "switch preserves current r32");
    RSE_EXPECT(cpu->nat_stack[cpu->bof], 1, "switch preserves current NaT");
    for (unsigned slot = 0; slot < 3; slot++)
        RSE_EXPECT(handoff_step(cpu, fast), MERCED_OK, "first return");
    if (fault) {
        RSE_EXPECT(cpu->ip, 0x9000, "fill enters alternate DTLB handler");
        RSE_EXPECT(cpu->nfaults, 1, "one fill fault");
        RSE_EXPECT(cpu->cr[19], 0x2000, "fill faults at return target");
        RSE_EXPECT((cpu->cr[16] >> 41) & 3, 0, "target slot is zero");
        RSE_EXPECT(cpu->cr[20], first_addr | UINT64_C(0x4000000000000000),
                    "missing backing-store address");
        RSE_EXPECT((cpu->cr[17] >> 34) & 0x19, 0x19, "ISR read, RSE, incomplete frame");
        RSE_EXPECT((cpu->cr[17] >> 43) & 1, 0, "RSE fault never reports ISR.ed");
        RSE_EXPECT(cpu->gr_stack[60], 0xaaaa, "failed fill does not commit prefix");
        RSE_EXPECT(cpu->rse_flushed_regs, 64 - dirty, "failed fill keeps boundary");
        for (unsigned slot = 0; slot < 3; slot++)
            RSE_EXPECT(handoff_step(cpu, fast), MERCED_OK, "map and rfi retry");
        RSE_EXPECT(cpu->interruption_rse_depth, 0, "rfi consumes interrupted snapshot");
    }
    RSE_EXPECT(cpu->ip, 0x2000, "target ready after required fills");
    RSE_EXPECT(cpu->bof_total, 60, "return frame popped exactly once");
    RSE_EXPECT(cpu->cfm, 5 | (4 << 7), "restored parent frame");
    RSE_EXPECT(cpu->rse_flushed_regs, 60, "filled frame is resident");
    for (unsigned i = 0; i < 4; i++) {
        RSE_EXPECT(cpu->gr_stack[60 + i], incoming[i], "incoming frame value");
        RSE_EXPECT(cpu->nat_stack[60 + i], i < 2, "incoming RNAT bit");
    }
    RSE_EXPECT(cpu->gr_stack[64], 0xfeed, "overlapping current register preserved");
    RSE_EXPECT(cpu->nat_stack[64], 1, "overlapping current NaT preserved");
    RSE_EXPECT(cpu->ar[19], first_rnat, "RNAT follows new spill boundary");
    for (unsigned slot = 0; slot < 6; slot++)
        RSE_EXPECT(handoff_step(cpu, fast), MERCED_OK, "restored parent returns");
    RSE_EXPECT(cpu->ip, 0x3000, "return uses incoming r34 without loadrs");
    RSE_EXPECT(cpu->nfaults, fault ? 1 : 0, "no additional fault");
#undef RSE_EXPECT
    merced_destroy(cpu);
}

/* flushrs must spill parent frames only, replace stale RNAT bits, and emit
 * a collection word when the backing store crosses a 512-byte boundary. */
static void check_rse_flush(bool fast, bool big_endian) {
    RseFixture fixture = {0};
    const uint64_t nop = UINT64_C(0x08000000);
    rse_test_bundle(&fixture, 0x1000, 1, UINT64_C(0x0c) << 27, nop, nop);
    MercedBus bus = {.ud = &fixture, .read = rse_test_read,
                     .fetch = rse_test_read, .write = rse_test_write};
    Merced *cpu = merced_create(&bus);
    if (!cpu) {
        failures++;
        return;
    }
    cpu->ip = 0x1000;
    cpu->psr = UINT64_C(1) << 13;
    cpu->cr[72] = UINT64_C(1) << 16;
    cpu->bof = cpu->bof_total = 64;
    cpu->cfm = 2;
    cpu->rse_flushed_regs = 60;
    cpu->rse_anchor_addr = 0x2410;
    cpu->rse_anchor_regs = 64;
    cpu->ar[16] = big_endian ? 16 : 0;
    cpu->ar[19] = (UINT64_C(1) << 61) | 1; /* old r60 NaT must be cleared */
    for (unsigned i = 60; i < 66; i++) {
        cpu->gr_stack[i] = UINT64_C(0x1122334455667700) | i;
        cpu->nat_stack[i] = i == 61 || i == 63;
    }
#define FLUSH_EXPECT(actual, expected, what) \
    rse_test_expect((actual), (expected), (what), fast, 4, false, big_endian, false)
    FLUSH_EXPECT(handoff_step(cpu, fast), MERCED_OK, "flushrs status");
    FLUSH_EXPECT(cpu->rse_flushed_regs, 64, "flush stops at BSP, excluding current frame");
    for (unsigned i = 60; i < 64; i++) {
        uint64_t expected = cpu->gr_stack[i];
        if (big_endian)
            expected = __builtin_bswap64(expected);
        FLUSH_EXPECT(rse_test_read(&fixture, rse_test_move(0x2410, (int)i - 64), 8),
                      expected, "flushed register value");
    }
    uint64_t full_rnat = (UINT64_C(1) << 62) | 1;
    FLUSH_EXPECT(rse_test_read(&fixture, 0x23f8, 8),
                  big_endian ? __builtin_bswap64(full_rnat) : full_rnat,
                  "stored RNAT preserves lower bits and replaces false NaT");
    FLUSH_EXPECT(cpu->ar[19], 2, "partial next-group RNAT");
    FLUSH_EXPECT(rse_test_read(&fixture, 0x2410, 8), 0, "current frame is not spilled");
    FLUSH_EXPECT(cpu->nfaults, 0, "physical flush has no faults");
#undef FLUSH_EXPECT
    merced_destroy(cpu);
}

typedef struct {
    RseFixture ram;
    unsigned fills;
} ZeroFillFixture;

static bool zero_fill_bulk(void *ud, uint64_t addr, uint8_t value, uint64_t len) {
    ZeroFillFixture *fixture = ud;
    fixture->fills++;
    if (addr >= sizeof(fixture->ram.memory) ||
        len > sizeof(fixture->ram.memory) - addr)
        return false;
    for (uint64_t i = 0; i < len; i++)
        fixture->ram.memory[addr + i] = value;
    return true;
}

/* The SAL clearing idiom also has valid virtual-memory semantics.  Adjacent
 * virtual pages need not be physically adjacent, and a first-store fault
 * must enter its handler without completing stores or base updates there. */
static void check_zero_fill(bool fast, unsigned scenario) {
    ZeroFillFixture fixture = {0};
    for (unsigned i = 0; i < sizeof(fixture.ram.memory); i++)
        fixture.ram.memory[i] = 0xa5;
    const uint64_t nop = UINT64_C(0x08000000);
    uint64_t store = (UINT64_C(5) << 37) | (UINT64_C(0x33) << 30) | (16 << 6);
    uint64_t loop = (UINT64_C(4) << 37) | (5 << 6);
    rse_test_bundle(&fixture.ram, 0x1000, 25,
                    store | (8 << 20), store | (9 << 20), loop);
    uint64_t insert = (UINT64_C(1) << 37) | (UINT64_C(0x2e) << 27) | (13 << 13);
    rse_test_bundle(&fixture.ram, 0x9000, 17, insert, nop, UINT64_C(8) << 27);
    MercedBus bus = {.ud = &fixture, .read = rse_test_read,
                     .fetch = rse_test_read, .write = rse_test_write,
                     .fill = zero_fill_bulk};
    Merced *cpu = merced_create(&bus);
    if (!cpu) {
        failures++;
        return;
    }
    const uint64_t va = scenario ? UINT64_C(0x4000000000021ff0) : 0x7ff0;
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 44) |
               (scenario ? UINT64_C(1) << 17 : 0);
    cpu->cfm = 0;
    cpu->cr[2] = 0x8000;
    cpu->cr[72] = UINT64_C(1) << 16;
    cpu->rr[2] = (3 << 8) | (13 << 2);
    cpu->gr_static[8] = va;
    cpu->gr_static[9] = va + 8;
    cpu->gr_static[13] = 0x6661;
    cpu->ar[65] = 1; /* ar.lc: two iterations, four eight-byte stores */
    cpu->dtr[0] = (MercedTlbEntry){
        .va_start = UINT64_C(0x4000000000020000),
        .va_end = UINT64_C(0x4000000000021fff),
        .pfn_base = 0x6000, .rid = 3, .ps = 13, .valid = scenario == 1,
        .itir = (3 << 8) | (13 << 2), .pte = 0x6661,
    };
    cpu->dtr[1] = (MercedTlbEntry){
        .va_start = UINT64_C(0x4000000000022000),
        .va_end = UINT64_C(0x4000000000023fff),
        .pfn_base = 0xa000, .rid = 3, .ps = 13, .valid = scenario != 0,
        .itir = (3 << 8) | (13 << 2), .pte = 0xa661,
    };
#define ZERO_EXPECT(actual, expected, what) \
    load_restart_expect((actual), (expected), (what), fast, false, scenario, 0)
    if (scenario == 2) {
        ZERO_EXPECT(handoff_step(cpu, fast), MERCED_OK, "zero-loop translation fault status");
        ZERO_EXPECT(cpu->ip, 0x9000, "zero-loop fault preserves handler entry");
        ZERO_EXPECT(cpu->nfaults, 1, "zero-loop raises exactly one fault");
        ZERO_EXPECT(cpu->cr[19], 0x1000, "zero-loop faults at original store");
        ZERO_EXPECT((cpu->cr[16] >> 41) & 3, 0, "zero-loop faults at slot zero");
        ZERO_EXPECT(cpu->gr_static[8], va, "fault preserves first postincrement base");
        ZERO_EXPECT(cpu->gr_static[9], va + 8, "fault preserves second base");
        ZERO_EXPECT(cpu->ar[65], 1, "fault preserves loop count");
        ZERO_EXPECT(fixture.fills, 0, "fault performs no bulk memory write");
        if (cpu->ip != 0x9000) {
            merced_destroy(cpu);
            return;
        }
        for (unsigned slot = 0; slot < 3; slot++)
            ZERO_EXPECT(handoff_step(cpu, fast), MERCED_OK, "zero-loop handler maps and returns");
        ZERO_EXPECT(cpu->ip, 0x1000, "zero-loop retries original store");
    }
    for (unsigned slot = 0; slot < 6 && cpu->ip != 0x1010; slot++)
        ZERO_EXPECT(handoff_step(cpu, fast), MERCED_OK, "zero-loop execution");
    ZERO_EXPECT(cpu->ip, 0x1010, "zero-loop finishes both iterations");
    ZERO_EXPECT(cpu->gr_static[8], va + 32, "first base advances once per store");
    ZERO_EXPECT(cpu->gr_static[9], va + 40, "second base advances once per store");
    ZERO_EXPECT(cpu->ar[65], 0, "loop count exhausted");
    ZERO_EXPECT(fixture.fills, scenario ? 0 : 1, "only physical clearing uses bulk fill");
    const uint64_t sentinel = UINT64_C(0xa5a5a5a5a5a5a5a5);
    ZERO_EXPECT(rse_test_read(&fixture, 0x7ff0, 8), 0, "first page cleared");
    ZERO_EXPECT(rse_test_read(&fixture, 0x7ff8, 8), 0, "last word of first page cleared");
    ZERO_EXPECT(rse_test_read(&fixture, scenario ? 0xa000 : 0x8000, 8), 0,
                 "second page cleared at its own physical mapping");
    ZERO_EXPECT(rse_test_read(&fixture, scenario ? 0xa008 : 0x8008, 8), 0,
                 "second page second word cleared");
    ZERO_EXPECT(rse_test_read(&fixture, scenario ? 0x8000 : 0xa000, 8), sentinel,
                 "unrelated physical page preserved");
    ZERO_EXPECT(rse_test_read(&fixture, 0x7fe8, 8), sentinel, "preceding bytes preserved");
    ZERO_EXPECT(cpu->nfaults, scenario == 2, "zero-loop has no extra faults");
#undef ZERO_EXPECT
    merced_destroy(cpu);
}

typedef struct {
    RseFixture ram;
    unsigned invalid_reads;
} PredicateIrqFixture;

static uint64_t predicate_irq_read(void *ud, uint64_t addr, unsigned size) {
    PredicateIrqFixture *fixture = ud;
    if (addr > sizeof(fixture->ram.memory) - size)
        fixture->invalid_reads++;
    return rse_test_read(ud, addr, size);
}

/* A four-stage rotating pipeline produces an address, loads its value and
 * accumulates it. NT's IRQ entry saves PR before cover and restores it with
 * RRB cleared, then rfi restores the interrupted CFM. Saving logical PR
 * instead of physical PR enables the load before its address is ready. */
static void check_predicate_irq_pipeline(bool fast, bool interrupt) {
    PredicateIrqFixture fixture = {0};
    const uint64_t nop = UINT64_C(0x08000000);
    const uint64_t adds = (UINT64_C(8) << 37) | (UINT64_C(2) << 34);
    uint64_t load = (UINT64_C(4) << 37) | (UINT64_C(3) << 30) |
                    (35 << 20) | (38 << 6) | 18; /* (p18) ld8 r38=[r35] */
    uint64_t advance = adds | (33 << 20) | (8 << 13) | (32 << 6) | 16;
    uint64_t address = adds | (33 << 20) | (34 << 6) | 17;
    uint64_t sum = (UINT64_C(8) << 37) | (39 << 20) | (20 << 13) |
                   (20 << 6) | 19; /* (p19) add r20=r20,r39 */
    uint64_t ctop = (UINT64_C(4) << 37) | (UINT64_C(1) << 36) |
                    (UINT64_C(0xfffff) << 13) | (7 << 6);
    rse_test_bundle(&fixture.ram, 0x1000, 1, nop,
                    UINT64_C(0x00400000040), nop); /* mov pr.rot=1<<16 */
    rse_test_bundle(&fixture.ram, 0x1010, 9, load, advance, address);
    rse_test_bundle(&fixture.ram, 0x1020, 17, sum, nop, ctop);
    /* The save and cover are the same instruction forms as NT's IRQ
     * prologue at831EB080. Acknowledge IVR before restoring PR and rfi. */
    rse_test_bundle(&fixture.ram, 0xb000, 17, nop,
                    UINT64_C(0x001980004c0), UINT64_C(2) << 27);
    uint64_t ivr = (UINT64_C(1) << 37) | (UINT64_C(0x24) << 27) |
                   (65 << 20) | (21 << 6);
    rse_test_bundle(&fixture.ram, 0xb010, 17, ivr,
                    UINT64_C(0x016ff027fc0), UINT64_C(8) << 27);
    for (unsigned i = 0; i < 4; i++)
        rse_test_write(&fixture.ram, 0x6000 + i * 8, i + 1, 8);
    MercedBus bus = {.ud = &fixture, .read = predicate_irq_read,
                     .fetch = predicate_irq_read, .write = rse_test_write};
    Merced *cpu = merced_create(&bus);
    if (!cpu) {
        failures++;
        return;
    }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 14) |
               (UINT64_C(1) << 44);
    cpu->cr[2] = 0x8000;
    cpu->cr[72] = UINT64_C(1) << 16;
    cpu->cfm = 8 | (8 << 7) | (1 << 14); /* eight rotating registers */
    cpu->ar[65] = 3;
    cpu->ar[66] = 4;
    for (unsigned i = 0; i < 8; i++)
        cpu->gr_stack[i] = UINT64_C(0xdead0000);
    cpu->gr_stack[1] = 0x5ff8;
    unsigned arrivals = 0;
    bool injected = false, returned = false;
    uint64_t saved_pr = 0, saved_cfm = 0;
#define PIPE_EXPECT(actual, expected, what) \
    handoff_expect((actual), (expected), (what), 0, fast, interrupt)
    for (unsigned step = 0; step < 100 && cpu->ip != 0x1030; step++) {
        if (cpu->ip == 0x1010) {
            arrivals++;
            if (interrupt && arrivals == 2) {
                saved_pr = cpu->pr;
                saved_cfm = cpu->cfm;
                merced_raise_external(cpu, 0x40);
                injected = true;
            } else if (injected && !returned) {
                PIPE_EXPECT(cpu->pr, saved_pr, "IRQ restores physical predicates");
                PIPE_EXPECT(cpu->cfm, saved_cfm, "rfi restores rotating frame");
                returned = true;
            }
        }
        PIPE_EXPECT(handoff_step(cpu, fast), MERCED_OK, "rotating pipeline instruction");
    }
    PIPE_EXPECT(cpu->ip, 0x1030, "rotating pipeline completes");
    PIPE_EXPECT(cpu->gr_static[20], 10, "all four produced values accumulated once");
    PIPE_EXPECT(fixture.invalid_reads, 0, "load waits for its address-producing stage");
    PIPE_EXPECT(cpu->nfaults, interrupt ? 1 : 0, "only the requested IRQ occurs");
    PIPE_EXPECT(cpu->interruption_rse_depth, 0, "IRQ return completes");
    PIPE_EXPECT(returned, interrupt, "interrupt resumes the pipeline");
#undef PIPE_EXPECT
    merced_destroy(cpu);
}

int main(void) {
    check_ifb_config();
    check_cmd649_config();
    check_acpi_enable();
    check_uhci_empty();
    check_chipset_bus_discovery();
    check_chipset_topology();
    gemu_monitor_set_default("none");
    check_rejected_memory(16);
    check_rejected_memory(96);
    check_rejected_memory(1792);
    check_memory(64);
    check_memory(128);
    check_memory(256);
    check_memory(512);
    check_memory(768);
    check_memory(1024);
    check_memory(1536);
    check_memory(2048);
    const unsigned handoff_ram[] = {512, 2048};
    for (unsigned i = 0; i < sizeof(handoff_ram) / sizeof(handoff_ram[0]); i++) {
        unsigned ram_mb = handoff_ram[i];
        check_efi_range_stop(ram_mb);
        for (unsigned fast = 0; fast < 2; fast++)
            for (unsigned timer = 0; timer < 2; timer++) {
                /* The actual Windows loader offset, outside the top16MiB
                 * firmware window, plus another valid EFI allocation well
                 * below the top64MiB to avoid an arbitrary-size workaround. */
                check_efi_handoff(ram_mb, fast, timer,
                    ((uint64_t)ram_mb << 20) - UINT64_C(0x0106e000));
                check_efi_handoff(ram_mb, fast, timer, UINT64_C(0x08000000));
            }
    }
    for (unsigned ifetch = 0; ifetch < 2; ifetch++) {
        for (unsigned long_format = 0; long_format < 2; long_format++) {
            for (unsigned ma = 1; ma <= 3; ma++)
                check_reserved_vhpt(long_format, ifetch, ma << 2, 0,
                                    "reserved MA");
            check_reserved_vhpt(long_format, ifetch, 2, 0, "PTE bit 1");
            check_reserved_vhpt(long_format, ifetch, UINT64_C(1) << 50, 0,
                                "PTE bit 50");
            check_reserved_vhpt(long_format, ifetch, UINT64_C(1) << 51, 0,
                                "PTE bit 51");
        }
        check_reserved_vhpt(true, ifetch, 0, 1, "ITIR bit 0");
        check_reserved_vhpt(true, ifetch, 0, UINT64_C(1) << 32, "ITIR bit 32");
        check_reserved_vhpt(true, ifetch, 0, 2 << 2, "unsupported page size");
    }
    for (unsigned fast = 0; fast < 2; fast++) {
        for (unsigned scenario = 0; scenario < 4; scenario++)
            check_region7_translation(fast, scenario);
        check_predicate_irq_pipeline(fast, false);
        check_predicate_irq_pipeline(fast, true);
        const unsigned predicate_rrbs[] = {0, 1, 17, 47};
        for (unsigned form = 0; form < 5; form++)
            for (unsigned i = 0; i < sizeof(predicate_rrbs) / sizeof(predicate_rrbs[0]); i++)
                for (unsigned nat = 0; nat < 2; nat++)
                    for (unsigned qualified = 0; qualified < 2; qualified++)
                        check_predicate_transfer(form, predicate_rrbs[i], qualified,
                                                  nat, fast);
        for (unsigned mode = 0; mode < 5; mode++)
            for (unsigned sample = 0; sample < 2; sample++)
                for (unsigned nat = 0; nat < 2; nat++)
                    for (unsigned qualified = 0; qualified < 2; qualified++)
                        check_mux1(mode, sample, nat, qualified, fast);
        for (unsigned scenario = 0; scenario < 3; scenario++)
            check_zero_fill(fast, scenario);
        for (unsigned big_endian = 0; big_endian < 2; big_endian++) {
            check_rse_flush(fast, big_endian);
            for (unsigned dirty = 0; dirty < 2; dirty++)
                for (unsigned rewind = 0; rewind < 2; rewind++)
                    for (unsigned fault = 0; fault < 2; fault++)
                        check_rse_context_switch(fast, dirty, rewind, big_endian, fault);
        }
        for (unsigned sample = 0; sample < 5; sample++)
            for (unsigned op = 0; op < 4; op++)
                for (unsigned nat = 0; nat < 3; nat++)
                    for (unsigned qualified = 0; qualified < 2; qualified++)
                        if (sample != 4 || nat != 1) /* f0 is never NaTVal */
                            check_fp_lanes(sample, op, nat, qualified, fast);
        for (unsigned unconditional = 0; unconditional < 2; unconditional++)
            for (unsigned tnat = 0; tnat < 2; tnat++)
                for (unsigned qualified = 0; qualified < 2; qualified++)
                    for (unsigned bit = 0; bit < 2; bit++)
                        for (unsigned nat = 0; nat < 2; nat++)
                            check_test_predicate(unconditional, tnat, qualified,
                                                 bit, nat, fast);
        for (unsigned reg_update = 0; reg_update < 2; reg_update++)
            for (unsigned scenario = 0; scenario < 3; scenario++)
                for (unsigned initial_nat = 0; initial_nat < 2; initial_nat++)
                    check_load_restart(fast, reg_update, scenario, initial_nat);
        for (unsigned x6 = 0x20; x6 <= 0x27; x6++)
            for (unsigned form = 0; form < 3; form++)
                check_fp_check_load(x6, form, 0, fast);
        for (unsigned scenario = 1; scenario <= 3; scenario++) {
            check_fp_check_load(0x23, 1, scenario, fast);
            check_fp_check_load(0x27, 2, scenario, fast);
        }
    }
    if (failures) {
        fprintf(stderr, "i2000 memory: %u checks failed\n", failures);
        return 1;
    }
    puts("i2000 memory: supported 64-2048 MiB populations and invalid-size checks passed");
    puts("i2000 CMD649: I/O BAR types, sizing, alignment and partial writes passed");
    puts("i2000 IFB: documented BAR layouts, command enables and relocated SMBus transactions passed");
    puts("i2000 ACPI: PMBASE decode, SCI enable/disable handshake and GPIO isolation passed");
    puts("i2000 UHCI: relocatable empty controller, reset, run/stop and root ports passed");
    puts("i2000 chipset: fixed CBN identity, discovery and bus relocation passed");
    puts("i2000 EFI handoff: slow/fast external and timer delivery checks passed");
    puts("Merced VHPT: short/long reserved entries fault without caching; repairs retry");
    puts("Merced region7: HAL I/O uses guest translations; unmapped references fault");
    puts("Merced FP check-loads: formats, updates, predication and faults passed");
    puts("Merced integer loads: precise fault restart and speculative deferral passed");
    puts("Merced tbit/tnat: unconditional predicate clearing and guarded branches passed");
    puts("Merced fmix/fsxt: lanes, source order, sign extension, format, NaTVal and predication passed");
    puts("Merced RSE: context switches, automatic fills, RNAT, dirty overlap and fault retry passed");
    puts("Merced zero-fill: physical acceleration, discontiguous mappings and fault retry passed");
    puts("Merced mux1: all byte permutations, low-byte broadcast, NaT and predication passed");
    puts("Merced rotating predicates: IRQ save, cover, restore and rfi preserve pipeline stages");
    puts("Merced predicate transfers: physical PR bits, immediate sign extension, qualifiers and NaT passed");
    return 0;
}
