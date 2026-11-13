/* Regression coverage for the 460GX memory-card SPD configuration window.
 * Uses the public physical bus interface; no firmware image is required.
 * Run with: python3 tools/ia64/run_i2000_memory_test.py */
#include "i2000.h"
#include "merced.h"
#include "gemu/monitor.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

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
    /* The SMBus segment carries the processor cartridge's Scratch EEPROM at
     * slave 50h, erased and writable. Enable the host controller, store a
     * byte and read it back. */
    config_select_bus(s, 0, 3, 3, 0x40);
    ia64_i2000_phys_write8(s, io_address(0xcfc), 1);
    ia64_i2000_phys_write8(s, io_address(0x1a03), 2); /* word address */
    ia64_i2000_phys_write8(s, io_address(0x1a05), 0x4a);
    ia64_i2000_phys_write8(s, io_address(0x1a04), 0xa0); /* slave50 write */
    ia64_i2000_phys_write8(s, io_address(0x1a02), 0x48); /* byte-data start */
    expect(ia64_i2000_phys_read8(s, io_address(0x1a00)), 2,
           "SMBus transaction completes at assigned base", 64, 3);
    ia64_i2000_phys_write8(s, io_address(0x1a00), 2);
    ia64_i2000_phys_write8(s, io_address(0x1a05), 0);
    ia64_i2000_phys_write8(s, io_address(0x1a04), 0xa1); /* slave50 read */
    ia64_i2000_phys_write8(s, io_address(0x1a02), 0x48);
    expect(ia64_i2000_phys_read8(s, io_address(0x1a05)), 0x4a,
           "SMBus reads back a device on its segment", 64, 3);
    ia64_i2000_phys_write8(s, io_address(0x1a00), 2);
    expect(ia64_i2000_phys_read8(s, io_address(0x1a00)), 0,
           "SMBus completion is write-one-to-clear", 64, 3);
    config_select_bus(s, 0, 3, 3, 0x20);
    ia64_i2000_phys_write32(s, io_address(0xcfc), 0x1b00);
    expect(ia64_i2000_phys_read8(s, io_address(0x1a00)), 0xff,
           "SMBus old I/O address is unmapped", 64, 3);
    expect(ia64_i2000_phys_read8(s, io_address(0x1b05)), 0x4a,
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
    expect(ia64_i2000_phys_read8(s, io_address(0xa04)), 0,
           "APM command alone does not set SCI_EN (the firmware's PMI "
           "handler does)", 64, 0);
    expect(ia64_i2000_phys_read8(s, io_address(0xb2)), 0xa0,
           "APM command latch", 64, 0);
    expect(ia64_i2000_phys_read32(s, io_address(0x404)), UINT32_MAX,
           "the GPIO block does not decode until enabled", 64, 0);
    ia64_i2000_phys_write32(s, io_address(0xcf8), 0x800018d0);
    ia64_i2000_phys_write32(s, io_address(0xcfc), 0x401);
    ia64_i2000_phys_write32(s, io_address(0xcf8), 0x800018d4);
    ia64_i2000_phys_write32(s, io_address(0xcfc), 1);
    expect(ia64_i2000_phys_read32(s, io_address(0x404)), 0x010401ff,
           "GPIO data carries the J29 strap on its input pins", 64, 0);
    ia64_i2000_phys_write8(s, io_address(0xa05), 4);
    expect(ia64_i2000_phys_read32(s, io_address(0xa04)), 0x400,
           "PM1 control keeps SLP_TYP", 64, 0);
    ia64_i2000_phys_write32(s, io_address(0xa00), UINT32_MAX);
    expect(ia64_i2000_phys_read32(s, io_address(0xa00)), 0x05210000,
           "PM1 status is write-one-to-clear; defined enables are retained",
           64, 0);
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
                              uint64_t entry, bool ami_context) {
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
    if (ami_context) {
        cpu->cr[8] = 0x40; /* AMI shell's PTA/RR0 during physical app calls. */
        cpu->rr[0] = 0x100030;
    }
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

/* IVA=0 is architecturally valid. The low-application exception must not
 * suppress interrupts outside the observed firmware execution context. */
static void check_low_efi_interrupt(bool fast, bool timer, unsigned scenario) {
    const unsigned ram_mb = 512;
    MercedBus bus = {.ram_size = (uint64_t)ram_mb << 20,
                     .read = handoff_read, .write = handoff_write};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    uint64_t entry = 0x01042000;
    cpu->ip = entry;
    cpu->cr[8] = 0x40;
    cpu->rr[0] = 0x100030;
    cpu->cr[72] = UINT64_C(1) << 16;
    for (unsigned slot = 0; slot < 3; slot++)
        handoff_expect(handoff_step(cpu, fast), MERCED_OK, "cache warmup",
                       ram_mb, fast, timer);
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 14);
    switch (scenario) {
    case 0: merced_set_cpu_revision(cpu, 6); break;
    case 1: cpu->cr[8] = 0; break;
    case 2: cpu->rr[0] = 0x30; break;
    case 3: cpu->psr |= UINT64_C(1) << 36; break; /* virtual instruction */
    case 4: cpu->psr |= UINT64_C(1) << 17; break; /* virtual data */
    case 5: cpu->psr |= UINT64_C(1) << 27; break; /* virtual RSE */
    case 6: entry = 0x10000; break;             /* low interrupt vectors */
    case 7: cpu->bus.ram_size = entry; break;   /* unbacked address */
    case 8: cpu->psr |= UINT64_C(3) << 32; break; /* user privilege */
    }
    cpu->ip = entry;
    cpu->group_start = 1;
    if (timer) {
        cpu->cr[72] = 0x40;
        cpu->timer_pending = true;
    } else {
        merced_raise_external(cpu, 0);
    }
    handoff_expect(handoff_step(cpu, fast), MERCED_OK, "low-code IRQ status",
                   ram_mb, fast, timer);
    handoff_expect(cpu->ip, 0x3000, "IVA0 interrupt delivered outside AMI context",
                   ram_mb, fast, timer);
    handoff_expect(cpu->nfaults, 1, "one low-code interrupt", ram_mb, fast, timer);
    handoff_expect(cpu->cr[19], entry, "saved low-code IP", ram_mb, fast, timer);
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

/* epc promotes to the privilege level of its own page when that page has
 * execute/promote rights (TLB.ar = 7), sets CPL 0 with instruction
 * translation off, and faults when PFS.ppl is more privileged than CPL.
 * br.ret then demotes to PFS.ppl, never promotes.  Windows' system-call
 * gateway (E0000000FFA00020) depends on both; without them SMSS faulted
 * reading the PCR and Setup stopped with SESSION5_INITIALIZATION_FAILED
 * (Intel SDM Vol.3 epc, br.ret; Vol.2 4.1.1.6). */
static void check_epc(unsigned scenario, bool fast) {
    static const struct {
        bool it; unsigned ar, pl, cpl, ppl, expect_cpl; bool fault;
        unsigned ret_ppl, expect_ret_cpl;
    } cases[] = {
        {true,  7, 0, 3, 3, 0, false, 3, 3},  /* gateway: promote, return demotes */
        {true,  7, 1, 3, 3, 1, false, 3, 3},  /* promote to the page's own PL */
        {true,  3, 3, 3, 3, 3, false, 3, 3},  /* no promote rights: unchanged */
        {true,  7, 0, 3, 0, 3, true,  0, 0},  /* PFS.ppl < CPL: illegal op */
        {false, 3, 3, 3, 3, 0, false, 3, 3},  /* translation off: CPL 0 */
        {true,  7, 0, 3, 3, 0, false, 0, 0},  /* br.ret never promotes... */
        {true,  7, 2, 2, 2, 2, false, 3, 3},  /* ...but does demote */
    };
    const __uint128_t nop = UINT64_C(0x08000000);
    const __uint128_t epc = UINT64_C(0x10) << 27;
    const __uint128_t ret = (UINT64_C(0x21) << 27) | (4 << 6);   /* br.ret b0 */
    TestPredicateFixture fixture = {.bundle = {
        0x11 | (nop << 5) | (nop << 46) | (epc << 87),
        0x11 | (nop << 5) | (nop << 46) | (ret << 87),
    }};
    MercedBus bus = {.ud = &fixture, .read = test_predicate_read,
                     .fetch = test_predicate_read, .write = handoff_write};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    const unsigned rid = 5;
    cpu->ip = 0x1000;
    cpu->cr[2] = 0x8000;                       /* IVA */
    cpu->cr[72] = UINT64_C(1) << 16;           /* ITV masked */
    cpu->rr[0] = ((uint64_t)rid << 8) | (13 << 2);
    cpu->itr[0] = (MercedTlbEntry){
        .va_start = 0, .va_end = 0x1fff, .pfn_base = 0, .rid = rid, .ps = 13,
        .valid = 1, .itir = 13 << 2,
        .pte = 1 | (UINT64_C(1) << 5) | ((uint64_t)cases[scenario].ar << 9) |
               ((uint64_t)cases[scenario].pl << 7),
    };
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 44) |
               (cases[scenario].it ? UINT64_C(1) << 36 : 0) |
               ((uint64_t)cases[scenario].cpl << 32);
    cpu->ar[64] = (uint64_t)cases[scenario].ppl << 62;     /* ar.pfs */
    cpu->br[0] = 0x3000;
    bool ok = true;
    for (unsigned i = 0; i < 3; i++)
        ok &= handoff_step(cpu, fast) == MERCED_OK;
    unsigned cpl = (unsigned)((cpu->psr >> 32) & 3);
    bool faulted = cpu->nfaults != 0;
    if (!ok || faulted != cases[scenario].fault ||
        (cases[scenario].fault ? cpu->ip != 0xd400
                               : (cpl != cases[scenario].expect_cpl ||
                                  cpu->ip != 0x1010))) {
        fprintf(stderr, "epc scenario %u %s: cpl=%u/%u faults=%" PRIu64
                " ip=%016" PRIx64 "\n", scenario, fast ? "fast" : "slow",
                cpl, cases[scenario].expect_cpl, cpu->nfaults, cpu->ip);
        failures++;
    } else if (!cases[scenario].fault) {
        cpu->ar[64] = (uint64_t)cases[scenario].ret_ppl << 62;
        for (unsigned i = 0; i < 3; i++)
            ok &= handoff_step(cpu, fast) == MERCED_OK;
        cpl = (unsigned)((cpu->psr >> 32) & 3);
        if (!ok || cpu->ip != 0x3000 || cpu->nfaults != 0 ||
            cpl != (cases[scenario].expect_ret_cpl > cases[scenario].expect_cpl
                    ? cases[scenario].expect_ret_cpl
                    : cases[scenario].expect_cpl)) {
            fprintf(stderr, "br.ret after epc scenario %u %s: cpl=%u ip=%016"
                    PRIx64 " faults=%" PRIu64 "\n", scenario,
                    fast ? "fast" : "slow", cpl, cpu->ip, cpu->nfaults);
            failures++;
        }
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

/* An interruption handler may select a different backing store and resume at
 * the same IIP/IPSR. Also exercise two interrupted frames at a common fault
 * address: dispatching the nested interruption to another continuation must
 * not let a later rfi resurrect the older frame. NT's CcUnpinData takes this
 * path while switching threads during Setup; stale registers corrupt its
 * caller's saved return address. All transitions below execute guest code. */
static void check_rse_interrupted_context(bool fast, bool nested,
                                           bool switch_store) {
    RseFixture fixture = {0};
    const uint64_t nop = UINT64_C(0x08000000);
    const uint64_t mov_ar = (UINT64_C(1) << 37) | (UINT64_C(0x2a) << 27);
    const uint64_t mov_cr = (UINT64_C(1) << 37) | (UINT64_C(0x2c) << 27);
    const uint64_t rfi = UINT64_C(8) << 27;
    const uint64_t psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 17) |
                         (UINT64_C(1) << 44);
    const uint64_t cfm = 4 | (2 << 7);
    const uint64_t store = switch_store ? 0x4410 : 0x2410;
    const uint64_t load = (UINT64_C(4) << 37) | (UINT64_C(3) << 30) |
                          (10 << 20) | (8 << 6); /* ld8 r8=[r10] */
    rse_test_bundle(&fixture, 0x3000, 9, nop, load, nop);
    rse_test_bundle(&fixture, 0x9000, 17, nop, nop, UINT64_C(2) << 27);
    rse_test_bundle(&fixture, 0x9010, 1, UINT64_C(0x0c) << 27, nop, nop);
    if (nested) {
        /* cover/flushrs, alloc a different frame, then re-enable collection
         * and fault at the same load. Give the nested handler its own IVA. */
        uint64_t alloc = (UINT64_C(1) << 37) | (UINT64_C(6) << 33) |
                         (4 << 13) | (2 << 20) | (8 << 6);
        rse_test_bundle(&fixture, 0x9020, 1, alloc, nop, nop);
        rse_test_bundle(&fixture, 0x9030, 1, mov_cr | (2 << 20) | (5 << 13), nop, nop);
        rse_test_bundle(&fixture, 0x9040, 1,
                        (UINT64_C(6) << 27) | (UINT64_C(0x22000) << 6), nop, nop);
        rse_test_bundle(&fixture, 0x9050, 17,
                        UINT64_C(0x30) << 27, nop, UINT64_C(0x0d) << 27);
        rse_test_bundle(&fixture, 0x9060, 17,
                        nop, nop, (UINT64_C(0x20) << 27) | (1 << 13));
        /* The second handler switches to a continuation at 0x2000 with an
         * empty frame. It later resumes the second fault from its backing
         * store, at precisely the first fault's IIP/IPSR. */
        rse_test_bundle(&fixture, 0x1000, 17, nop, nop, UINT64_C(2) << 27);
        rse_test_bundle(&fixture, 0x1010, 1, UINT64_C(0x0c) << 27, nop, nop);
        rse_test_bundle(&fixture, 0x1020, 1, mov_cr | (19 << 20) | (14 << 13), nop, nop);
        rse_test_bundle(&fixture, 0x1030, 1, mov_cr | (23 << 20) | (15 << 13), nop, nop);
        rse_test_bundle(&fixture, 0x1040, 1, mov_cr | (16 << 20) | (7 << 13), nop, nop);
        rse_test_bundle(&fixture, 0x1050, 17, nop, nop, rfi);
    }
    uint64_t resume = nested ? 0x2000 : 0x9020;
    rse_test_bundle(&fixture, resume, 1, mov_ar | (18 << 20) | (25 << 13), nop, nop);
    rse_test_bundle(&fixture, resume + 0x10, 1, mov_ar | (19 << 20) | (24 << 13), nop, nop);
    rse_test_bundle(&fixture, resume + 0x20, 1, mov_ar | (16 << 20) | (23 << 13), nop, nop);
    rse_test_bundle(&fixture, resume + 0x30, 1, UINT64_C(0x0a) << 27, nop, nop);
    rse_test_bundle(&fixture, resume + 0x40, 1, mov_cr | (19 << 20) | (11 << 13), nop, nop);
    rse_test_bundle(&fixture, resume + 0x50, 1, mov_cr | (23 << 20) | (9 << 13), nop, nop);
    rse_test_bundle(&fixture, resume + 0x60, 1, mov_cr | (16 << 20) | (6 << 13), nop, nop);
    uint64_t insert = (UINT64_C(1) << 37) | (UINT64_C(0x2e) << 27) | (13 << 13);
    rse_test_bundle(&fixture, resume + 0x70, 17, insert, nop, rfi);

    for (unsigned i = 0; i < 4; i++)
        rse_test_write(&fixture, rse_test_move(store, (int)i - 4), 0x1110 + i, 8);
    rse_test_write(&fixture, store - 24, UINT64_C(1) << 62, 8); /* RNAT word */
    rse_test_write(&fixture, 0x6020, UINT64_C(0x778899aabbccddee), 8);
    MercedBus bus = {.ud = &fixture, .read = rse_test_read,
                     .fetch = rse_test_read, .write = rse_test_write};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x3000;
    cpu->psr = psr;
    cpu->cr[2] = 0x8000;
    cpu->cr[72] = UINT64_C(1) << 16;
    cpu->rr[2] = (3 << 8) | (13 << 2);
    cpu->gr_static[5] = 0;
    cpu->gr_static[6] = psr | (UINT64_C(1) << 41); /* RI=1 */
    cpu->gr_static[7] = psr;
    cpu->gr_static[9] = (UINT64_C(1) << 63) | cfm;
    cpu->gr_static[10] = UINT64_C(0x4000000000006020);
    cpu->gr_static[11] = 0x3000;
    cpu->gr_static[13] = 0x6661; /* present writable PTE for the faulting load */
    cpu->gr_static[14] = 0x2000;
    cpu->gr_static[15] = UINT64_C(1) << 63;
    cpu->br[1] = 0x3000;
    cpu->gr_bank0[25 - 16] = cpu->gr_static[25] = store;
    cpu->gr_bank0[24 - 16] = cpu->gr_static[24] = 1;
    cpu->gr_bank0[23 - 16] = cpu->gr_static[23] = UINT64_C(40) << 16;
    cpu->bof = cpu->bof_total = 64;
    cpu->cfm = cfm;
    cpu->rse_anchor_addr = 0x23e8;
    cpu->rse_anchor_regs = cpu->rse_flushed_regs = 64;
    for (unsigned i = 0; i < 4; i++) {
        cpu->gr_stack[64 + i] = 0xaaa0 + i;
        cpu->nat_stack[64 + i] = i == 1 || i == 2;
    }
#define INTERRUPT_EXPECT(actual, expected, what) \
    rse_test_expect((actual), (expected), (what), fast, nested, switch_store, false, true)
    /* First execute the fault; stopping at 0x3001 afterwards means that rfi
     * has resumed it, rather than stopping before the original instruction. */
    INTERRUPT_EXPECT(handoff_step(cpu, fast), MERCED_OK, "initial nop");
    INTERRUPT_EXPECT(handoff_step(cpu, fast), MERCED_OK, "initial load fault");
    for (unsigned step = 0; step < 100 &&
         (cpu->ip != 0x3001 || cpu->nfaults < (nested ? 2u : 1u)); step++)
        INTERRUPT_EXPECT(handoff_step(cpu, fast), MERCED_OK, "interrupted context transition");
    unsigned frame = nested ? 68 : 64;
    INTERRUPT_EXPECT(cpu->ip, 0x3001, "same IIP/IPSR resumes");
    INTERRUPT_EXPECT(cpu->bof_total, frame, "correct interrupted frame restored");
    for (unsigned i = 0; i < 4; i++) {
        INTERRUPT_EXPECT(merced_gr(cpu, 32 + i), (switch_store ? 0x1110 : 0xaaa0) + i,
                          "handler-selected saved register");
        INTERRUPT_EXPECT(cpu->nat_stack[frame + i], i == 1 || i == 2,
                          "handler-selected saved NaT");
    }
    uint64_t bsp = rse_test_move(cpu->rse_anchor_addr,
                               (int64_t)cpu->bof_total - cpu->rse_anchor_regs);
    INTERRUPT_EXPECT(bsp, rse_test_move(store, -4), "selected backing store retained");
    INTERRUPT_EXPECT(handoff_step(cpu, fast), MERCED_OK, "faulting load retries");
    INTERRUPT_EXPECT(cpu->gr_static[8], UINT64_C(0x778899aabbccddee), "retried load result");
    INTERRUPT_EXPECT(cpu->nfaults, nested ? 2 : 1, "only the intended faults occur");
#undef INTERRUPT_EXPECT
    merced_destroy(cpu);
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

/* Windows disk.sys executes the literal chk.a.clr at +0x17ed0.  Its ALAT
 * miss must reach the recovery block 0x170 bytes ahead, preserving the
 * caller's frame.  Taking the checked register number (r30) as part of
 * the displacement instead enters an unrelated function and crashes. */
static void check_advanced_branch(bool fast, unsigned form, bool backward,
                                  bool qualified, bool warm) {
    static const uint64_t opcodes[2][4] = {
        {UINT64_C(0x0080002e780), UINT64_C(0x00a0002e780),
         UINT64_C(0x00c0002e780), UINT64_C(0x00e0002e780)},
        {UINT64_C(0x019fffd2780), UINT64_C(0x01bfffd2780),
         UINT64_C(0x01dfffd2780), UINT64_C(0x01ffffd2780)},
    }; /* chk.a.{nc,clr} {r30,f30}, IP +/- 0x170 */
    const uint64_t nop = UINT64_C(0x08000000);
    const uint64_t value = UINT64_C(0x55667788);
    RseFixture fixture = {0};
    uint64_t raw = opcodes[backward][form] | (qualified ? 0 : 1);
    rse_test_bundle(&fixture, 0x3000, 9, nop, raw, nop);
    /* A real advanced load establishes the integer ALAT association. */
    rse_test_bundle(&fixture, 0x1000, 9,
                    UINT64_C(0x08281d00780), nop, nop); /* ld4.a r30=[r29] */
    rse_test_write(&fixture, 0x5000, value, 4);
    MercedBus bus = {.ud = &fixture, .read = rse_test_read,
                     .write = rse_test_write, .ram_size = sizeof(fixture.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->psr = 0;
    cpu->ip = 0x1000;
    cpu->gr_static[29] = 0x5000;
    cpu->gr_static[30] = value;
#define CHK_EXPECT(actual, expected, what) do { \
    uint64_t a_ = (actual), e_ = (expected); \
    if (a_ != e_) { \
        fprintf(stderr, "chk.a %s form=%u back=%u qualified=%u warm=%u: " \
                "%s: got 0x%" PRIx64 ", expected 0x%" PRIx64 "\n", \
                fast ? "fast" : "slow", form, backward, qualified, warm, \
                what, a_, e_); failures++; \
    } \
} while (0)
    if (warm) {
        CHK_EXPECT(handoff_step(cpu, fast), MERCED_OK, "advanced load status");
        CHK_EXPECT(cpu->gr_static[30], value, "advanced load value");
        CHK_EXPECT(cpu->alat_valid_mask != 0, 1, "advanced load creates entry");
    }
    cpu->ip = 0x3000;
    cpu->cfm = 35 | (UINT64_C(30) << 7);
    cpu->bof = cpu->bof_total = 8;
    cpu->ar[64] = UINT64_C(0x12345678); /* PFS */
    cpu->br[0] = 0x8760;
    CHK_EXPECT(handoff_step(cpu, fast), MERCED_OK, "decode-cache warmup");
    CHK_EXPECT(handoff_step(cpu, fast), MERCED_OK, "advanced check status");
    bool hit = warm && form < 2;
    uint64_t target = backward ? 0x2e90 : 0x3170;
    CHK_EXPECT(cpu->ip, qualified && !hit ? target : 0x3002,
               "recovery branch or fallthrough");
    CHK_EXPECT(cpu->gr_static[30], value, "checked value unchanged");
    CHK_EXPECT(cpu->cfm, 35 | (UINT64_C(30) << 7), "frame unchanged");
    CHK_EXPECT(cpu->bof, 8, "physical frame position unchanged");
    CHK_EXPECT(cpu->bof_total, 8, "logical frame position unchanged");
    CHK_EXPECT(cpu->ar[64], UINT64_C(0x12345678), "PFS unchanged");
    CHK_EXPECT(cpu->br[0], 0x8760, "return address unchanged");
    CHK_EXPECT(cpu->nfaults, 0, "check does not fault");
    CHK_EXPECT(cpu->alat_valid_mask != 0,
               warm && !(qualified && form == 1),
               "only a qualified integer clr consumes the integer entry");
#undef CHK_EXPECT
    merced_destroy(cpu);
}

/* The first GUI session executes (p15) chk.s f8,IP+0x670 in MMB slot 1.
 * Exercise that literal instruction and the signed backward encoding, with
 * ordinary/rotating FP sources, constant registers and the integer M20 form. */
static void check_control_branch(bool fast, unsigned source, bool backward,
                                 bool qualified, bool nat, unsigned disabled) {
    static const unsigned regs[] = {8, 40, 0, 1, 8, 8, 8};
    const unsigned reg = regs[source];
    const bool fp = source != 6;
    const unsigned physical = reg >= 32 ? reg + 7 : reg;
    const uint64_t nop = UINT64_C(0x08000000);
    uint64_t raw = backward ? UINT64_C(0x037fff1064f) :
                              UINT64_C(0x026000119cf);
    raw = (raw & ~(UINT64_C(127) << 13)) | ((uint64_t)reg << 13);
    if (!fp)
        raw &= ~(UINT64_C(2) << 33); /* x3=1: chk.s.m */
    RseFixture fixture = {0};
    rse_test_bundle(&fixture, 0x3000, 0x19, nop, raw, 0);
    MercedBus bus = {.ud = &fixture, .read = rse_test_read,
                     .write = rse_test_write, .ram_size = sizeof(fixture.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x3000;
    cpu->psr = (UINT64_C(1) << 13) | ((uint64_t)disabled << 18);
    cpu->pr = 1 | (qualified ? UINT64_C(1) << 15 : 0);
    cpu->cr[2] = 0x8000;
    cpu->cr[72] = UINT64_C(1) << 16;
    cpu->cfm = 35 | (UINT64_C(30) << 7) | (UINT64_C(7) << 25);
    cpu->bof = cpu->bof_total = 8;
    cpu->ar[64] = UINT64_C(0x12345678);
    cpu->br[0] = 0x8760;
    cpu->gr_static[8] = UINT64_C(0x1122334455667788);
    cpu->nat_static[8] = fp ? !nat : nat; /* distinguish FR from GR */
    MercedFpReg value = {.sig = UINT64_C(0xc000000000000000),
                         .exp = 0x1ffff, .nat = nat};
    if (source == 4 || source == 5) {
        /* Register-format NaTVal, plus its sign-one pseudo-zero neighbor. */
        value.nat = 0;
        value.sign = source == 5;
        if (nat) { value.sig = 0; value.exp = 0x1fffe; }
    }
    cpu->fr[physical] = value;
    uint64_t original_cfm = cpu->cfm;
#define CHKS_EXPECT(actual, expected, what) do { \
    uint64_t a_ = (actual), e_ = (expected); \
    if (a_ != e_) { \
        fprintf(stderr, "chk.s %s src=%u back=%u qualified=%u nat=%u disabled=%u: " \
                "%s: got 0x%" PRIx64 ", expected 0x%" PRIx64 "\n", \
                fast ? "fast" : "slow", source, backward, qualified, nat, disabled, \
                what, a_, e_); failures++; \
    } \
} while (0)
    CHKS_EXPECT(handoff_step(cpu, fast), MERCED_OK, "decode-cache warmup");
    CHKS_EXPECT(handoff_step(cpu, fast), MERCED_OK, "speculation check status");
    unsigned fault_code = fp && reg >= 2 ? (reg < 32 ? disabled & 1 : disabled & 2) : 0;
    bool fault = qualified && fault_code;
    bool branch = qualified && nat && !(fp && reg < 2) && source != 5;
    uint64_t target = backward ? 0x2990 : 0x3670;
    CHKS_EXPECT(cpu->ip, fault ? 0xd500 : branch ? target : 0x3002,
                 "disabled fault, recovery branch or fallthrough");
    CHKS_EXPECT(cpu->nfaults, fault, "only disabled FP access faults");
    CHKS_EXPECT(cpu->fr[physical].sig, value.sig, "source significand preserved");
    CHKS_EXPECT(cpu->fr[physical].exp, value.exp, "source exponent preserved");
    CHKS_EXPECT(cpu->fr[physical].sign, value.sign, "source sign preserved");
    CHKS_EXPECT(cpu->fr[physical].nat, value.nat, "source NaTVal preserved");
    CHKS_EXPECT(cpu->ar[64], UINT64_C(0x12345678), "PFS preserved");
    CHKS_EXPECT(cpu->br[0], 0x8760, "return address preserved");
    CHKS_EXPECT(cpu->bof, 8, "physical frame position preserved");
    CHKS_EXPECT(cpu->bof_total, 8, "logical frame position preserved");
    if (fault) {
        CHKS_EXPECT(cpu->cr[17] & 0xffff, fault_code, "disabled register bank code");
        CHKS_EXPECT(cpu->cr[19], 0x3000, "fault saves bundle address");
        CHKS_EXPECT((cpu->cr[16] >> 41) & 3, 1, "fault saves slot one");
    } else {
        CHKS_EXPECT(cpu->cfm, original_cfm, "frame preserved across branch");
    }
#undef CHKS_EXPECT
    merced_destroy(cpu);
}

typedef struct {
    RseFixture ram;
    unsigned reads, writes;
} Ia32CmpxchgFixture;

static uint64_t ia32_cmpxchg_read(void *ud, uint64_t addr, unsigned size) {
    Ia32CmpxchgFixture *f = ud;
    if (addr >= 0x5000 && addr < 0x8000) f->reads++;
    return rse_test_read(&f->ram, addr, size);
}

static void ia32_cmpxchg_write(void *ud, uint64_t addr, uint64_t value,
                              unsigned size) {
    Ia32CmpxchgFixture *f = ud;
    if (addr >= 0x5000 && addr < 0x8000) f->writes++;
    rse_test_write(&f->ram, addr, value, size);
}

/* IA-32 setup helpers use CMPXCHG for synchronization. A mismatch still
 * writes the original memory value, including write-permission checks.
 * Flags and EAX must remain intact if that write faults, then retry once. */
static void check_ia32_cmpxchg(bool fast, unsigned size, unsigned form,
                              unsigned sample, unsigned fault) {
    Ia32CmpxchgFixture f = {0};
    const uint32_t mask = size == 4 ? UINT32_MAX : (1u << (size * 8)) - 1;
    const uint32_t acc = sample == 0 ? 0x81 : sample == 1 ? 0 : 1u << (size * 8 - 1);
    const uint32_t dest = sample == 0 ? acc : 1;
    const uint32_t src = UINT32_C(0x765432a5) & mask;
    const uint64_t upper = UINT64_C(0xfedcba9800000000);
    const uint32_t initial_flags = 0xe97; /* arithmetic bits plus IF and DF */
    static const uint32_t flags[] = {0x44, 0x95, 0x814};
    const uint32_t expected_flags = sample == 2 && size == 1 ? 0x810 : flags[sample];
    const uint32_t final_flags = (initial_flags & ~0x8d5u) | expected_flags;
    unsigned len = 0;
    if (form == 0) f.ram.memory[0x1000 + len++] = 0xf0; /* LOCK memory form */
    if (size == 2) f.ram.memory[0x1000 + len++] = 0x66;
    f.ram.memory[0x1000 + len++] = 0x0f;
    f.ram.memory[0x1000 + len++] = size == 1 ? 0xb0 : 0xb1;
    /* [edx],ecx; ebx,ecx; ah,cl; bl,ah. Last two expose byte aliases. */
    static const uint8_t modrm[] = {0x0a, 0xcb, 0xcc, 0xe3};
    f.ram.memory[0x1000 + len++] = modrm[form];
    rse_test_write(&f.ram, 0x5000, dest, size);
    const uint64_t nop = UINT64_C(0x08000000);
    rse_test_bundle(&f.ram, 0xd300, 0x11, nop, nop, UINT64_C(8) << 27);
    rse_test_bundle(&f.ram, 0xd000, 0x11, nop, nop, UINT64_C(8) << 27);
    MercedBus bus = {.ud = &f, .read = ia32_cmpxchg_read,
                     .write = ia32_cmpxchg_write, .ram_size = sizeof(f.ram.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 17) |
               (UINT64_C(1) << 34) | (UINT64_C(1) << 44); /* IC, DT, IS, BN */
    cpu->ar[24] = upper | initial_flags;
    cpu->gr_static[25] = UINT64_C(1) << 62; /* 32-bit CS */
    cpu->gr_static[8] = upper | UINT32_C(0xabcd0000) | acc;
    if (size == 4) cpu->gr_static[8] = upper | acc;
    if (form == 2 || form == 3)
        cpu->gr_static[8] = (cpu->gr_static[8] & ~UINT64_C(0xff00)) |
                           ((uint64_t)(form == 2 ? dest : src) << 8);
    cpu->gr_static[9] = upper | src;
    cpu->gr_static[10] = 0x4000;
    cpu->gr_static[11] = upper | UINT32_C(0x12340000) | dest;
    if (size == 4) cpu->gr_static[11] = upper | dest;
    uint64_t old_acc = cpu->gr_static[8], old_dest = cpu->gr_static[11];
    cpu->cr[2] = 0x8000;
    cpu->cr[72] = UINT64_C(1) << 16;
    cpu->rr[0] = (1 << 8) | (12 << 2);
    cpu->dtr[0] = (MercedTlbEntry) {
        .va_start = 0x4000, .va_end = 0x4fff, .pfn_base = 0x5000,
        .rid = 1, .ps = 12, .valid = 1, .itir = (1 << 8) | (12 << 2),
        .pte = fault == 1 ? 0x5061 : fault == 2 ? 0x5460 : 0x5461,
    };
#define CMPX_EXPECT(actual, expected, what) do { \
    uint64_t a_ = (actual), e_ = (expected); \
    if (a_ != e_) { \
        fprintf(stderr, "IA32 cmpxchg %s size=%u form=%u sample=%u fault=%u: " \
                "%s: got 0x%" PRIx64 ", expected 0x%" PRIx64 "\n", \
                fast ? "fast" : "slow", size, form, sample, fault, \
                what, a_, e_); failures++; \
    } \
} while (0)
    CMPX_EXPECT(handoff_step(cpu, fast), MERCED_OK, "instruction status");
    if (fault) {
        CMPX_EXPECT(cpu->nfaults, 1, "one access fault");
        CMPX_EXPECT(cpu->ip, fault == 1 ? 0xd300 : 0xd000, "fault vector");
        CMPX_EXPECT(cpu->cr[19], 0x1000, "fault restarts at first prefix");
        CMPX_EXPECT(cpu->cr[20], 0x4000, "fault reports operand address");
        CMPX_EXPECT(cpu->cr[17] & (UINT64_C(3) << 33),
                    UINT64_C(3) << 33, "fault reports read-modify-write access");
        CMPX_EXPECT(cpu->gr_static[8], old_acc, "fault preserves accumulator");
        CMPX_EXPECT(cpu->ar[24], upper | initial_flags, "fault preserves flags");
        CMPX_EXPECT(f.writes, 0, "fault prevents backing write");
        CMPX_EXPECT(f.reads, 0, "fault precedes backing read");
        CMPX_EXPECT(rse_test_read(&f.ram, 0x5000, size), dest, "fault preserves memory");
        cpu->dtr[0].pte = 0x5461;
        merced_flush_translation_cache(cpu);
        for (unsigned slot = 0; slot < 3; slot++)
            CMPX_EXPECT(handoff_step(cpu, fast), MERCED_OK, "native handler rfi");
        CMPX_EXPECT(cpu->ip, 0x1000, "rfi resumes original IA32 instruction");
        CMPX_EXPECT(handoff_step(cpu, fast), MERCED_OK, "repaired retry");
    }
    uint64_t expected_acc = sample ? (old_acc & ~(uint64_t)mask) | dest : old_acc;
    if (!sample && form == 2)
        expected_acc = (old_acc & ~UINT64_C(0xff00)) | ((uint64_t)src << 8);
    uint64_t expected_dest = !sample && (form == 1 || form == 3)
                             ? (old_dest & ~(uint64_t)mask) | src : old_dest;
    CMPX_EXPECT(cpu->ip, 0x1000 + len, "completed instruction advances EIP");
    CMPX_EXPECT(cpu->nfaults, fault != 0, "no extra fault on retry");
    CMPX_EXPECT(cpu->gr_static[8], expected_acc, "accumulator and upper bits");
    CMPX_EXPECT(cpu->gr_static[11], expected_dest, "register destination and upper bits");
    CMPX_EXPECT(cpu->ar[24], upper | final_flags, "all arithmetic flags and preserved flags");
    CMPX_EXPECT(f.writes, form == 0, "memory write occurs on match and mismatch");
    CMPX_EXPECT(f.reads, form == 0, "operand read occurs exactly once");
    CMPX_EXPECT(rse_test_read(&f.ram, 0x5000, size),
                form == 0 && !sample ? src : dest, "memory replacement or writeback");
#undef CMPX_EXPECT
    merced_destroy(cpu);
}

static void check_ia32_cmpxchg_cross_page(bool fast, unsigned size,
                                         bool equal, bool fault) {
    Ia32CmpxchgFixture f = {0};
    const uint32_t mask = size == 4 ? UINT32_MAX : UINT16_MAX;
    const uint32_t dest = UINT32_C(0x11223344) & mask;
    const uint32_t src = UINT32_C(0xaabbccdd) & mask;
    const uint32_t acc = equal ? dest : dest ^ 1;
    unsigned len = 0;
    f.ram.memory[0x1000 + len++] = 0xf0;
    if (size == 2) f.ram.memory[0x1000 + len++] = 0x66;
    f.ram.memory[0x1000 + len++] = 0x0f;
    f.ram.memory[0x1000 + len++] = 0xb1;
    f.ram.memory[0x1000 + len++] = 0x0a; /* [edx],ecx */
    rse_test_write(&f.ram, 0x5fff, dest & 0xff, 1);
    for (unsigned i = 1; i < size; i++)
        rse_test_write(&f.ram, 0x7000 + i - 1, (dest >> (8 * i)) & 0xff, 1);
    rse_test_write(&f.ram, 0x6000, UINT32_C(0xdeadbeef), 4);
    const uint64_t nop = UINT64_C(0x08000000);
    rse_test_bundle(&f.ram, 0xd300, 0x11, nop, nop, UINT64_C(8) << 27);
    MercedBus bus = {.ud = &f, .read = ia32_cmpxchg_read,
                     .write = ia32_cmpxchg_write, .ram_size = sizeof(f.ram.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 17) |
               (UINT64_C(1) << 34) | (UINT64_C(1) << 44);
    cpu->ar[24] = 0xed7;
    cpu->gr_static[25] = UINT64_C(1) << 62;
    cpu->gr_static[8] = acc;
    cpu->gr_static[9] = src;
    cpu->gr_static[10] = 0x4fff;
    cpu->cr[2] = 0x8000;
    cpu->cr[72] = UINT64_C(1) << 16;
    cpu->rr[0] = (1 << 8) | (12 << 2);
    for (unsigned i = 0; i < 2; i++) {
        uint64_t va = 0x4000 + 0x1000 * i, pa = 0x5000 + 0x2000 * i;
        cpu->dtr[i] = (MercedTlbEntry) {
            .va_start = va, .va_end = va + 0xfff, .pfn_base = pa,
            .rid = 1, .ps = 12, .valid = 1, .itir = (1 << 8) | (12 << 2),
            .pte = pa | (fault && i ? 0x61 : 0x461),
        };
    }
#define CROSS_EXPECT(actual, expected, what) do { \
    uint64_t a_ = (actual), e_ = (expected); \
    if (a_ != e_) { \
        fprintf(stderr, "IA32 cmpxchg cross-page %s size=%u equal=%u fault=%u: " \
                "%s: got 0x%" PRIx64 ", expected 0x%" PRIx64 "\n", \
                fast ? "fast" : "slow", size, equal, fault, \
                what, a_, e_); failures++; \
    } \
} while (0)
    CROSS_EXPECT(handoff_step(cpu, fast), MERCED_OK, "instruction status");
    if (fault) {
        CROSS_EXPECT(cpu->ip, 0xd300, "second page write-permission fault");
        CROSS_EXPECT(cpu->cr[20], 0x5000, "fault names second page");
        CROSS_EXPECT(cpu->cr[19], 0x1000, "fault retains original instruction");
        CROSS_EXPECT(cpu->cr[17] & (UINT64_C(3) << 33), UINT64_C(3) << 33,
                      "fault reports read-modify-write");
        CROSS_EXPECT(f.reads, 0, "preflight prevents all operand reads");
        CROSS_EXPECT(f.writes, 0, "preflight prevents partial operand writes");
        CROSS_EXPECT(cpu->gr_static[8], acc, "fault preserves accumulator");
        CROSS_EXPECT(cpu->ar[24], 0xed7, "fault preserves flags");
        cpu->dtr[1].pte = 0x7461;
        merced_flush_translation_cache(cpu);
        for (unsigned slot = 0; slot < 3; slot++)
            CROSS_EXPECT(handoff_step(cpu, fast), MERCED_OK, "native handler rfi");
        CROSS_EXPECT(cpu->ip, 0x1000, "rfi resumes cross-page operand");
        CROSS_EXPECT(handoff_step(cpu, fast), MERCED_OK, "cross-page retry");
    }
    uint32_t expected = equal ? src : dest;
    CROSS_EXPECT(cpu->ip, 0x1000 + len, "complete instruction");
    CROSS_EXPECT(cpu->nfaults, fault, "only requested fault");
    CROSS_EXPECT(cpu->gr_static[8], equal ? acc : dest, "accumulator result");
    CROSS_EXPECT(cpu->ar[24], equal ? 0x646 : 0x602, "comparison flags");
    CROSS_EXPECT(f.reads, size, "read each discontiguous byte once");
    CROSS_EXPECT(f.writes, size, "write each discontiguous byte once");
    CROSS_EXPECT(rse_test_read(&f.ram, 0x5fff, 1), expected & 0xff, "first page byte");
    for (unsigned i = 1; i < size; i++)
        CROSS_EXPECT(rse_test_read(&f.ram, 0x7000 + i - 1, 1),
                      (expected >> (8 * i)) & 0xff, "second page bytes");
    CROSS_EXPECT(rse_test_read(&f.ram, 0x6000, 4), UINT32_C(0xdeadbeef),
                  "physical gap is never touched");
#undef CROSS_EXPECT
    merced_destroy(cpu);
}

typedef struct {
    RseFixture ram;
    uint32_t addresses[4], values[4];
    unsigned size, reads;
} Ia32ScasFixture;

static void ia32_test_set_register(uint64_t *regs, unsigned reg,
                                    unsigned size, uint32_t value) {
    unsigned shift = size == 1 && reg >= 4 ? 8 : 0;
    unsigned index = shift ? reg - 4 : reg;
    uint64_t mask = (size == 4 ? UINT32_MAX : (1u << (size * 8)) - 1u);
    regs[index] = (regs[index] & ~(mask << shift)) | (((uint64_t)value & mask) << shift);
}

static uint64_t ia32_string_test_pa(uint32_t va) {
    if (va >= 0x6000) return va + 0x3000;
    if (va >= 0x5000) return va + 0x2000;
    return va + 0x1000;
}

/* Repeated transfers must retire CX alongside each successful element.
 * Otherwise a fault after one element retries the original count at the
 * advanced index, overwriting a guard byte or loading one element too many.
 * A split element additionally exercises discontiguous pages and precise
 * write-permission checks before any part of that element is stored.
 * kind: 0=MOVS, 1=LODS, 2=STOS. fault_mode: 0=source absent (MOVS/LODS),
 * 1=destination absent (MOVS/STOS), 2=destination read-only (MOVS/STOS). */
static void check_ia32_string_restart(bool fast, unsigned kind, unsigned size,
                                      bool address32, bool crossing,
                                      unsigned fault_mode) {
    Ia32CmpxchgFixture f = {0};
    memset(f.ram.memory + 0x5000, 0xcc, 0x5000);
    bool source_fault = fault_mode == 0;
    unsigned completed = crossing ? 0 : 1;
    uint32_t fault_start = crossing ? 0x5000 - size / 2 : 0x5000 - size;
    uint32_t source = kind == 2 ? 0x3456 : source_fault ? fault_start : 0x6008;
    uint32_t destination = kind == 1 ? 0x4567 : source_fault ? 0x6008 : fault_start;
    uint64_t mask = size == 4 ? UINT32_MAX : (UINT64_C(1) << (size * 8)) - 1;
    uint64_t amask = address32 ? UINT32_MAX : UINT16_MAX;
    uint64_t old_si = UINT64_C(0x1234567800000000) |
                      (address32 ? 0 : 0x13570000) | source;
    uint64_t old_di = UINT64_C(0x2345678900000000) |
                      (address32 ? 0 : 0x24680000) | destination;
    uint64_t old_cx = UINT64_C(0x3456789a00000000) |
                      (address32 ? 0 : 0x35790000) | 3;
    uint64_t old_ax = UINT64_C(0x456789ab89abcda5);
    uint64_t flags = UINT64_C(0x56789abc00000202);
    static const uint8_t opcodes[] = {0xa4, 0xac, 0xaa};
    static const uint32_t values[] = {0x12345611, 0x23456722, 0x34567833, 0x45678944};
    unsigned len = 0;
    f.ram.memory[0x1000 + len++] = 0xf3;
    if (size == 2) f.ram.memory[0x1000 + len++] = 0x66;
    if (!address32) f.ram.memory[0x1000 + len++] = 0x67;
    f.ram.memory[0x1000 + len++] = opcodes[kind] + (size != 1);
    if (kind != 2)
        for (unsigned element = 0; element < 4; element++)
            for (unsigned i = 0; i < size; i++)
                rse_test_write(&f.ram, ia32_string_test_pa(source + element * size + i),
                               (values[element] >> (8 * i)) & 0xff, 1);
    const uint64_t nop = UINT64_C(0x08000000);
    rse_test_bundle(&f.ram, 0xd000, 0x11, nop, nop, UINT64_C(8) << 27);
    rse_test_bundle(&f.ram, 0xd300, 0x11, nop, nop, UINT64_C(8) << 27);
    MercedBus bus = {.ud = &f, .read = ia32_cmpxchg_read,
                     .write = ia32_cmpxchg_write, .ram_size = sizeof(f.ram.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 17) |
               (UINT64_C(1) << 34) | (UINT64_C(1) << 44);
    cpu->gr_static[25] = UINT64_C(1) << 62;
    cpu->gr_static[24] = cpu->gr_static[27] = 0;
    cpu->gr_static[8] = old_ax;
    cpu->gr_static[9] = old_cx;
    cpu->gr_static[14] = old_si;
    cpu->gr_static[15] = old_di;
    cpu->ar[24] = flags;
    cpu->cr[2] = 0x8000;
    cpu->cr[72] = UINT64_C(1) << 16;
    cpu->rr[0] = (1 << 8) | (12 << 2);
    for (unsigned i = 0; i < 3; i++) {
        uint64_t va = 0x4000 + 0x1000 * i, pa = 0x5000 + 0x2000 * i;
        uint64_t pte = pa | 0x461;
        if (i == 1) pte &= ~(fault_mode == 2 ? UINT64_C(0x400) : UINT64_C(1));
        cpu->dtr[i] = (MercedTlbEntry) {
            .va_start = va, .va_end = va + 0xfff, .pfn_base = pa,
            .rid = 1, .ps = 12, .valid = 1, .itir = (1 << 8) | (12 << 2),
            .pte = pte,
        };
    }
#define STRING_EXPECT(actual, expected, what) do { \
    uint64_t a_ = (actual), e_ = (expected); \
    if (a_ != e_) { \
        fprintf(stderr, "IA32 string %s kind=%u size=%u address32=%u split=%u fault=%u: " \
                "%s: got 0x%" PRIx64 ", expected 0x%" PRIx64 "\n", \
                fast ? "fast" : "slow", kind, size, address32, crossing, fault_mode, \
                what, a_, e_); failures++; \
    } \
} while (0)
    uint64_t vector = fault_mode == 2 ? 0xd300 : 0xd000;
    STRING_EXPECT(handoff_step(cpu, fast), MERCED_OK, "fault delivery status");
    STRING_EXPECT(cpu->ip, vector, "fault on inaccessible second page");
    STRING_EXPECT(cpu->cr[19], 0x1000, "repeat restarts at original prefixes");
    STRING_EXPECT(cpu->cr[20], 0x5000, "faulting byte address");
    STRING_EXPECT(cpu->cr[17] & (UINT64_C(3) << 33),
                  UINT64_C(1) << (source_fault ? 34 : 33), "read/write fault metadata");
    STRING_EXPECT(cpu->gr_static[9], (old_cx & ~amask) | (3 - completed),
                  "remaining count commits each completed element");
    STRING_EXPECT(cpu->gr_static[14], (old_si & ~amask) |
                  (source + (kind != 2 ? completed * size : 0)), "source index on fault");
    STRING_EXPECT(cpu->gr_static[15], (old_di & ~amask) |
                  (destination + (kind != 1 ? completed * size : 0)), "destination index on fault");
    STRING_EXPECT(cpu->gr_static[8], kind == 1 && completed ?
                  (old_ax & ~mask) | (values[0] & mask) : old_ax,
                  "LODS accumulator retains only completed element");
    STRING_EXPECT(cpu->ar[24], flags, "transfer leaves flags intact");
    if (kind != 1)
        for (unsigned i = 0; i < 3 * size + 1; i++) {
            uint64_t expected = i < completed * size ?
                ((kind == 0 ? values[0] : old_ax) >> (8 * (i % size))) & 0xff : 0xcc;
            STRING_EXPECT(rse_test_read(&f.ram, ia32_string_test_pa(destination + i), 1),
                          expected, "fault preserves incomplete element and destination guard");
        }
    if (cpu->ip != vector) { merced_destroy(cpu); return; }
    cpu->dtr[1].pte |= 0x401;
    merced_flush_translation_cache(cpu);
    for (unsigned slot = 0; slot < 3; slot++)
        STRING_EXPECT(handoff_step(cpu, fast), MERCED_OK, "native RFI resumes transfer");
    STRING_EXPECT(cpu->ip, 0x1000, "retry keeps instruction prefixes");
    STRING_EXPECT(handoff_step(cpu, fast), MERCED_OK, "retry completes unread suffix");
    STRING_EXPECT(cpu->ip, 0x1000 + len, "completion advances instruction");
    STRING_EXPECT(cpu->nfaults, 1, "one expected fault");
    STRING_EXPECT(cpu->gr_static[9], old_cx & ~amask, "exact repeat count completed");
    STRING_EXPECT(cpu->gr_static[14], (old_si & ~amask) |
                  (source + (kind != 2 ? 3 * size : 0)), "source index has no extra iteration");
    STRING_EXPECT(cpu->gr_static[15], (old_di & ~amask) |
                  (destination + (kind != 1 ? 3 * size : 0)), "destination index has no extra iteration");
    STRING_EXPECT(cpu->gr_static[8], kind == 1 ? (old_ax & ~mask) | (values[2] & mask) : old_ax,
                  "LODS returns intended third element, not fourth");
    STRING_EXPECT(cpu->ar[24], flags, "retry preserves flags");
    if (kind != 1) {
        for (unsigned i = 0; i < 3 * size; i++) {
            uint64_t expected = ((kind == 0 ? values[i / size] : old_ax) >> (8 * (i % size))) & 0xff;
            STRING_EXPECT(rse_test_read(&f.ram, ia32_string_test_pa(destination + i), 1),
                          expected, "all intended destination bytes copied once");
        }
        STRING_EXPECT(rse_test_read(&f.ram, ia32_string_test_pa(destination - 1), 1),
                      0xcc, "leading destination guard preserved");
        STRING_EXPECT(rse_test_read(&f.ram, ia32_string_test_pa(destination + 3 * size), 1),
                      0xcc, "trailing destination guard prevents count overrun");
    }
    if (kind != 2)
        for (unsigned element = 0; element < 4; element++)
            for (unsigned i = 0; i < size; i++)
                STRING_EXPECT(rse_test_read(&f.ram, ia32_string_test_pa(source + element * size + i), 1),
                              (values[element] >> (8 * i)) & 0xff, "source memory unchanged");
    STRING_EXPECT(rse_test_read(&f.ram, 0x6000, 8), UINT64_C(0xcccccccccccccccc),
                  "split accesses use mapped page, not physical neighbor");
#undef STRING_EXPECT
    merced_destroy(cpu);
}

static void check_ia32_xadd(bool fast, unsigned size, unsigned form,
                            unsigned sample, unsigned scenario) {
    Ia32CmpxchgFixture f = {0};
    bool memory = form == 0 || form == 7;
    bool crossing = scenario == 1 || scenario == 3;
    bool fault = scenario >= 2;
    bool identical = form == 2 || form == 6;
    uint32_t address = crossing ? 0x4fff : 0x4000;
    uint32_t mask = size == 4 ? UINT32_MAX : (1u << (size * 8)) - 1;
    uint32_t sign = 1u << (size * 8 - 1);
    uint32_t dest = sample == 0 ? 1 : sample == 1 ? mask : sample == 2 ? sign - 1 : sign;
    uint32_t src = identical ? dest : sample == 0 ? 2 : sample == 3 ? sign : 1;
    if (form == 7) src = address & mask;
    uint32_t sum = (dest + src) & mask;
    static const uint8_t modrm[] = {0x08,0xcb,0xc9,0xcc,0xe0,0xc4,0xe4,0x00};
    unsigned src_reg = (modrm[form] >> 3) & 7, dst_reg = modrm[form] & 7;
    unsigned len = 0;
    if (memory) f.ram.memory[0x1000 + len++] = 0xf0;
    if (size == 2) f.ram.memory[0x1000 + len++] = 0x66;
    f.ram.memory[0x1000 + len++] = 0x0f;
    f.ram.memory[0x1000 + len++] = size == 1 ? 0xc0 : 0xc1;
    f.ram.memory[0x1000 + len++] = modrm[form];
    uint64_t initial[8], expected[8];
    for (unsigned r = 0; r < 8; r++) initial[r] = UINT64_C(0x1234567812345600) | r;
    if (memory) initial[0] = UINT64_C(0x8765432100000000) | address;
    else ia32_test_set_register(initial, dst_reg, size, dest);
    ia32_test_set_register(initial, src_reg, size, src);
    memcpy(expected, initial, sizeof(expected));
    ia32_test_set_register(expected, src_reg, size, dest);
    if (!memory) ia32_test_set_register(expected, dst_reg, size, sum);
    for (unsigned i = 0; i < size; i++) {
        unsigned off = (address & 0xfff) + i;
        uint64_t pa = off < 0x1000 ? 0x5000 + off : 0x7000 + off - 0x1000;
        rse_test_write(&f.ram, pa, (dest >> (8 * i)) & 0xff, 1);
    }
    const uint64_t nop = UINT64_C(0x08000000);
    rse_test_bundle(&f.ram, 0xd300, 0x11, nop, nop, UINT64_C(8) << 27);
    MercedBus bus = {.ud = &f, .read = ia32_cmpxchg_read,
                     .write = ia32_cmpxchg_write, .ram_size = sizeof(f.ram.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 17) |
               (UINT64_C(1) << 34) | (UINT64_C(1) << 44);
    cpu->gr_static[25] = UINT64_C(1) << 62;
    memcpy(cpu->gr_static + 8, initial, sizeof(initial));
    const uint64_t flags = UINT64_C(0x1234567800000ed7);
    cpu->ar[24] = flags;
    cpu->cr[2] = 0x8000;
    cpu->cr[72] = UINT64_C(1) << 16;
    cpu->rr[0] = (1 << 8) | (12 << 2);
    for (unsigned i = 0; i < 2; i++) {
        uint64_t va = 0x4000 + 0x1000 * i, pa = 0x5000 + 0x2000 * i;
        bool writable = !(scenario == 2 && i == 0) && !(scenario == 3 && i == 1);
        cpu->dtr[i] = (MercedTlbEntry) {
            .va_start = va, .va_end = va + 0xfff, .pfn_base = pa,
            .rid = 1, .ps = 12, .valid = 1, .itir = (1 << 8) | (12 << 2),
            .pte = pa | 0x61 | (writable ? 0x600 : 0),
        };
    }
#define XADD_EXPECT(actual, expected_, what) do { \
    uint64_t a_ = (actual), e_ = (expected_); \
    if (a_ != e_) { \
        fprintf(stderr, "IA32 xadd %s size=%u form=%u sample=%u scenario=%u: " \
                "%s: got 0x%" PRIx64 ", expected 0x%" PRIx64 "\n", \
                fast ? "fast" : "slow", size, form, sample, scenario, \
                what, a_, e_); failures++; \
    } \
} while (0)
    XADD_EXPECT(handoff_step(cpu, fast), MERCED_OK, "instruction status");
    if (fault) {
        XADD_EXPECT(cpu->ip, 0xd300, "RMW permission fault");
        XADD_EXPECT(cpu->cr[19], 0x1000, "fault starts at original prefix");
        XADD_EXPECT(cpu->cr[20], scenario == 3 ? 0x5000 : address, "faulting operand byte");
        XADD_EXPECT(cpu->cr[17] & (UINT64_C(3) << 33), UINT64_C(3) << 33, "RMW metadata");
        for (unsigned r = 0; r < 8; r++)
            XADD_EXPECT(cpu->gr_static[8 + r], initial[r], "fault preserves source/address registers");
        XADD_EXPECT(cpu->ar[24], flags, "fault preserves flags");
        XADD_EXPECT(f.reads, 0, "preflight before operand reads");
        XADD_EXPECT(f.writes, 0, "no partial write");
        cpu->dtr[scenario == 3].pte |= 0x600;
        merced_flush_translation_cache(cpu);
        for (unsigned slot = 0; slot < 3; slot++)
            XADD_EXPECT(handoff_step(cpu, fast), MERCED_OK, "native RFI retry");
        XADD_EXPECT(cpu->ip, 0x1000, "RFI resumes XADD");
        XADD_EXPECT(handoff_step(cpu, fast), MERCED_OK, "repaired operand");
    }
    XADD_EXPECT(cpu->ip, 0x1000 + len, "instruction completes");
    XADD_EXPECT(cpu->nfaults, fault, "fault count");
    for (unsigned r = 0; r < 8; r++)
        XADD_EXPECT(cpu->gr_static[8 + r], expected[r], "register exchange and alias ordering");
    static const unsigned plain_flags[] = {0x4,0x55,0x894,0x845};
    static const unsigned identical_flags[] = {0,0x91,0x890,0x845};
    unsigned result_flags = identical ? identical_flags[sample] : plain_flags[sample];
    if (!identical && sample == 2 && size == 1) result_flags = 0x890;
    if (form == 7) result_flags = crossing ? 0x14 : 0;
    XADD_EXPECT(cpu->ar[24], (flags & ~UINT64_C(0x8d5)) | result_flags, "ADD arithmetic flags");
    for (unsigned i = 0; i < size; i++) {
        unsigned off = (address & 0xfff) + i;
        uint64_t pa = off < 0x1000 ? 0x5000 + off : 0x7000 + off - 0x1000;
        XADD_EXPECT(rse_test_read(&f.ram, pa, 1), ((memory ? sum : dest) >> (8 * i)) & 0xff,
                      "memory updated at original effective address");
    }
    XADD_EXPECT(f.reads, memory ? (crossing ? size : 1) : 0, "operand read count");
    XADD_EXPECT(f.writes, memory ? (crossing ? size : 1) : 0, "operand write count");
#undef XADD_EXPECT
    merced_destroy(cpu);
}

static void check_ia32_xadd_locked_register(bool fast, unsigned size) {
    RseFixture f = {0};
    unsigned len = 0;
    f.memory[0x1000 + len++] = 0xf0;
    if (size == 2) f.memory[0x1000 + len++] = 0x66;
    f.memory[0x1000 + len++] = 0x0f;
    f.memory[0x1000 + len++] = size == 1 ? 0xc0 : 0xc1;
    f.memory[0x1000 + len++] = 0xcb;
    MercedBus bus = {.ud = &f, .read = rse_test_read,
                     .write = rse_test_write, .ram_size = sizeof(f.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 34) | (UINT64_C(1) << 44);
    cpu->gr_static[25] = UINT64_C(1) << 62;
    cpu->gr_static[9] = 3;
    cpu->gr_static[11] = 4;
    cpu->ar[24] = 0xed7;
    cpu->cr[2] = 0x8000;
    cpu->cr[72] = UINT64_C(1) << 16;
#define XADD_BAD_EXPECT(actual, expected, what) \
    rse_test_expect((actual), (expected), (what), fast, size, false, false, false)
    XADD_BAD_EXPECT(handoff_step(cpu, fast), MERCED_OK, "invalid LOCK XADD intercept");
    XADD_BAD_EXPECT(cpu->ip, 0xea00, "LOCK register form is invalid");
    XADD_BAD_EXPECT(cpu->cr[19], 0x1000, "intercept restarts prefixes");
    XADD_BAD_EXPECT(cpu->cr[24], size == 1 ? 0xcbc00f : 0xcbc10f, "intercept opcode bytes");
    XADD_BAD_EXPECT(cpu->cr[17] & 0xffff, size == 2 ? 0x200c : 0x100e, "LOCK/size prefix metadata");
    XADD_BAD_EXPECT(cpu->gr_static[9], 3, "invalid form preserves source");
    XADD_BAD_EXPECT(cpu->gr_static[11], 4, "invalid form preserves destination");
    XADD_BAD_EXPECT(cpu->ar[24], 0xed7, "invalid form preserves flags");
#undef XADD_BAD_EXPECT
    merced_destroy(cpu);
}

static uint64_t ia32_scas_read(void *ud, uint64_t addr, unsigned size) {
    Ia32ScasFixture *f = ud;
    for (unsigned i = 0; i < 4; i++)
        if (addr == f->addresses[i] && size == f->size) {
            f->reads++;
            return f->values[i];
        }
    return rse_test_read(&f->ram, addr, size);
}

/* Generic MOV and arithmetic operands share the split-page translator with
 * string operands. kind: load, store, relocation ADD, carry-sensitive ADC.
 * fault: none, absent second page, read-only second page (writes only). */
static void check_ia32_split_operand(bool fast, unsigned size, unsigned first,
                                     unsigned kind, unsigned fault) {
    Ia32CmpxchgFixture f = {0};
    uint32_t address = 0x5000 - first;
    uint64_t mask = size == 4 ? UINT32_MAX : UINT16_MAX;
    uint32_t initial = kind == 2 ? (size == 4 ? UINT32_C(0x7da39f62) : 0x9f62) : 1;
    uint32_t source = kind == 2 ? (size == 4 ? UINT32_C(0x002b0000) - UINT32_C(0x7da20000) : 0x1200) : 0;
    uint64_t old_ax = UINT64_C(0x1234567889abcdef);
    uint64_t flags = UINT64_C(0x2345678900000ad7); /* ADC must retain entry CF=1 on fault. */
    uint32_t expected = kind == 0 ? initial : kind == 1 ? old_ax & mask :
                        kind == 2 ? (initial + source) & mask : 2;
    unsigned len = 0;
    if (size == 2) f.ram.memory[0x1000 + len++] = 0x66;
    static const uint8_t opcodes[] = {0x8b, 0x89, 0x01, 0x11};
    f.ram.memory[0x1000 + len++] = opcodes[kind];
    f.ram.memory[0x1000 + len++] = kind < 2 ? 0x07 : 0x1f;
    for (unsigned i = 0; i < size; i++)
        rse_test_write(&f.ram, ia32_string_test_pa(address + i), initial >> (8 * i), 1);
    rse_test_write(&f.ram, 0x6000, UINT64_C(0xcccccccccccccccc), 8);
    const uint64_t nop = UINT64_C(0x08000000);
    rse_test_bundle(&f.ram, 0xd000, 0x11, nop, nop, UINT64_C(8) << 27);
    rse_test_bundle(&f.ram, 0xd300, 0x11, nop, nop, UINT64_C(8) << 27);
    MercedBus bus = {.ud = &f, .read = ia32_cmpxchg_read,
                     .write = ia32_cmpxchg_write, .ram_size = sizeof(f.ram.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 17) |
               (UINT64_C(1) << 34) | (UINT64_C(1) << 44);
    cpu->gr_static[25] = UINT64_C(1) << 62;
    cpu->gr_static[8] = old_ax;
    cpu->gr_static[11] = UINT64_C(0x3456789a00000000) | source;
    cpu->gr_static[15] = address;
    cpu->ar[24] = flags;
    cpu->cr[2] = 0x8000;
    cpu->cr[72] = UINT64_C(1) << 16;
    cpu->rr[0] = (1 << 8) | (12 << 2);
    for (unsigned i = 0; i < 2; i++) {
        uint64_t va = 0x4000 + 0x1000 * i, pa = 0x5000 + 0x2000 * i;
        uint64_t pte = pa | (kind ? 0x461 : 0x61);
        if (i && fault == 1) pte &= ~UINT64_C(1);
        if (i && fault == 2) pte &= ~UINT64_C(0x400);
        cpu->dtr[i] = (MercedTlbEntry) {
            .va_start = va, .va_end = va + 0xfff, .pfn_base = pa,
            .rid = 1, .ps = 12, .valid = 1, .itir = (1 << 8) | (12 << 2), .pte = pte,
        };
    }
#define OPERAND_EXPECT(actual, wanted, what) do { \
    uint64_t a_ = (actual), e_ = (wanted); \
    if (a_ != e_) { \
        fprintf(stderr, "IA32 split %s size=%u first=%u kind=%u fault=%u: " \
                "%s: got 0x%" PRIx64 ", expected 0x%" PRIx64 "\n", \
                fast ? "fast" : "slow", size, first, kind, fault, what, a_, e_); failures++; \
    } \
} while (0)
    OPERAND_EXPECT(handoff_step(cpu, fast), MERCED_OK, "instruction status");
    if (fault) {
        uint64_t vector = fault == 1 ? 0xd000 : 0xd300;
        OPERAND_EXPECT(cpu->ip, vector, "second-page fault vector");
        OPERAND_EXPECT(cpu->cr[19], 0x1000, "fault restarts original prefixes");
        OPERAND_EXPECT(cpu->cr[20], 0x5000, "fault identifies second page");
        OPERAND_EXPECT(cpu->gr_static[8], old_ax, "load destination unchanged on fault");
        OPERAND_EXPECT(cpu->ar[24], flags, "fault restores original arithmetic flags and carry");
        OPERAND_EXPECT(f.writes, 0, "no partial store before second-page permission check");
        for (unsigned i = 0; i < size; i++)
            OPERAND_EXPECT(rse_test_read(&f.ram, ia32_string_test_pa(address + i), 1),
                            (initial >> (8 * i)) & 0xff, "incomplete operand unchanged");
        if (cpu->ip != vector) { merced_destroy(cpu); return; }
        cpu->dtr[1].pte |= kind ? 0x401 : 1;
        merced_flush_translation_cache(cpu);
        for (unsigned slot = 0; slot < 3; slot++)
            OPERAND_EXPECT(handoff_step(cpu, fast), MERCED_OK, "native RFI resumes operand");
        OPERAND_EXPECT(cpu->ip, 0x1000, "retry retains opcode and prefixes");
        OPERAND_EXPECT(handoff_step(cpu, fast), MERCED_OK, "retry completes full operand");
    }
    OPERAND_EXPECT(cpu->ip, 0x1000 + len, "complete instruction length");
    OPERAND_EXPECT(cpu->nfaults, !!fault, "only expected translation fault");
    OPERAND_EXPECT(cpu->gr_static[8], kind == 0 ? (old_ax & ~mask) | initial : old_ax,
                    "correct load and preserved arithmetic registers");
    OPERAND_EXPECT(cpu->gr_static[15], address, "address register unchanged");
    for (unsigned i = 0; i < size; i++)
        OPERAND_EXPECT(rse_test_read(&f.ram, ia32_string_test_pa(address + i), 1),
                        ((kind ? expected : initial) >> (8 * i)) & 0xff,
                        "logical operand spans mapped physical pages");
    OPERAND_EXPECT(rse_test_read(&f.ram, 0x6000, 8), UINT64_C(0xcccccccccccccccc),
                    "physical neighbor is never mistaken for second mapped page");
    if (kind < 2) OPERAND_EXPECT(cpu->ar[24], flags, "MOV leaves flags unchanged");
    if (kind == 3) OPERAND_EXPECT(cpu->ar[24], flags & ~UINT64_C(0x8d5),
                                   "ADC retry uses original carry and updates flags once");
#undef OPERAND_EXPECT
    merced_destroy(cpu);
}

typedef struct {
    RseFixture ram;
    uint32_t addresses[8], values[8];
    unsigned size, reads, writes;
} Ia32CmpsFixture;

static uint64_t ia32_cmps_read(void *ud, uint64_t addr, unsigned size) {
    Ia32CmpsFixture *f = ud;
    for (unsigned i = 0; i < 8; i++)
        if (addr == f->addresses[i] && size == f->size) {
            f->reads++;
            return f->values[i];
        }
    return rse_test_read(&f->ram, addr, size);
}

static void ia32_cmps_write(void *ud, uint64_t addr, uint64_t value, unsigned size) {
    Ia32CmpsFixture *f = ud;
    f->writes++;
    rse_test_write(&f->ram, addr, value, size);
}

/* mod=00/SIB base=5 omits the base register, but keeps the scaled index.
 * The ntdll memcpy jump table exposed the old decoder replacing that index
 * with disp32, branching into an instruction-tail value of 0x907da883. */
static void check_ia32_sib_no_base(bool fast, bool code32, unsigned index,
                                  unsigned scale, bool wrapped, bool override,
                                  bool lea) {
    Ia32CmpsFixture f = {0};
    uint64_t regs[8];
    for (unsigned i = 0; i < 8; i++)
        regs[i] = UINT64_C(0x1234567800000000) |
                  (wrapped ? UINT32_C(0x40000002) : 0x100 + i * 8);
    uint32_t displacement = wrapped ? UINT32_C(0xfffffffc) : 0x4000;
    uint32_t offset = displacement + (index == 4 ? 0 : (uint32_t)regs[index] << scale);
    uint32_t address = (override ? 0x5000u : 0x2000u) + offset;
    unsigned size = code32 ? 4 : 2;
    unsigned len = 0;
    if (!code32) f.ram.memory[0x1000 + len++] = 0x67; /* Address32, operand16. */
    if (override) f.ram.memory[0x1000 + len++] = 0x64;
    f.ram.memory[0x1000 + len++] = lea ? 0x8d : 0x8b;
    f.ram.memory[0x1000 + len++] = 0x04;
    f.ram.memory[0x1000 + len++] = (scale << 6) | (index << 3) | 5;
    for (unsigned i = 0; i < 4; i++)
        f.ram.memory[0x1000 + len++] = displacement >> (8 * i);
    f.size = size;
    f.addresses[0] = address;
    f.values[0] = size == 4 ? UINT32_C(0x89abcdef) : 0xcdef;
    MercedBus bus = {.ud = &f, .read = ia32_cmps_read, .fetch = rse_test_read,
                     .write = ia32_cmps_write, .ram_size = sizeof(f.ram.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 34) | (UINT64_C(1) << 44);
    cpu->gr_static[25] = (uint64_t)code32 << 62;
    cpu->gr_static[26] = 0x8000; /* EBP index must not select SS. */
    cpu->gr_static[27] = 0x2000;
    cpu->gr_static[28] = 0x5000;
    memcpy(cpu->gr_static + 8, regs, sizeof(regs));
    cpu->ar[24] = UINT64_C(0x2345678900000ed7);
    cpu->cr[72] = UINT64_C(1) << 16;
#define SIB_EXPECT(actual, expected, what) do { \
    uint64_t a_ = (actual), e_ = (expected); \
    if (a_ != e_) { \
        fprintf(stderr, "IA32 SIB %s CS.D=%u index=%u scale=%u wrap=%u FS=%u LEA=%u: " \
                "%s: got 0x%" PRIx64 ", expected 0x%" PRIx64 "\n", \
                fast ? "fast" : "slow", code32, index, scale, wrapped, override, lea, \
                what, a_, e_); failures++; \
    } \
} while (0)
    SIB_EXPECT(handoff_step(cpu, fast), MERCED_OK, "instruction status");
    SIB_EXPECT(cpu->ip, 0x1000 + len, "all four displacement bytes consumed");
    SIB_EXPECT(cpu->nfaults, 0, "no unexpected fault");
    uint64_t mask = size == 4 ? UINT32_MAX : UINT16_MAX;
    SIB_EXPECT(cpu->gr_static[8], (regs[0] & ~mask) |
                  ((lea ? offset : f.values[0]) & mask), "index plus displacement, width and wrap");
    for (unsigned i = 1; i < 8; i++)
        SIB_EXPECT(cpu->gr_static[8 + i], regs[i], "unrelated registers unchanged");
    SIB_EXPECT(cpu->ar[24], UINT64_C(0x2345678900000ed7), "flags unchanged");
    SIB_EXPECT(f.reads, lea ? 0 : 1, "correct effective address read once");
    SIB_EXPECT(f.writes, 0, "no memory write");
#undef SIB_EXPECT
    merced_destroy(cpu);
}

static void check_ia32_sib_ntdll_jump(bool fast) {
    Ia32CmpsFixture f = {0};
    /* Exact ntdll!memcpy instruction at 0x7da882dd; the no-base address
     * points into overlapping instruction bytes unless EAX*4 is added. */
    static const uint8_t instruction[] = {0xff, 0x24, 0x85, 0xf0, 0x82, 0xa8, 0x7d};
    memcpy(f.ram.memory + 0x1000, instruction, sizeof(instruction));
    f.size = 4;
    f.addresses[0] = UINT32_C(0x7da882f0);
    f.values[0] = UINT32_C(0x907da883);
    f.addresses[1] = UINT32_C(0x7da882f8);
    f.values[1] = UINT32_C(0x7da8832c);
    MercedBus bus = {.ud = &f, .read = ia32_cmps_read, .fetch = rse_test_read,
                     .write = ia32_cmps_write, .ram_size = sizeof(f.ram.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 34) | (UINT64_C(1) << 44);
    cpu->gr_static[25] = UINT64_C(1) << 62;
    cpu->gr_static[8] = 2;
    cpu->gr_static[12] = 0x2000;
    cpu->ar[24] = 0x206;
    cpu->cr[72] = UINT64_C(1) << 16;
#define SIB_JUMP_EXPECT(actual, expected, what) \
    rse_test_expect((actual), (expected), (what), fast, 4, false, false, false)
    SIB_JUMP_EXPECT(handoff_step(cpu, fast), MERCED_OK, "ntdll SIB jump status");
    SIB_JUMP_EXPECT(cpu->ip, UINT32_C(0x7da8832c), "scaled index selects valid memcpy branch");
    SIB_JUMP_EXPECT(cpu->gr_static[8], 2, "jump preserves index register");
    SIB_JUMP_EXPECT(cpu->gr_static[12], 0x2000, "jump preserves stack pointer");
    SIB_JUMP_EXPECT(cpu->ar[24], 0x206, "jump preserves flags");
    SIB_JUMP_EXPECT(f.reads, 1, "jump table read once");
    SIB_JUMP_EXPECT(f.writes, 0, "jump does not write memory");
#undef SIB_JUMP_EXPECT
    merced_destroy(cpu);
}

static void check_ia32_cmps(bool fast, bool code32, bool op_prefix,
                            bool byte, bool addr_prefix, bool backward,
                            unsigned repeat, unsigned scenario, unsigned segment) {
    Ia32CmpsFixture f = {0};
    unsigned size = byte ? 1 : code32 != op_prefix ? 4 : 2;
    unsigned asize = code32 != addr_prefix ? 4 : 2;
    uint32_t amask = asize == 4 ? UINT32_MAX : UINT16_MAX;
    uint32_t src = scenario == 3 ? 0 : 1u << (size * 8 - 1);
    unsigned count = scenario == 1 ? 2 : scenario == 2 ? 0 : 4;
    unsigned iterations = !repeat ? 1 : scenario == 1 ? 2 : scenario == 2 ? 0 : 3;
    uint32_t si = scenario == 3 ? (backward ? 0 : amask - size + 1) : 0x100;
    uint32_t di = segment == 2 ? 0x200 : si;
    int step = backward ? -(int)size : (int)size;
    uint64_t flags = UINT64_C(0x1122334400000a97) |
                      (backward ? 0x400 : 0) | (repeat == 2 ? 0x40 : 0);
    uint64_t old_si = UINT64_C(0x1234567800000000) | (asize == 2 ? 0x13570000 : 0) | si;
    uint64_t old_di = UINT64_C(0x2345678900000000) | (asize == 2 ? 0x24680000 : 0) | di;
    uint64_t old_cx = UINT64_C(0x3456789a00000000) | (asize == 2 ? 0x35790000 : 0) | count;
    uint64_t old_ax = UINT64_C(0x456789ab11223344);
    unsigned len = 0;
    if (segment) f.ram.memory[0x1000 + len++] = segment == 1 ? 0x64 : 0x26;
    if (repeat) f.ram.memory[0x1000 + len++] = repeat == 1 ? 0xf3 : 0xf2;
    if (op_prefix) f.ram.memory[0x1000 + len++] = 0x66;
    if (addr_prefix) f.ram.memory[0x1000 + len++] = 0x67;
    f.ram.memory[0x1000 + len++] = byte ? 0xa6 : 0xa7;
    uint32_t source_base = segment == 0 ? 0x2000 : segment == 1 ? 0x5000 : 0x9000;
    f.size = size;
    for (unsigned i = 0; i < 4; i++) {
        f.addresses[2 * i] = source_base + ((si + i * step) & amask);
        f.addresses[2 * i + 1] = 0x9000u + ((di + i * step) & amask);
        bool equal = repeat == 1 ? (scenario == 1 || i < 2) : repeat == 2 && i == 2;
        f.values[2 * i] = src;
        f.values[2 * i + 1] = equal ? src : 1;
    }
    MercedBus bus = {.ud = &f, .read = ia32_cmps_read, .fetch = rse_test_read,
                     .write = ia32_cmps_write, .ram_size = sizeof(f.ram.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 34) | (UINT64_C(1) << 44);
    cpu->gr_static[25] = (uint64_t)code32 << 62;
    cpu->gr_static[24] = 0x9000;
    cpu->gr_static[27] = 0x2000;
    cpu->gr_static[28] = 0x5000;
    cpu->gr_static[8] = old_ax;
    cpu->gr_static[14] = old_si;
    cpu->gr_static[15] = old_di;
    cpu->gr_static[9] = old_cx;
    cpu->ar[24] = flags;
    cpu->cr[72] = UINT64_C(1) << 16;
#define CMPS_EXPECT(actual, expected, what) do { \
    uint64_t a_ = (actual), e_ = (expected); \
    if (a_ != e_) { \
        fprintf(stderr, "IA32 cmps %s CS.D=%u 66=%u byte=%u 67=%u DF=%u rep=%u scenario=%u seg=%u: " \
                "%s: got 0x%" PRIx64 ", expected 0x%" PRIx64 "\n", \
                fast ? "fast" : "slow", code32, op_prefix, byte, addr_prefix, \
                backward, repeat, scenario, segment, what, a_, e_); failures++; \
    } \
} while (0)
    CMPS_EXPECT(handoff_step(cpu, fast), MERCED_OK, "instruction status");
    CMPS_EXPECT(cpu->ip, 0x1000 + len, "complete instruction");
    CMPS_EXPECT(cpu->nfaults, 0, "no unexpected fault");
    CMPS_EXPECT(f.reads, 2 * iterations, "exact source and destination reads");
    CMPS_EXPECT(f.writes, 0, "comparison does not write memory");
    CMPS_EXPECT(cpu->gr_static[14], (old_si & ~(uint64_t)amask) |
                  ((si + iterations * step) & amask), "source index direction, width and wrap");
    CMPS_EXPECT(cpu->gr_static[15], (old_di & ~(uint64_t)amask) |
                  ((di + iterations * step) & amask), "destination index direction, width and wrap");
    CMPS_EXPECT(cpu->gr_static[9], repeat ? (old_cx & ~(uint64_t)amask) |
                  (count - iterations) : old_cx, "count changes only with repetition");
    CMPS_EXPECT(cpu->gr_static[8], old_ax, "accumulator unchanged");
    uint64_t result_flags = flags;
    if (iterations) {
        bool equal = f.values[2 * iterations - 1] == src;
        uint64_t arithmetic = equal ? 0x44 : scenario == 3 ? 0x95 : size == 1 ? 0x810 : 0x814;
        result_flags = (flags & ~UINT64_C(0x8d5)) | arithmetic;
    }
    CMPS_EXPECT(cpu->ar[24], result_flags, "source-minus-destination flags; zero count preserves flags");
#undef CMPS_EXPECT
    merced_destroy(cpu);
}

/* Count both mapped data regions; instruction fetches use a separate callback. */
static uint64_t ia32_cmps_fault_read(void *ud, uint64_t addr, unsigned size) {
    Ia32CmpxchgFixture *f = ud;
    if (addr >= 0x5000 && addr < 0xa000) f->reads++;
    return rse_test_read(&f->ram, addr, size);
}

static void ia32_cmps_fault_write(void *ud, uint64_t addr, uint64_t value,
                                  unsigned size) {
    Ia32CmpxchgFixture *f = ud;
    if (addr >= 0x5000 && addr < 0xa000) f->writes++;
    rse_test_write(&f->ram, addr, value, size);
}

/* Both complete reads must precede flags, either index, and count updates.
 * On a destination fault the source has been read, but the pair is uncommitted.
 * The repaired retry deliberately stops after the second comparison, leaving
 * one element unscanned so a repeated/lost comparison changes the final count.
 * Every present data page is read-only, including the repaired page. */
static void check_ia32_cmps_fault(bool fast, unsigned size, bool address32,
                                  bool repe, bool crossing, bool source_fault) {
    Ia32CmpxchgFixture f = {0};
    memset(f.ram.memory + 0x5000, 0xcc, 0x5000);
    unsigned completed = crossing ? 0 : 1;
    uint32_t fault_start = crossing ? 0x5000 - size / 2 : 0x5000 - size;
    uint32_t source = source_fault ? fault_start : 0x6008;
    uint32_t destination = source_fault ? 0x6008 : fault_start;
    uint64_t amask = address32 ? UINT32_MAX : UINT16_MAX;
    uint64_t old_si = UINT64_C(0x1234567800000000) |
                      (address32 ? 0 : 0x13570000) | source;
    uint64_t old_di = UINT64_C(0x2345678900000000) |
                      (address32 ? 0 : 0x24680000) | destination;
    uint64_t old_cx = UINT64_C(0x3456789a00000000) |
                      (address32 ? 0 : 0x35790000) | 3;
    uint64_t old_ax = UINT64_C(0x456789ab89abcda5);
    uint64_t flags = UINT64_C(0x56789abc00000202) | (repe ? 0 : 0x40);
    unsigned len = 0;
    f.ram.memory[0x1000 + len++] = 0x64; /* FS overrides source, ES remains fixed. */
    f.ram.memory[0x1000 + len++] = repe ? 0xf3 : 0xf2;
    if (size == 2) f.ram.memory[0x1000 + len++] = 0x66;
    if (!address32) f.ram.memory[0x1000 + len++] = 0x67;
    f.ram.memory[0x1000 + len++] = size == 1 ? 0xa6 : 0xa7;
    for (unsigned element = 0; element < 3; element++) {
        uint32_t source_value = element == 2 ? 0x77 : 0;
        uint32_t destination_value = element == 2 ? 0x88 : repe ? element : !element;
        for (unsigned i = 0; i < size; i++) {
            rse_test_write(&f.ram, ia32_string_test_pa(source + element * size + i),
                           (source_value >> (8 * i)) & 0xff, 1);
            rse_test_write(&f.ram, ia32_string_test_pa(destination + element * size + i),
                           (destination_value >> (8 * i)) & 0xff, 1);
        }
    }
    const uint64_t nop = UINT64_C(0x08000000);
    rse_test_bundle(&f.ram, 0xd000, 0x11, nop, nop, UINT64_C(8) << 27);
    MercedBus bus = {.ud = &f, .read = ia32_cmps_fault_read,
                     .fetch = rse_test_read, .write = ia32_cmps_fault_write,
                     .ram_size = sizeof(f.ram.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 17) |
               (UINT64_C(1) << 34) | (UINT64_C(1) << 44);
    cpu->gr_static[25] = UINT64_C(1) << 62;
    cpu->gr_static[24] = cpu->gr_static[28] = 0; /* ES and FS bases */
    cpu->gr_static[27] = 0x10000; /* Unmapped DS must not replace override. */
    cpu->gr_static[8] = old_ax;
    cpu->gr_static[9] = old_cx;
    cpu->gr_static[14] = old_si;
    cpu->gr_static[15] = old_di;
    cpu->ar[24] = flags;
    cpu->cr[2] = 0x8000;
    cpu->cr[72] = UINT64_C(1) << 16;
    cpu->rr[0] = (1 << 8) | (12 << 2);
    for (unsigned i = 0; i < 3; i++) {
        uint64_t va = 0x4000 + 0x1000 * i, pa = 0x5000 + 0x2000 * i;
        cpu->dtr[i] = (MercedTlbEntry) {
            .va_start = va, .va_end = va + 0xfff, .pfn_base = pa,
            .rid = 1, .ps = 12, .valid = 1, .itir = (1 << 8) | (12 << 2),
            .pte = pa | 0x60 | (i != 1),
        };
    }
#define CMPS_FAULT_EXPECT(actual, expected, what) do { \
    uint64_t a_ = (actual), e_ = (expected); \
    if (a_ != e_) { \
        fprintf(stderr, "IA32 CMPS %s size=%u address32=%u repe=%u split=%u sourcefault=%u: " \
                "%s: got 0x%" PRIx64 ", expected 0x%" PRIx64 "\n", \
                fast ? "fast" : "slow", size, address32, repe, crossing, source_fault, \
                what, a_, e_); failures++; \
    } \
} while (0)
    CMPS_FAULT_EXPECT(handoff_step(cpu, fast), MERCED_OK, "fault delivery status");
    CMPS_FAULT_EXPECT(cpu->ip, 0xd000, "incomplete pair faults on absent page");
    CMPS_FAULT_EXPECT(cpu->cr[19], 0x1000, "fault preserves original prefixes");
    CMPS_FAULT_EXPECT(cpu->cr[20], 0x5000, "faulting byte address");
    CMPS_FAULT_EXPECT(cpu->cr[17] & (UINT64_C(3) << 33), UINT64_C(1) << 34,
                      "both operands require only read permission");
    CMPS_FAULT_EXPECT(cpu->gr_static[9], (old_cx & ~amask) | (3 - completed),
                      "only complete comparisons decrement count");
    CMPS_FAULT_EXPECT(cpu->gr_static[14], (old_si & ~amask) | (source + completed * size),
                      "source index does not half-commit before second read");
    CMPS_FAULT_EXPECT(cpu->gr_static[15], (old_di & ~amask) | (destination + completed * size),
                      "destination index retains complete comparisons only");
    CMPS_FAULT_EXPECT(cpu->gr_static[8], old_ax, "comparison preserves accumulator");
    CMPS_FAULT_EXPECT(cpu->ar[24], flags, "fault restores entry comparison flags");
    CMPS_FAULT_EXPECT(f.reads, 2 * completed + !source_fault,
                      "source read precedes destination fault; split operand is preflighted");
    CMPS_FAULT_EXPECT(f.writes, 0, "neither operand is written on fault");
    if (cpu->ip != 0xd000) { merced_destroy(cpu); return; }
    cpu->dtr[1].pte |= 1; /* Repair presence only: page remains read-only. */
    merced_flush_translation_cache(cpu);
    for (unsigned slot = 0; slot < 3; slot++)
        CMPS_FAULT_EXPECT(handoff_step(cpu, fast), MERCED_OK, "native RFI resumes CMPS");
    CMPS_FAULT_EXPECT(cpu->ip, 0x1000, "retry restarts original prefixed instruction");
    CMPS_FAULT_EXPECT(handoff_step(cpu, fast), MERCED_OK, "retry finishes remaining comparison");
    CMPS_FAULT_EXPECT(cpu->ip, 0x1000 + len, "repeat terminates at changed comparison result");
    CMPS_FAULT_EXPECT(cpu->nfaults, 1, "read-only operands cause no extra faults");
    CMPS_FAULT_EXPECT(cpu->gr_static[9], (old_cx & ~amask) | 1, "one element remains unscanned");
    CMPS_FAULT_EXPECT(cpu->gr_static[14], (old_si & ~amask) | (source + 2 * size),
                      "source index advances exactly two elements");
    CMPS_FAULT_EXPECT(cpu->gr_static[15], (old_di & ~amask) | (destination + 2 * size),
                      "destination index advances exactly two elements");
    CMPS_FAULT_EXPECT(cpu->gr_static[8], old_ax, "CMPS never changes accumulator");
    CMPS_FAULT_EXPECT(cpu->ar[24], (flags & ~UINT64_C(0x8d5)) | (repe ? 0x95 : 0x44),
                      "final source-minus-destination flags control termination");
    CMPS_FAULT_EXPECT(f.reads, (crossing ? size + 3 : 4) + !source_fault,
                      "only incomplete pair may reread its successful first operand");
    CMPS_FAULT_EXPECT(f.writes, 0, "read-only comparisons never write memory");
    CMPS_FAULT_EXPECT(rse_test_read(&f.ram, 0x6000, 8), UINT64_C(0xcccccccccccccccc),
                      "noncontiguous page neighbor remains untouched");
#undef CMPS_FAULT_EXPECT
    merced_destroy(cpu);
}

static void check_ia32_cmps_locked(bool fast, unsigned size, bool repeated) {
    Ia32CmpsFixture f = {0};
    unsigned len = 0;
    f.ram.memory[0x1000 + len++] = 0xf0;
    if (repeated) f.ram.memory[0x1000 + len++] = 0xf3;
    if (size == 2) f.ram.memory[0x1000 + len++] = 0x66;
    f.ram.memory[0x1000 + len++] = size == 1 ? 0xa6 : 0xa7;
    f.size = size;
    f.addresses[0] = 0x3000;
    f.addresses[1] = 0x4000;
    MercedBus bus = {.ud = &f, .read = ia32_cmps_read, .fetch = rse_test_read,
                     .write = ia32_cmps_write, .ram_size = sizeof(f.ram.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 34) | (UINT64_C(1) << 44);
    cpu->gr_static[25] = UINT64_C(1) << 62;
    cpu->gr_static[9] = 3;
    cpu->gr_static[14] = 0x3000;
    cpu->gr_static[15] = 0x4000;
    cpu->ar[24] = 0xed7;
    cpu->cr[2] = 0x8000;
    cpu->cr[72] = UINT64_C(1) << 16;
#define CMPS_BAD_EXPECT(actual, expected, what) \
    rse_test_expect((actual), (expected), (what), fast, size, repeated, false, false)
    CMPS_BAD_EXPECT(handoff_step(cpu, fast), MERCED_OK, "invalid LOCK CMPS intercept");
    CMPS_BAD_EXPECT(cpu->ip, 0xea00, "LOCK CMPS is invalid");
    CMPS_BAD_EXPECT(cpu->cr[19], 0x1000, "intercept restarts prefixes");
    CMPS_BAD_EXPECT(cpu->cr[24], size == 1 ? 0xa6 : 0xa7, "intercept opcode byte");
    CMPS_BAD_EXPECT(cpu->cr[17] & 0xffff, ((len - 1) << 12) |
                    (size == 2 ? 0xc : 0xe) | (repeated ? 0x10 : 0), "LOCK/REP/size prefix metadata");
    CMPS_BAD_EXPECT(cpu->gr_static[9], 3, "invalid form preserves count");
    CMPS_BAD_EXPECT(cpu->gr_static[14], 0x3000, "invalid form preserves source index");
    CMPS_BAD_EXPECT(cpu->gr_static[15], 0x4000, "invalid form preserves destination index");
    CMPS_BAD_EXPECT(cpu->ar[24], 0xed7, "invalid form preserves flags");
    CMPS_BAD_EXPECT(f.reads, 0, "invalid form does not read operands");
    CMPS_BAD_EXPECT(f.writes, 0, "invalid form does not write operands");
#undef CMPS_BAD_EXPECT
    merced_destroy(cpu);
}

static void check_ia32_scas(bool fast, bool code32, bool op_prefix,
                            bool byte, bool addr_prefix, bool backward,
                            unsigned repeat, unsigned scenario) {
    Ia32ScasFixture f = {0};
    unsigned size = byte ? 1 : code32 != op_prefix ? 4 : 2;
    unsigned asize = code32 != addr_prefix ? 4 : 2;
    uint32_t amask = asize == 4 ? UINT32_MAX : UINT16_MAX;
    uint32_t acc = 1u << (size * 8 - 1);
    unsigned count = scenario == 1 ? 2 : scenario == 2 ? 0 : 4;
    unsigned iterations = !repeat ? 1 : scenario == 1 ? 2 : scenario == 2 ? 0 : 3;
    uint32_t index = scenario == 3 ? (backward ? 0 : amask - size + 1) : 0x100;
    int step = backward ? -(int)size : (int)size;
    uint64_t flags = UINT64_C(0x1122334400000a97) |
                      (backward ? 0x400 : 0) | (repeat == 2 ? 0x40 : 0);
    uint64_t old_di = UINT64_C(0x1234567800000000) | (asize == 2 ? 0x24680000 : 0) | index;
    uint64_t old_cx = UINT64_C(0x2345678900000000) | (asize == 2 ? 0x35790000 : 0) | count;
    unsigned len = 0;
    f.ram.memory[0x1000 + len++] = 0x3e; /* Cannot override implicit ES. */
    if (repeat) f.ram.memory[0x1000 + len++] = repeat == 1 ? 0xf3 : 0xf2;
    if (op_prefix) f.ram.memory[0x1000 + len++] = 0x66;
    if (addr_prefix) f.ram.memory[0x1000 + len++] = 0x67;
    f.ram.memory[0x1000 + len++] = byte ? 0xae : 0xaf;
    f.size = size;
    for (unsigned i = 0; i < 4; i++) {
        uint32_t di = (index + i * step) & amask;
        f.addresses[i] = 0x9000u + di;
        bool equal = repeat == 1 ? (scenario == 1 || i < 2) : repeat == 2 && i == 2;
        f.values[i] = equal ? acc : 1;
    }
    MercedBus bus = {.ud = &f, .read = ia32_scas_read, .fetch = rse_test_read,
                     .write = rse_test_write, .ram_size = sizeof(f.ram.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 34) | (UINT64_C(1) << 44);
    cpu->gr_static[25] = (uint64_t)code32 << 62;
    cpu->gr_static[24] = 0x9000;
    cpu->gr_static[27] = 0x2000;
    cpu->gr_static[8] = UINT64_C(0x3456789a00000000) | acc;
    cpu->gr_static[15] = old_di;
    cpu->gr_static[9] = old_cx;
    cpu->ar[24] = flags;
    cpu->cr[72] = UINT64_C(1) << 16;
#define SCAS_EXPECT(actual, expected, what) do { \
    uint64_t a_ = (actual), e_ = (expected); \
    if (a_ != e_) { \
        fprintf(stderr, "IA32 scas %s CS.D=%u 66=%u byte=%u 67=%u DF=%u rep=%u scenario=%u: " \
                "%s: got 0x%" PRIx64 ", expected 0x%" PRIx64 "\n", \
                fast ? "fast" : "slow", code32, op_prefix, byte, addr_prefix, \
                backward, repeat, scenario, what, a_, e_); failures++; \
    } \
} while (0)
    SCAS_EXPECT(handoff_step(cpu, fast), MERCED_OK, "instruction status");
    SCAS_EXPECT(cpu->ip, 0x1000 + len, "complete instruction");
    SCAS_EXPECT(cpu->nfaults, 0, "no unexpected fault");
    SCAS_EXPECT(f.reads, iterations, "correct number of comparisons");
    SCAS_EXPECT(cpu->gr_static[15], (old_di & ~(uint64_t)amask) |
                  ((index + iterations * step) & amask), "index direction, width and wrap");
    SCAS_EXPECT(cpu->gr_static[9], repeat ? (old_cx & ~(uint64_t)amask) |
                  (count - iterations) : old_cx, "count changes only with repetition");
    SCAS_EXPECT(cpu->gr_static[8], UINT64_C(0x3456789a00000000) | acc,
                  "accumulator unchanged");
    uint64_t result_flags = flags;
    if (iterations) {
        bool equal = f.values[iterations - 1] == acc;
        result_flags = (flags & ~UINT64_C(0x8d5)) | (equal ? 0x44 : size == 1 ? 0x810 : 0x814);
    }
    SCAS_EXPECT(cpu->ar[24], result_flags, "accumulator-minus-memory flags; zero count preserves flags");
#undef SCAS_EXPECT
    merced_destroy(cpu);
}

static void check_ia32_scas_fault(bool fast, unsigned size, bool address32,
                                  bool repe, bool crossing) {
    Ia32CmpxchgFixture f = {0};
    uint32_t address = crossing ? 0x4fff : 0x5000 - size;
    uint32_t completed = crossing ? 0 : 1;
    uint64_t old_di = UINT64_C(0x1234567800000000) | (address32 ? 0 : 0x24680000) | address;
    uint64_t old_cx = UINT64_C(0x2345678900000000) | (address32 ? 0 : 0x35790000) | 3;
    uint64_t flags = UINT64_C(0x3456789a00000202) | (repe ? 0 : 0x40);
    uint32_t amask = address32 ? UINT32_MAX : UINT16_MAX;
    unsigned len = 0;
    f.ram.memory[0x1000 + len++] = 0x3e;
    f.ram.memory[0x1000 + len++] = repe ? 0xf3 : 0xf2;
    if (size == 2) f.ram.memory[0x1000 + len++] = 0x66;
    if (!address32) f.ram.memory[0x1000 + len++] = 0x67;
    f.ram.memory[0x1000 + len++] = size == 1 ? 0xae : 0xaf;
    for (unsigned element = 0; element < 2; element++) {
        uint32_t value = repe ? element : !element;
        for (unsigned i = 0; i < size; i++) {
            uint32_t va = address + element * size + i;
            uint64_t pa = va < 0x5000 ? va + 0x1000 : va + 0x2000;
            rse_test_write(&f.ram, pa, (value >> (8 * i)) & 0xff, 1);
        }
    }
    const uint64_t nop = UINT64_C(0x08000000);
    rse_test_bundle(&f.ram, 0xd000, 0x11, nop, nop, UINT64_C(8) << 27);
    MercedBus bus = {.ud = &f, .read = ia32_cmpxchg_read,
                     .write = ia32_cmpxchg_write, .ram_size = sizeof(f.ram.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 17) |
               (UINT64_C(1) << 34) | (UINT64_C(1) << 44);
    cpu->gr_static[25] = UINT64_C(1) << 62;
    cpu->gr_static[24] = 0;
    cpu->gr_static[27] = 0x9000;
    cpu->gr_static[8] = UINT64_C(0x456789ab00000000);
    cpu->gr_static[15] = old_di;
    cpu->gr_static[9] = old_cx;
    cpu->ar[24] = flags;
    cpu->cr[2] = 0x8000;
    cpu->cr[72] = UINT64_C(1) << 16;
    cpu->rr[0] = (1 << 8) | (12 << 2);
    for (unsigned i = 0; i < 2; i++) {
        uint64_t va = 0x4000 + 0x1000 * i, pa = 0x5000 + 0x2000 * i;
        cpu->dtr[i] = (MercedTlbEntry) {
            .va_start = va, .va_end = va + 0xfff, .pfn_base = pa,
            .rid = 1, .ps = 12, .valid = 1, .itir = (1 << 8) | (12 << 2),
            .pte = pa | 0x60 | !i,
        };
    }
#define SCAS_FAULT_EXPECT(actual, expected, what) \
    rse_test_expect((actual), (expected), (what), fast, size, address32, repe, crossing)
    SCAS_FAULT_EXPECT(handoff_step(cpu, fast), MERCED_OK, "SCAS fault status");
    SCAS_FAULT_EXPECT(cpu->ip, 0xd000, "SCAS page-not-present fault");
    SCAS_FAULT_EXPECT(cpu->cr[19], 0x1000, "repeat resumes at original prefixes");
    SCAS_FAULT_EXPECT(cpu->cr[20], 0x5000, "fault reports unreadable element page");
    SCAS_FAULT_EXPECT(cpu->cr[17] & (UINT64_C(3) << 33), UINT64_C(1) << 34, "read-only access metadata");
    SCAS_FAULT_EXPECT(cpu->gr_static[15], (old_di & ~(uint64_t)amask) |
                        (address + completed * size), "only completed index increments survive");
    SCAS_FAULT_EXPECT(cpu->gr_static[9], (old_cx & ~(uint64_t)amask) | (3 - completed),
                        "remaining repeat count committed before fault");
    SCAS_FAULT_EXPECT(cpu->ar[24], flags, "fault restores entry comparison flags");
    SCAS_FAULT_EXPECT(f.reads, completed, "incomplete element does not partially read");
    cpu->dtr[1].pte |= 1;
    merced_flush_translation_cache(cpu);
    for (unsigned slot = 0; slot < 3; slot++)
        SCAS_FAULT_EXPECT(handoff_step(cpu, fast), MERCED_OK, "native RFI resumes repeat");
    SCAS_FAULT_EXPECT(cpu->ip, 0x1000, "repeat retry at original instruction");
    SCAS_FAULT_EXPECT(handoff_step(cpu, fast), MERCED_OK, "repeat reads remaining suffix");
    SCAS_FAULT_EXPECT(cpu->ip, 0x1000 + len, "repeat completes after repaired fault");
    SCAS_FAULT_EXPECT(cpu->nfaults, 1, "only expected fault");
    SCAS_FAULT_EXPECT(cpu->gr_static[15], (old_di & ~(uint64_t)amask) | (address + 2 * size),
                        "no repeated or lost element");
    SCAS_FAULT_EXPECT(cpu->gr_static[9], (old_cx & ~(uint64_t)amask) | 1, "termination leaves correct suffix count");
    SCAS_FAULT_EXPECT(cpu->ar[24], (flags & ~UINT64_C(0x8d5)) | (repe ? 0x95 : 0x44),
                        "final comparison controls flags");
    SCAS_FAULT_EXPECT(f.reads, crossing ? size + 1 : 2, "completed elements are not reread");
    SCAS_FAULT_EXPECT(f.writes, 0, "SCAS does not write operands");
#undef SCAS_FAULT_EXPECT
    merced_destroy(cpu);
}

static void check_ia32_cmpxchg8b(bool fast, bool code32, bool op_prefix,
                                bool addr_prefix, bool locked,
                                unsigned sample, unsigned scenario) {
    Ia32CmpxchgFixture f = {0};
    bool address32 = code32 != addr_prefix;
    bool crossing = scenario == 1 || scenario == 3;
    bool fault = scenario >= 2;
    uint32_t address = crossing ? 0x4ffc : 0x4000;
    const uint64_t acc = UINT64_C(0x8877665544332211);
    const uint64_t src = UINT64_C(0xffeeddccbbaa0099);
    uint64_t dest = acc ^ (sample == 1 ? 1 : sample == 2 ? UINT64_C(1) << 40 : 0);
    const uint64_t flags = UINT64_C(0x1122334400000e97) | (sample ? 0x40 : 0);
    unsigned len = 0;
    if (locked) f.ram.memory[0x1000 + len++] = 0xf0;
    if (op_prefix) f.ram.memory[0x1000 + len++] = 0x66;
    if (addr_prefix) f.ram.memory[0x1000 + len++] = 0x67;
    f.ram.memory[0x1000 + len++] = 0x0f;
    f.ram.memory[0x1000 + len++] = 0xc7;
    f.ram.memory[0x1000 + len++] = address32 ? 0x4d : 0x4e; /* [EBP/BP+0] */
    f.ram.memory[0x1000 + len++] = 0;
    for (unsigned i = 0; i < 8; i++) {
        unsigned off = (address & 0xfff) + i;
        uint64_t pa = off < 0x1000 ? 0x5000 + off : 0x7000 + off - 0x1000;
        rse_test_write(&f.ram, pa, (dest >> (8 * i)) & 0xff, 1);
    }
    rse_test_write(&f.ram, 0x6000, UINT64_C(0xdeadbeef12345678), 8);
    const uint64_t nop = UINT64_C(0x08000000);
    rse_test_bundle(&f.ram, 0xd300, 0x11, nop, nop, UINT64_C(8) << 27);
    MercedBus bus = {.ud = &f, .read = ia32_cmpxchg_read,
                     .write = ia32_cmpxchg_write, .ram_size = sizeof(f.ram.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 17) |
               (UINT64_C(1) << 34) | (UINT64_C(1) << 44);
    cpu->ar[24] = flags;
    cpu->gr_static[25] = (uint64_t)code32 << 62;
    cpu->gr_static[26] = 0x1000;
    cpu->gr_static[27] = 0xa000;
    cpu->gr_static[13] = (address32 ? 0 : UINT64_C(0x12340000)) | (address - 0x1000);
    cpu->gr_static[8] = UINT64_C(0x1234567800000000) | (uint32_t)acc;
    cpu->gr_static[10] = UINT64_C(0x2345678900000000) | (acc >> 32);
    cpu->gr_static[9] = UINT64_C(0x3456789a00000000) | (src >> 32);
    cpu->gr_static[11] = UINT64_C(0x456789ab00000000) | (uint32_t)src;
    cpu->cr[2] = 0x8000;
    cpu->cr[72] = UINT64_C(1) << 16;
    cpu->rr[0] = (1 << 8) | (12 << 2);
    for (unsigned i = 0; i < 2; i++) {
        uint64_t va = 0x4000 + 0x1000 * i, pa = 0x5000 + 0x2000 * i;
        bool writable = !(scenario == 2 && i == 0) && !(scenario == 3 && i == 1);
        cpu->dtr[i] = (MercedTlbEntry) {
            .va_start = va, .va_end = va + 0xfff, .pfn_base = pa,
            .rid = 1, .ps = 12, .valid = 1, .itir = (1 << 8) | (12 << 2),
            .pte = pa | 0x61 | (writable ? 0x600 : 0),
        };
    }
#define C8_EXPECT(actual, expected, what) do { \
    uint64_t a_ = (actual), e_ = (expected); \
    if (a_ != e_) { \
        fprintf(stderr, "IA32 cmpxchg8b %s CS.D=%u 66=%u 67=%u lock=%u sample=%u scenario=%u: " \
                "%s: got 0x%" PRIx64 ", expected 0x%" PRIx64 "\n", \
                fast ? "fast" : "slow", code32, op_prefix, addr_prefix, locked, \
                sample, scenario, what, a_, e_); failures++; \
    } \
} while (0)
    C8_EXPECT(handoff_step(cpu, fast), MERCED_OK, "instruction status");
    if (fault) {
        C8_EXPECT(cpu->ip, 0xd300, "read-only operand faults even on mismatch");
        C8_EXPECT(cpu->cr[19], 0x1000, "fault restarts instruction including prefixes");
        C8_EXPECT(cpu->cr[20], scenario == 3 ? 0x5000 : address, "faulting data page");
        C8_EXPECT(cpu->cr[17] & (UINT64_C(3) << 33), UINT64_C(3) << 33,
                    "read-modify-write access metadata");
        C8_EXPECT(cpu->gr_static[8], UINT64_C(0x1234567800000000) | (uint32_t)acc,
                    "EAX untouched on fault");
        C8_EXPECT(cpu->gr_static[10], UINT64_C(0x2345678900000000) | (acc >> 32),
                    "EDX untouched on fault");
        C8_EXPECT(cpu->ar[24], flags, "flags untouched on fault");
        C8_EXPECT(f.reads, 0, "all operand bytes checked before reading");
        C8_EXPECT(f.writes, 0, "no partial operand write");
        cpu->dtr[scenario == 3].pte |= 0x600;
        merced_flush_translation_cache(cpu);
        for (unsigned slot = 0; slot < 3; slot++)
            C8_EXPECT(handoff_step(cpu, fast), MERCED_OK, "native RFI retry");
        C8_EXPECT(cpu->ip, 0x1000, "RFI resumes locked instruction");
        C8_EXPECT(handoff_step(cpu, fast), MERCED_OK, "repaired instruction");
    }
    C8_EXPECT(cpu->ip, 0x1000 + len, "instruction completes");
    C8_EXPECT(cpu->nfaults, fault, "exact fault count");
    C8_EXPECT(cpu->ar[24], (flags & ~UINT64_C(0x40)) | (sample ? 0 : 0x40),
                "only ZF changes");
    C8_EXPECT(cpu->gr_static[8], UINT64_C(0x1234567800000000) | (uint32_t)dest,
                "low comparison register result");
    C8_EXPECT(cpu->gr_static[10], UINT64_C(0x2345678900000000) | (dest >> 32),
                "high comparison register result");
    C8_EXPECT(cpu->gr_static[9], UINT64_C(0x3456789a00000000) | (src >> 32), "ECX preserved");
    C8_EXPECT(cpu->gr_static[11], UINT64_C(0x456789ab00000000) | (uint32_t)src, "EBX preserved");
    uint64_t expected = sample ? dest : src;
    for (unsigned i = 0; i < 8; i++) {
        unsigned off = (address & 0xfff) + i;
        uint64_t pa = off < 0x1000 ? 0x5000 + off : 0x7000 + off - 0x1000;
        C8_EXPECT(rse_test_read(&f.ram, pa, 1), (expected >> (8 * i)) & 0xff,
                    "all eight destination bytes");
    }
    C8_EXPECT(rse_test_read(&f.ram, 0x6000, 8), UINT64_C(0xdeadbeef12345678),
                "gap between physical pages untouched");
    C8_EXPECT(f.reads, crossing ? 8 : 1, "operand read count");
    C8_EXPECT(f.writes, crossing ? 8 : 1, "both outcomes write memory");
#undef C8_EXPECT
    merced_destroy(cpu);
}

static void check_ia32_cmpxchg8b_invalid(bool fast, bool locked,
                                        bool op_prefix, bool addr_prefix) {
    RseFixture f = {0};
    unsigned len = 0;
    if (locked) f.memory[0x1000 + len++] = 0xf0;
    if (op_prefix) f.memory[0x1000 + len++] = 0x66;
    if (addr_prefix) f.memory[0x1000 + len++] = 0x67;
    unsigned prefixes = len;
    f.memory[0x1000 + len++] = 0x0f;
    f.memory[0x1000 + len++] = 0xc7;
    f.memory[0x1000 + len++] = 0xc8; /* Invalid register operand, /1. */
    MercedBus bus = {.ud = &f, .read = rse_test_read,
                     .write = rse_test_write, .ram_size = sizeof(f.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 34) | (UINT64_C(1) << 44);
    cpu->gr_static[25] = UINT64_C(1) << 62;
    cpu->gr_static[8] = UINT64_C(0x123456789abcdef0);
    cpu->ar[24] = 0xed7;
    cpu->cr[2] = 0x8000;
    cpu->cr[72] = UINT64_C(1) << 16;
#define C8_BAD_EXPECT(actual, expected, what) \
    rse_test_expect((actual), (expected), (what), fast, locked, op_prefix, addr_prefix, false)
    C8_BAD_EXPECT(handoff_step(cpu, fast), MERCED_OK, "invalid CMPXCHG8B intercept status");
    C8_BAD_EXPECT(cpu->ip, 0xea00, "invalid register form is instruction intercept");
    C8_BAD_EXPECT(cpu->cr[19], 0x1000, "intercept starts at first prefix");
    C8_BAD_EXPECT(cpu->cr[24], 0xc8c70f, "intercept instruction bytes");
    C8_BAD_EXPECT(cpu->cr[17] & 0xffffff,
                   (prefixes << 12) | (locked ? 8 : 0) |
                   (op_prefix ? 0 : 2) | (addr_prefix ? 0 : 4), "intercept prefix metadata");
    C8_BAD_EXPECT(cpu->gr_static[8], UINT64_C(0x123456789abcdef0), "invalid form does not modify EAX");
    C8_BAD_EXPECT(cpu->ar[24], 0xed7, "invalid form preserves flags");
#undef C8_BAD_EXPECT
    merced_destroy(cpu);
}

static void check_ia32_leave(bool fast, bool code32, bool stack32,
                             bool operand_prefix, bool address_prefix,
                             unsigned scenario) {
    Ia32CmpxchgFixture f = {0};
    unsigned size = code32 != operand_prefix ? 4 : 2;
    bool crossing = scenario == 2 || scenario == 3;
    bool fault = scenario == 1 || scenario == 3;
    unsigned offset = crossing ? 0x2fff : scenario == 4 ? 0x10000 - size : 0x2000;
    const uint64_t old_bp = UINT64_C(0xfedcba9812340000) | offset;
    const uint64_t old_sp = UINT64_C(0x88776655abcd1000);
    const uint64_t flags = UINT64_C(0x1122334400000ed7);
    uint32_t frame = stack32 ? (uint32_t)old_bp : (uint16_t)old_bp;
    uint32_t address = 0x2000 + frame;
    uint32_t page = address & ~0xfffu;
    uint32_t value = size == 4 ? UINT32_C(0x7654abcd) : 0xabcd;
    unsigned len = 0;
    f.ram.memory[0x1000 + len++] = 0x3e; /* DS override must not replace SS. */
    if (operand_prefix) f.ram.memory[0x1000 + len++] = 0x66;
    if (address_prefix) f.ram.memory[0x1000 + len++] = 0x67;
    f.ram.memory[0x1000 + len++] = 0xc9;
    for (unsigned i = 0; i < size; i++) {
        unsigned off = (address & 0xfff) + i;
        uint64_t pa = off < 0x1000 ? 0x5000 + off : 0x7000 + off - 0x1000;
        rse_test_write(&f.ram, pa, (value >> (8 * i)) & 0xff, 1);
    }
    const uint64_t nop = UINT64_C(0x08000000);
    rse_test_bundle(&f.ram, 0xd000, 0x11, nop, nop, UINT64_C(8) << 27);
    MercedBus bus = {.ud = &f, .read = ia32_cmpxchg_read,
                     .write = ia32_cmpxchg_write, .ram_size = sizeof(f.ram.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 17) |
               (UINT64_C(1) << 34) | (UINT64_C(1) << 44);
    cpu->ar[24] = flags;
    cpu->gr_static[25] = (uint64_t)code32 << 62;
    cpu->gr_static[26] = ((uint64_t)stack32 << 62) | 0x2000;
    cpu->gr_static[27] = 0x6000; /* Different DS base; implicit access uses SS. */
    cpu->gr_static[12] = old_sp;
    cpu->gr_static[13] = old_bp;
    cpu->cr[2] = 0x8000;
    cpu->cr[72] = UINT64_C(1) << 16;
    cpu->rr[0] = (1 << 8) | (12 << 2);
    for (unsigned i = 0; i < 2; i++) {
        uint64_t va = page + 0x1000 * i, pa = 0x5000 + 0x2000 * i;
        bool present = !(scenario == 1 && i == 0) && !(scenario == 3 && i == 1);
        cpu->dtr[i] = (MercedTlbEntry) {
            .va_start = va, .va_end = va + 0xfff, .pfn_base = pa,
            .rid = 1, .ps = 12, .valid = 1, .itir = (1 << 8) | (12 << 2),
            .pte = pa | 0x60 | present, /* Read-only stack is sufficient. */
        };
    }
#define LEAVE_EXPECT(actual, expected, what) do { \
    uint64_t a_ = (actual), e_ = (expected); \
    if (a_ != e_) { \
        fprintf(stderr, "IA32 leave %s CS.D=%u SS.B=%u 66=%u 67=%u scenario=%u: " \
                "%s: got 0x%" PRIx64 ", expected 0x%" PRIx64 "\n", \
                fast ? "fast" : "slow", code32, stack32, operand_prefix, \
                address_prefix, scenario, what, a_, e_); failures++; \
    } \
} while (0)
    LEAVE_EXPECT(handoff_step(cpu, fast), MERCED_OK, "instruction status");
    if (fault) {
        LEAVE_EXPECT(cpu->ip, 0xd000, "stack read fault");
        LEAVE_EXPECT(cpu->cr[19], 0x1000, "fault restarts at first prefix");
        LEAVE_EXPECT(cpu->cr[20], scenario == 3 ? page + 0x1000 : address,
                      "fault reports unreadable stack page");
        LEAVE_EXPECT(cpu->cr[17] & (UINT64_C(3) << 33), UINT64_C(1) << 34,
                      "implicit stack access is read-only");
        LEAVE_EXPECT(cpu->gr_static[12], old_sp, "fault preserves full ESP");
        LEAVE_EXPECT(cpu->gr_static[13], old_bp, "fault preserves full EBP");
        LEAVE_EXPECT(cpu->ar[24], flags, "fault preserves flags");
        LEAVE_EXPECT(f.reads, 0, "all pages preflight before reading");
        cpu->dtr[scenario == 3].pte |= 1;
        merced_flush_translation_cache(cpu);
        for (unsigned slot = 0; slot < 3; slot++)
            LEAVE_EXPECT(handoff_step(cpu, fast), MERCED_OK, "native handler rfi");
        LEAVE_EXPECT(cpu->ip, 0x1000, "rfi resumes LEAVE");
        LEAVE_EXPECT(handoff_step(cpu, fast), MERCED_OK, "repaired retry");
    }
    uint64_t stack_mask = stack32 ? UINT32_MAX : UINT16_MAX;
    uint64_t operand_mask = size == 4 ? UINT32_MAX : UINT16_MAX;
    LEAVE_EXPECT(cpu->ip, 0x1000 + len, "complete instruction");
    LEAVE_EXPECT(cpu->nfaults, fault, "no additional fault");
    LEAVE_EXPECT(cpu->gr_static[12], (old_sp & ~stack_mask) | ((frame + size) & stack_mask),
                  "SS.B chooses pointer width and increment wraps");
    LEAVE_EXPECT(cpu->gr_static[13], (old_bp & ~operand_mask) | value,
                  "operand size chooses BP/EBP and preserves upper bits");
    LEAVE_EXPECT(cpu->ar[24], flags, "LEAVE does not change flags");
    LEAVE_EXPECT(f.reads, crossing ? size : 1, "saved frame read exactly once");
    LEAVE_EXPECT(f.writes, 0, "LEAVE does not write stack memory");
#undef LEAVE_EXPECT
    merced_destroy(cpu);
}

/* Windows' IA-32 NTDLL enters its native helper with direct JMPE (0F B8).
 * Its displacement is an absolute unsigned CS offset, unlike a near JMP.
 * Keep the pre-existing register/memory form on the same transition path. */
static void check_ia32_jmpe(bool fast, bool code32, bool op_prefix,
                            bool addr_prefix, unsigned form, bool wrap,
                            bool disabled) {
    Ia32CmpxchgFixture f = {0};
    unsigned size = code32 != op_prefix ? 4 : 2;
    bool address32 = code32 != addr_prefix;
    uint32_t csbase = wrap ? UINT32_C(0xffff8000) : 0x2000;
    uint32_t offset = size == 4 ? UINT32_C(0x8000fed9) : 0xfed9;
    uint64_t csd = ((uint64_t)code32 << 62) | csbase;
    uint64_t ssd = UINT64_C(0x4123456700006000);
    const uint64_t flags = UINT64_C(0x1122334400010ed7);
    unsigned len = 0;
    if (op_prefix) f.ram.memory[0x1000 + len++] = 0x66;
    if (addr_prefix) f.ram.memory[0x1000 + len++] = 0x67;
    f.ram.memory[0x1000 + len++] = 0x0f;
    f.ram.memory[0x1000 + len++] = form ? 0x00 : 0xb8;
    if (form == 0) {
        for (unsigned i = 0; i < size; i++)
            f.ram.memory[0x1000 + len++] = offset >> (8 * i);
    } else if (form == 1) {
        f.ram.memory[0x1000 + len++] = 0xf0; /* JMPE EAX/AX */
    } else {
        f.ram.memory[0x1000 + len++] = address32 ? 0x35 : 0x36;
        for (unsigned i = 0; i < (address32 ? 4u : 2u); i++)
            f.ram.memory[0x1000 + len++] = 0x5000u >> (8 * i);
        rse_test_write(&f.ram, 0x5000, offset, size);
    }
    MercedBus bus = {.ud = &f, .read = ia32_cmpxchg_read,
                     .write = ia32_cmpxchg_write, .ram_size = sizeof(f.ram.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 34) |
               (UINT64_C(1) << 37) | (UINT64_C(2) << 41) | (UINT64_C(1) << 44);
    if (disabled) cpu->psr |= UINT64_C(1) << 22;
    uint64_t psr = cpu->psr;
    cpu->cr[2] = 0x8000;
    cpu->cr[72] = UINT64_C(1) << 16;
    cpu->ar[24] = flags;
    cpu->ar[25] = 0x1111;
    cpu->ar[26] = 0x2222;
    cpu->gr_static[25] = csd;
    cpu->gr_static[26] = ssd;
    cpu->gr_static[1] = UINT64_C(0x1122334455667788);
    cpu->nat_static[1] = 1;
    for (unsigned i = 8; i < 16; i++)
        cpu->gr_static[i] = UINT64_C(0x1234567800000000) |
                            ((i & 1) ? 0x80000000u : 0) | i;
    cpu->gr_static[8] = UINT64_C(0xdeadbeef00000000) | offset;
    cpu->ar[64] = UINT64_C(0x123456789abcdef0);
    cpu->bof = cpu->bof_total = 32;
    cpu->rse_anchor_regs = cpu->rse_flushed_regs = 32;
    cpu->rse_anchor_addr = 0x8000;
#define JMPE_EXPECT(actual, expected, what) do { \
    uint64_t a_ = (actual), e_ = (expected); \
    if (a_ != e_) { \
        fprintf(stderr, "IA32 jmpe %s CS.D=%u 66=%u 67=%u form=%u wrap=%u DI=%u: " \
                "%s: got 0x%" PRIx64 ", expected 0x%" PRIx64 "\n", \
                fast ? "fast" : "slow", code32, op_prefix, addr_prefix, form, \
                wrap, disabled, what, a_, e_); failures++; \
    } \
} while (0)
    JMPE_EXPECT(handoff_step(cpu, fast), MERCED_OK, "instruction status");
    if (disabled) {
        JMPE_EXPECT(cpu->ip, 0xd400, "disabled transition is General Exception");
        JMPE_EXPECT(cpu->cr[17] & 0xffff, 0x40, "disabled-transition fault code");
        JMPE_EXPECT(cpu->cr[19], 0x1000, "fault points to first prefix");
        JMPE_EXPECT(cpu->gr_static[1], UINT64_C(0x1122334455667788), "no return-PC commit");
        JMPE_EXPECT(cpu->nat_static[1], 1, "no return-PC NaT commit");
        JMPE_EXPECT(cpu->ar[24], flags, "fault retains RF");
        JMPE_EXPECT(f.reads, 0, "disabled transition precedes operand access");
    } else {
        JMPE_EXPECT(cpu->ip, (uint32_t)(csbase + offset) & UINT32_C(0xfffffff0),
                      "absolute unsigned CS-relative target with 32-bit wrap");
        JMPE_EXPECT(cpu->psr, (psr | (UINT64_C(1) << 4)) & ~((UINT64_C(1) << 34) | (UINT64_C(1) << 37) |
                                     (UINT64_C(3) << 41)), "native PSR transition");
        JMPE_EXPECT(cpu->gr_static[1], 0x1000 + len, "next linear PC in r1");
        JMPE_EXPECT(cpu->nat_static[1], 0, "return PC has no NaT");
        JMPE_EXPECT(cpu->ar[25], csd, "publish CSD");
        JMPE_EXPECT(cpu->ar[26], ssd, "publish SSD");
        JMPE_EXPECT(cpu->ar[24], flags & ~(UINT64_C(1) << 16), "clear only RF");
        JMPE_EXPECT(cpu->gr_static[8], (uint64_t)(int64_t)(int32_t)offset,
                      "EAX is sign-extended on native entry");
        for (unsigned i = 9; i < 16; i++) {
            uint32_t value = ((i & 1) ? 0x80000000u : 0) | i;
            JMPE_EXPECT(cpu->gr_static[i], (uint64_t)(int64_t)(int32_t)value,
                          "remaining IA32 registers sign-extend on exit");
        }
        JMPE_EXPECT(f.reads, form == 2, "one indirect operand read");
    }
    JMPE_EXPECT(cpu->nfaults, disabled, "only requested fault");
    JMPE_EXPECT(cpu->ar[64], UINT64_C(0x123456789abcdef0), "PFS preserved");
    JMPE_EXPECT(cpu->bof_total, 32, "BSP position preserved");
    JMPE_EXPECT(cpu->rse_flushed_regs, 32, "BSPSTORE position preserved");
    JMPE_EXPECT(f.writes, 0, "no operand writes");
#undef JMPE_EXPECT
    merced_destroy(cpu);
}

static void check_ia32_jmpe_fetch(bool fast, unsigned size, bool fault,
                                  bool indirect, bool disabled) {
    RseFixture f = {0};
    const uint32_t target = size == 4 ? UINT32_C(0x8765432f) : 0xfed9;
    f.memory[0x5ffd] = 0x0f;
    f.memory[0x5ffe] = indirect ? 0x00 : 0xb8;
    f.memory[0x5fff] = indirect ? (size == 4 ? 0x35 : 0x36) : target;
    if (indirect) {
        for (unsigned i = 0; i < size; i++)
            f.memory[0x7000 + i] = 0x3000u >> (8 * i);
        rse_test_write(&f, 0x3000, target, size);
    } else {
        for (unsigned i = 1; i < size; i++)
            f.memory[0x7000 + i - 1] = target >> (8 * i);
    }
    const uint64_t nop = UINT64_C(0x08000000);
    rse_test_bundle(&f, 0xd000, 0x11, nop, nop, UINT64_C(8) << 27);
    MercedBus bus = {.ud = &f, .read = rse_test_read,
                     .write = rse_test_write, .ram_size = sizeof(f.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x1ffd;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 34) |
               (UINT64_C(1) << 36) | (UINT64_C(1) << 44);
    if (disabled) cpu->psr |= UINT64_C(1) << 22;
    cpu->gr_static[25] = size == 4 ? UINT64_C(1) << 62 : 0;
    cpu->gr_static[1] = 0xbad;
    cpu->cr[2] = 0x8000;
    cpu->cr[72] = UINT64_C(1) << 16;
    cpu->rr[0] = (1 << 8) | (12 << 2);
    for (unsigned i = 0; i < 2; i++) {
        uint64_t va = 0x1000 + 0x1000 * i, pa = 0x5000 + 0x2000 * i;
        cpu->itr[i] = (MercedTlbEntry) {
            .va_start = va, .va_end = va + 0xfff, .pfn_base = pa,
            .rid = 1, .ps = 12, .valid = 1, .itir = (1 << 8) | (12 << 2),
            .pte = pa | 0x660 | !(fault && i == 1),
        };
    }
    /* Instruction translation remains enabled in this fault handler. */
    cpu->itr[2] = (MercedTlbEntry) {
        .va_start = 0xd000, .va_end = 0xdfff, .pfn_base = 0xd000,
        .rid = 1, .ps = 12, .valid = 1, .itir = (1 << 8) | (12 << 2),
        .pte = 0xd661,
    };
#define JFETCH_EXPECT(actual, expected, what) \
    rse_test_expect((actual), (expected), (what), fast, size, fault, indirect, disabled)
    JFETCH_EXPECT(handoff_step(cpu, fast), MERCED_OK, "JMPE fetch status");
    if (fault) {
        JFETCH_EXPECT(cpu->ip, 0xd000, "immediate continuation page faults");
        JFETCH_EXPECT(cpu->cr[19], 0x1ffd, "fetch fault restarts JMPE");
        JFETCH_EXPECT(cpu->cr[20], 0x2000, "fault address is next instruction page");
        JFETCH_EXPECT(cpu->gr_static[1], 0xbad, "no partial native transition");
        cpu->itr[1].pte |= 1;
        cpu->cr[16] &= ~(UINT64_C(1) << 22); /* Repair DI too before retry. */
        merced_flush_translation_cache(cpu);
        for (unsigned slot = 0; slot < 3; slot++)
            JFETCH_EXPECT(handoff_step(cpu, fast), MERCED_OK, "native fetch handler rfi");
        JFETCH_EXPECT(cpu->ip, 0x1ffd, "rfi resumes direct JMPE");
        JFETCH_EXPECT(handoff_step(cpu, fast), MERCED_OK, "repaired immediate fetch");
    }
    JFETCH_EXPECT(cpu->ip, target & UINT32_C(0xfffffff0), "discontiguous immediate target");
    JFETCH_EXPECT(cpu->gr_static[1], 0x1ffd + 2 + indirect + size,
                   "cross-page return address");
    JFETCH_EXPECT(cpu->nfaults, fault, "exactly the intended fetch fault");
#undef JFETCH_EXPECT
    merced_destroy(cpu);
}

typedef struct {
    RseFixture ram;
    unsigned immediate_reads, continuation_reads, gap_reads;
    unsigned stack_reads, stack_writes;
    unsigned io_reads, io_writes, io_size;
    uint64_t io_address, io_value;
} Ia32FetchIoFixture;

static uint64_t ia32_fetch_io_read(void *ud, uint64_t addr, unsigned size) {
    Ia32FetchIoFixture *f = ud;
    if (addr >= UINT64_C(0x100000000)) {
        f->io_reads++;
        f->io_address = addr;
        f->io_size = size;
        return UINT64_C(0x89abcdef);
    }
    if (addr >= 0x3ff0 && addr < 0x4010) f->stack_reads++;
    return rse_test_read(&f->ram, addr, size);
}

static uint64_t ia32_fetch_io_fetch(void *ud, uint64_t addr, unsigned size) {
    Ia32FetchIoFixture *f = ud;
    if (addr <= 0x5fff && addr + size > 0x5fff) f->immediate_reads++;
    if (addr >= 0x7000 && addr < 0x7008) f->continuation_reads++;
    if (addr >= 0x6000 && addr < 0x7000) f->gap_reads++;
    return rse_test_read(&f->ram, addr, size);
}

static void ia32_fetch_io_write(void *ud, uint64_t addr, uint64_t value,
                                 unsigned size) {
    Ia32FetchIoFixture *f = ud;
    if (addr >= UINT64_C(0x100000000)) {
        f->io_writes++;
        f->io_address = addr;
        f->io_size = size;
        f->io_value = value;
        return;
    }
    if (addr >= 0x3ff0 && addr < 0x4010) f->stack_writes++;
    rse_test_write(&f->ram, addr, value, size);
}

/* MOV immediate and relative CALL both fetch a multi-byte operand starting
 * at VA 1fff. The next virtual page lives at a nonadjacent PA; an absent or
 * nonexecutable continuation must fault before fetching any immediate byte
 * or touching the CALL stack. Repair followed by guest RFI retries once. */
static void check_ia32_split_instruction(bool fast, unsigned size,
                                         bool call, unsigned fault) {
    Ia32FetchIoFixture f = {0};
    const unsigned prefix = size == 2;
    const uint32_t start = 0x1fff - 1 - prefix;
    const uint32_t next = 0x2000 + size - 1;
    const uint32_t immediate = call ? 0x3000 - next : UINT32_C(0x78563412);
    const uint64_t initial_eax = UINT64_C(0x1234567812345678);
    const uint64_t initial_flags = UINT64_C(0x1122334400000247);
    if (prefix) f.ram.memory[0x4000 + start] = 0x66;
    f.ram.memory[0x5ffe] = call ? 0xe8 : 0xb8;
    f.ram.memory[0x5fff] = immediate;
    for (unsigned i = 1; i < size; i++)
        f.ram.memory[0x7000 + i - 1] = immediate >> (8 * i);
    memset(f.ram.memory + 0x6000, 0xcc, 8);
    memset(f.ram.memory + 0x3ff0, 0xa5, 0x20);
    const uint64_t nop = UINT64_C(0x08000000);
    const uint32_t vector = fault == 2 ? 0xd200 : 0xd000;
    rse_test_bundle(&f.ram, vector, 0x11, nop, nop, UINT64_C(8) << 27);
    MercedBus bus = {.ud = &f, .read = ia32_fetch_io_read,
                     .fetch = ia32_fetch_io_fetch, .write = ia32_fetch_io_write,
                     .ram_size = sizeof(f.ram.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = start;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 34) |
               (UINT64_C(1) << 36) | (UINT64_C(1) << 44);
    cpu->ar[24] = initial_flags;
    cpu->gr_static[25] = cpu->gr_static[26] = UINT64_C(1) << 62;
    cpu->gr_static[8] = initial_eax;
    cpu->gr_static[12] = 0x4000;
    cpu->cr[2] = 0x8000;
    cpu->cr[72] = UINT64_C(1) << 16;
    cpu->rr[0] = (1 << 8) | (12 << 2);
    for (unsigned i = 0; i < 2; i++) {
        uint64_t va = 0x1000 + 0x1000 * i, pa = 0x5000 + 0x2000 * i;
        cpu->itr[i] = (MercedTlbEntry) {
            .va_start = va, .va_end = va + 0xfff, .pfn_base = pa,
            .rid = 1, .ps = 12, .valid = 1, .itir = (1 << 8) | (12 << 2),
            .pte = pa | (i == 1 && fault == 2 ? 0x461 : 0x661),
        };
        if (i == 1 && fault == 1) cpu->itr[i].pte &= ~UINT64_C(1);
    }
    cpu->itr[2] = (MercedTlbEntry) {
        .va_start = 0xd000, .va_end = 0xdfff, .pfn_base = 0xd000,
        .rid = 1, .ps = 12, .valid = 1, .itir = (1 << 8) | (12 << 2),
        .pte = 0xd661,
    };
#define FETCH_EXPECT(actual, expected, what) \
    rse_test_expect((actual), (expected), (what), fast, size, call, fault, 0)
    FETCH_EXPECT(handoff_step(cpu, fast), MERCED_OK, "split instruction status");
    if (fault) {
        FETCH_EXPECT(cpu->ip, vector, "continuation instruction-page fault vector");
        FETCH_EXPECT(cpu->cr[19], start, "instruction fault retains first prefix IP");
        FETCH_EXPECT(cpu->cr[20], 0x2000, "fault identifies continuation VA");
        FETCH_EXPECT((cpu->cr[17] >> 32) & 7, 1, "fault reports instruction access");
        FETCH_EXPECT(cpu->gr_static[8], initial_eax, "no destination commit before fetch");
        FETCH_EXPECT(cpu->gr_static[12], 0x4000, "no CALL stack decrement before fetch");
        FETCH_EXPECT(f.immediate_reads, 0, "all immediate bytes preflight before bus fetch");
        FETCH_EXPECT(f.continuation_reads, 0, "no inaccessible instruction bus fetch");
        FETCH_EXPECT(f.stack_reads, 0, "no stack read during instruction fault");
        FETCH_EXPECT(f.stack_writes, 0, "no CALL stack write during instruction fault");
        cpu->itr[1].pte = 0x7661;
        merced_flush_translation_cache(cpu);
        for (unsigned slot = 0; slot < 3; slot++)
            FETCH_EXPECT(handoff_step(cpu, fast), MERCED_OK, "native fetch repair RFI");
        FETCH_EXPECT(cpu->ip, start, "RFI restarts entire instruction");
        FETCH_EXPECT(handoff_step(cpu, fast), MERCED_OK, "retried split instruction");
    }
    uint64_t mask = size == 4 ? UINT32_MAX : UINT16_MAX;
    FETCH_EXPECT(cpu->ip, call ? 0x3000 : next, "correct target or full instruction length");
    FETCH_EXPECT(cpu->gr_static[8], call ? initial_eax :
                 (initial_eax & ~mask) | (immediate & mask), "complete discontiguous immediate");
    FETCH_EXPECT(cpu->gr_static[12], call ? 0x4000 - size : 0x4000,
                   "exactly one successful CALL push");
    FETCH_EXPECT(f.stack_writes, call, "exactly one whole return-address write");
    if (call)
        FETCH_EXPECT(rse_test_read(&f.ram, 0x4000 - size, size), next & mask,
                       "CALL saves address after all immediate bytes");
    FETCH_EXPECT(f.immediate_reads, 1, "first immediate byte fetched once after preflight");
    FETCH_EXPECT(f.continuation_reads, size - 1, "read mapped continuation bytes");
    FETCH_EXPECT(f.gap_reads, 0, "never fetch physical gap as instruction data");
    FETCH_EXPECT(cpu->ar[24], initial_flags, "MOV and CALL preserve all flags");
    FETCH_EXPECT(cpu->nfaults, !!fault, "one intended fault only");
#undef FETCH_EXPECT
    merced_destroy(cpu);
}

/* RET imm16 must fetch its whole immediate before consuming any stack
 * entry, including before changing CS for the supported far-return form. */
static void check_ia32_x87_control(bool fast, unsigned kind, bool op_prefix,
                                   bool address32, bool crossing,
                                   unsigned scenario, bool unmask) {
    Ia32CmpxchgFixture f = {0};
    uint32_t address = crossing ? 0x4fff : 0x4000;
    uint64_t fcr = UINT64_C(0x00001f800000027f);
    uint64_t fsr = UINT64_C(0x00000000b5a53d01) | (scenario >= 3 ? 0x8080 : 0);
    uint64_t fir = UINT64_C(0x123456789abcdef0), fdr = UINT64_C(0x23456789abcdef01);
    uint16_t source = unmask ? 0xfcfe : 0xfcff;
    unsigned len = 0;
    f.ram.memory[0x1000 + len++] = 0x64;
    if (op_prefix) f.ram.memory[0x1000 + len++] = 0x66;
    if (!address32) f.ram.memory[0x1000 + len++] = 0x67;
    f.ram.memory[0x1000 + len++] = kind == 2 ? 0xdb : 0xd9;
    f.ram.memory[0x1000 + len++] = kind == 2 ? 0xe2 :
                                  (kind == 0 ? 0x38 : 0x28) | (address32 ? 5 : 6);
    if (kind != 2)
        for (unsigned i = 0; i < (address32 ? 4u : 2u); i++)
            f.ram.memory[0x1000 + len++] = (address - 0x1000) >> (8 * i);
    for (unsigned i = 0; i < 2; i++)
        rse_test_write(&f.ram, ia32_string_test_pa(address + i), source >> (8 * i), 1);
    rse_test_write(&f.ram, 0x6000, UINT64_C(0xcccccccccccccccc), 8);
    const uint64_t nop = UINT64_C(0x08000000);
    rse_test_bundle(&f.ram, 0xd000, 0x11, nop, nop, UINT64_C(8) << 27);
    rse_test_bundle(&f.ram, 0xd300, 0x11, nop, nop, UINT64_C(8) << 27);
    MercedBus bus = {.ud = &f, .read = ia32_cmpxchg_read,
                     .write = ia32_cmpxchg_write, .ram_size = sizeof(f.ram.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 17) |
               (UINT64_C(1) << 34) | (UINT64_C(1) << 44);
    if (scenario == 5) cpu->psr |= UINT64_C(1) << 18;
    cpu->gr_static[25] = UINT64_C(1) << 62;
    cpu->gr_static[27] = 0x9000;
    cpu->gr_static[28] = 0x1000;
    cpu->ar[21] = fcr;
    cpu->ar[24] = UINT64_C(0x3456789a00000247);
    cpu->ar[27] = scenario >= 4 ? 4 : 0;
    cpu->ar[28] = fsr;
    cpu->ar[29] = fir;
    cpu->ar[30] = fdr;
    cpu->cr[2] = 0x8000;
    cpu->cr[20] = 0x123456;
    cpu->cr[24] = 0x234567;
    cpu->cr[72] = UINT64_C(1) << 16;
    cpu->rr[0] = (1 << 8) | (12 << 2);
    for (unsigned i = 0; i < 2; i++) {
        uint64_t va = 0x4000 + 0x1000 * i, pa = 0x5000 + 0x2000 * i;
        uint64_t pte = pa | (kind ? 0x61 : 0x461);
        if (i && (scenario == 1 || scenario == 3)) pte &= ~UINT64_C(1);
        if (i && scenario == 2) pte &= ~UINT64_C(0x400);
        cpu->dtr[i] = (MercedTlbEntry) {
            .va_start = va, .va_end = va + 0xfff, .pfn_base = pa,
            .rid = 1, .ps = 12, .valid = 1, .itir = (1 << 8) | (12 << 2), .pte = pte,
        };
    }
    bool numeric = scenario == 3 && kind == 1;
    bool fp_fault = numeric || scenario >= 4;
    bool memory_fault = kind != 2 && crossing &&
                        (scenario == 1 || (scenario == 2 && kind == 0) ||
                         (scenario == 3 && kind == 0));
#define X87_CONTROL_EXPECT(actual, expected, what) do { \
    uint64_t a_ = (actual), e_ = (expected); \
    if (a_ != e_) { \
        fprintf(stderr, "IA32 x87 %s kind=%u 66=%u address32=%u split=%u scenario=%u unmask=%u: " \
                "%s: got 0x%" PRIx64 ", expected 0x%" PRIx64 "\n", \
                fast ? "fast" : "slow", kind, op_prefix, address32, crossing, scenario, unmask, \
                what, a_, e_); failures++; \
    } \
} while (0)
    X87_CONTROL_EXPECT(handoff_step(cpu, fast), MERCED_OK, "control instruction status");
    if (fp_fault || memory_fault) {
        uint64_t vector = fp_fault ? (scenario == 5 ? 0xd500 : 0xe900) :
                          scenario == 2 ? 0xd300 : 0xd000;
        X87_CONTROL_EXPECT(cpu->ip, vector, "floating-point/data fault priority");
        X87_CONTROL_EXPECT(cpu->cr[19], 0x1000, "fault restarts control prefixes");
        X87_CONTROL_EXPECT(cpu->ar[21], fcr, "fault preserves entire FCR");
        X87_CONTROL_EXPECT(cpu->ar[28], fsr, "fault preserves entire FSR");
        X87_CONTROL_EXPECT(f.reads, 0, "fault checks precede any operand read");
        X87_CONTROL_EXPECT(f.writes, 0, "fault checks precede any operand write");
        if (fp_fault) {
            X87_CONTROL_EXPECT(cpu->cr[17] & 0xffffff,
                                scenario == 5 ? 1 : (numeric ? 16 : 7) << 16,
                                "disabled/#NM/#MF exception metadata");
            X87_CONTROL_EXPECT(cpu->cr[20], 0x123456, "numeric fault leaves IFA unchanged");
            X87_CONTROL_EXPECT(cpu->cr[24], 0x234567, "numeric fault leaves IIM unchanged");
        } else if (cpu->ip == vector) {
            cpu->dtr[1].pte |= kind ? 1 : 0x401;
            merced_flush_translation_cache(cpu);
            for (unsigned slot = 0; slot < 3; slot++)
                X87_CONTROL_EXPECT(handoff_step(cpu, fast), MERCED_OK, "control operand RFI retry");
            X87_CONTROL_EXPECT(cpu->ip, 0x1000, "control retry preserves prefixes");
            X87_CONTROL_EXPECT(handoff_step(cpu, fast), MERCED_OK, "control operand retries once");
        }
    }
    if (!fp_fault) {
        X87_CONTROL_EXPECT(cpu->ip, 0x1000 + len, "complete control instruction");
        uint16_t fcw = (source & 0x1f3f) | 0x40;
        uint64_t expected_fsr = kind == 2 ? fsr & ~UINT64_C(0x80ff) : kind == 1 ?
            (fsr & ~UINT64_C(0x8080)) | (unmask ? 0x8080 : 0) : fsr;
        X87_CONTROL_EXPECT(cpu->ar[21], kind == 1 ? (fcr & ~UINT64_C(0xffff)) | fcw : fcr,
                            "only FCW changes; reserved control bits normalized and SSE preserved");
        X87_CONTROL_EXPECT(cpu->ar[28], expected_fsr, "sticky status/TOP/tags preserved except defined ES/B/clear bits");
        for (unsigned i = 0; i < 2; i++)
            X87_CONTROL_EXPECT(rse_test_read(&f.ram, ia32_string_test_pa(address + i), 1),
                                ((kind == 0 ? fcr : source) >> (8 * i)) & 0xff,
                                "fixed two-byte control transfer");
        if (kind == 1 && unmask) {
            f.ram.memory[0x1000 + len] = 0x9b;
            X87_CONTROL_EXPECT(handoff_step(cpu, fast), MERCED_OK, "newly unmasked flag waits until next FWAIT");
            X87_CONTROL_EXPECT(cpu->ip, 0xe900, "following FWAIT reports delayed numeric exception");
            X87_CONTROL_EXPECT(cpu->cr[19], 0x1000 + len, "delayed fault names FWAIT, not FLDCW");
            X87_CONTROL_EXPECT(cpu->cr[17] & 0xffffff, 16 << 16, "delayed #MF metadata");
        }
    }
    X87_CONTROL_EXPECT(cpu->ar[29], fir, "control instructions preserve FIR");
    X87_CONTROL_EXPECT(cpu->ar[30], fdr, "control instructions preserve FDR");
    X87_CONTROL_EXPECT(cpu->ar[24], UINT64_C(0x3456789a00000247), "control instructions preserve EFLAGS");
    X87_CONTROL_EXPECT(rse_test_read(&f.ram, 0x6000, 8), UINT64_C(0xcccccccccccccccc),
                        "control operand never touches physical gap");
#undef X87_CONTROL_EXPECT
    merced_destroy(cpu);
}

static void check_ia32_ret_fetch(bool fast, unsigned size, bool far_return,
                                 unsigned fault) {
    Ia32FetchIoFixture f = {0};
    uint32_t start = size == 2 ? 0x1ffd : 0x1ffe;
    if (size == 2) f.ram.memory[0x5ffd] = 0x66;
    f.ram.memory[0x5ffe] = far_return ? 0xca : 0xc2;
    f.ram.memory[0x5fff] = 0x10;
    f.ram.memory[0x7000] = 0;
    rse_test_write(&f.ram, 0x4000, 0x3000, size);
    rse_test_write(&f.ram, 0x4000 + size, far_return ? 0x20 : 0x3456, size);
    const uint64_t nop = UINT64_C(0x08000000);
    uint32_t vector = fault == 2 ? 0xd200 : 0xd000;
    rse_test_bundle(&f.ram, vector, 0x11, nop, nop, UINT64_C(8) << 27);
    MercedBus bus = {.ud = &f, .read = ia32_fetch_io_read,
                     .fetch = ia32_fetch_io_fetch, .write = ia32_fetch_io_write,
                     .ram_size = sizeof(f.ram.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = start;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 34) |
               (UINT64_C(1) << 36) | (UINT64_C(1) << 44);
    cpu->gr_static[25] = cpu->gr_static[26] = UINT64_C(1) << 62;
    cpu->gr_static[17] = 0x1b;
    cpu->gr_static[12] = 0x4000;
    cpu->ar[24] = 0x247;
    cpu->cr[2] = 0x8000;
    cpu->cr[72] = UINT64_C(1) << 16;
    cpu->rr[0] = (1 << 8) | (12 << 2);
    for (unsigned i = 0; i < 3; i++) {
        uint64_t va = i == 2 ? 0xd000 : 0x1000 + i * 0x1000;
        uint64_t pa = i == 2 ? 0xd000 : 0x5000 + i * 0x2000;
        uint64_t pte = pa | (i == 1 && fault == 2 ? 0x461 : 0x661);
        if (i == 1 && fault == 1) pte &= ~UINT64_C(1);
        cpu->itr[i] = (MercedTlbEntry) {
            .va_start = va, .va_end = va + 0xfff, .pfn_base = pa,
            .rid = 1, .ps = 12, .valid = 1, .itir = (1 << 8) | (12 << 2), .pte = pte,
        };
    }
#define RET_FETCH_EXPECT(actual, expected, what) \
    rse_test_expect((actual), (expected), (what), fast, size, far_return, fault, false)
    RET_FETCH_EXPECT(handoff_step(cpu, fast), MERCED_OK, "RET split immediate status");
    if (fault) {
        RET_FETCH_EXPECT(cpu->ip, vector, "RET immediate continuation faults");
        RET_FETCH_EXPECT(cpu->cr[19], start, "RET fault restarts original prefixes");
        RET_FETCH_EXPECT(cpu->cr[20], 0x2000, "RET faults on second instruction page");
        RET_FETCH_EXPECT(cpu->gr_static[12], 0x4000, "RET leaves SP unchanged before full fetch");
        RET_FETCH_EXPECT(cpu->gr_static[17], 0x1b, "RETF leaves CS unchanged before full fetch");
        RET_FETCH_EXPECT(f.stack_reads, 0, "RET does not pop before immediate fetch succeeds");
        RET_FETCH_EXPECT(f.immediate_reads, 0, "RET preflights both immediate bytes");
        if (cpu->ip != vector) { merced_destroy(cpu); return; }
        cpu->itr[1].pte = 0x7661;
        merced_flush_translation_cache(cpu);
        for (unsigned slot = 0; slot < 3; slot++)
            RET_FETCH_EXPECT(handoff_step(cpu, fast), MERCED_OK, "RET immediate native RFI repair");
        RET_FETCH_EXPECT(cpu->ip, start, "RET retries original instruction");
        RET_FETCH_EXPECT(handoff_step(cpu, fast), MERCED_OK, "RET retry status");
    }
    RET_FETCH_EXPECT(cpu->ip, far_return ? 0x3200 : 0x3000, "RET uses the original return address");
    RET_FETCH_EXPECT(cpu->gr_static[12], 0x4010 + size + (far_return ? 2 : 0),
                       "RET pops and applies cleanup exactly once");
    RET_FETCH_EXPECT(f.stack_reads, far_return ? 2 : 1, "exact number of successful pops");
    RET_FETCH_EXPECT(f.stack_writes, 0, "RET does not write the stack");
    RET_FETCH_EXPECT(f.immediate_reads, 1, "RET first immediate byte fetched once");
    RET_FETCH_EXPECT(f.continuation_reads, 1, "RET second immediate byte fetched once");
    RET_FETCH_EXPECT(cpu->ar[24], 0x247, "RET leaves flags unchanged");
    RET_FETCH_EXPECT(cpu->nfaults, !!fault, "one intended RET fault only");
#undef RET_FETCH_EXPECT
    merced_destroy(cpu);
}

/* Real IN/OUT instructions must retain the high AR.KR0 aperture bits and
 * issue one 16/32-bit bus transaction. Port 0fff also makes the expanded
 * physical aperture address cross a 4-KiB boundary without losing width. */
static void check_ia32_wide_io(bool fast, unsigned size, bool crossing) {
    Ia32FetchIoFixture f = {0};
    unsigned len = 0;
    if (size == 2) f.ram.memory[0x1000 + len++] = 0x66;
    f.ram.memory[0x1000 + len++] = 0xed; /* IN AX/EAX,DX */
    if (size == 2) f.ram.memory[0x1000 + len++] = 0x66;
    f.ram.memory[0x1000 + len++] = 0xef; /* OUT DX,AX/EAX */
    const uint64_t aperture = UINT64_C(0x00000e0000000000);
    const unsigned port = crossing ? 0xfff : 0x1f0;
    const uint64_t expected_address = aperture | ((uint64_t)(port & 0xfffc) << 10) |
                                      (port & 0xfff);
    const uint64_t old_eax = UINT64_C(0x1234567876543210);
    const uint64_t mask = size == 4 ? UINT32_MAX : UINT16_MAX;
    MercedBus bus = {.ud = &f, .read = ia32_fetch_io_read,
                     .fetch = ia32_fetch_io_fetch, .write = ia32_fetch_io_write,
                     .ram_size = sizeof(f.ram.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 34) | (UINT64_C(1) << 44);
    cpu->gr_static[25] = UINT64_C(1) << 62;
    cpu->gr_static[8] = old_eax;
    cpu->gr_static[10] = port;
    cpu->ar[0] = aperture;
    cpu->ar[24] = 0x247;
    cpu->cr[72] = UINT64_C(1) << 16;
#define IO_EXPECT(actual, expected, what) \
    rse_test_expect((actual), (expected), (what), fast, size, crossing, 0, 0)
    IO_EXPECT(handoff_step(cpu, fast), MERCED_OK, "wide IN status");
    IO_EXPECT(f.io_reads, 1, "one wide I/O read transaction");
    IO_EXPECT(f.io_address, expected_address, "IN preserves high aperture address bits");
    IO_EXPECT(f.io_size, size, "IN preserves bus transaction width");
    IO_EXPECT(cpu->gr_static[8], (old_eax & ~mask) | (UINT32_C(0x89abcdef) & mask),
                 "IN commits the correct accumulator width");
    IO_EXPECT(handoff_step(cpu, fast), MERCED_OK, "wide OUT status");
    IO_EXPECT(f.io_writes, 1, "one wide I/O write transaction");
    IO_EXPECT(f.io_address, expected_address, "OUT preserves high aperture address bits");
    IO_EXPECT(f.io_size, size, "OUT preserves bus transaction width");
    IO_EXPECT(f.io_value, UINT32_C(0x89abcdef) & mask, "OUT sends accumulator value");
    IO_EXPECT(cpu->ip, 0x1000 + len, "IN/OUT instruction lengths");
    IO_EXPECT(cpu->ar[24], 0x247, "I/O preserves flags");
    IO_EXPECT(cpu->nfaults, 0, "I/O does not introduce address truncation faults");
#undef IO_EXPECT
    merced_destroy(cpu);
}

/* Independent FWAIT state/priority matrix. Insert before production main. */
static void check_ia32_fwait_priority(bool fast, unsigned sample) {
    RseFixture f = {0};
    uint64_t fcr = UINT64_C(0x00001f800000027f);
    uint64_t fsr = UINT64_C(0x0000003fffff5500); /* SSE flags, tags, TOP/CC */
    uint64_t cflg = UINT64_C(0x0000060100000101), extra_psr = 0;
    unsigned vector = 0, isr = 0;
    bool locked = sample == 26 || sample == 27;
    if (sample >= 1 && sample <= 6) fsr |= UINT64_C(1) << (sample - 1);
    if (sample >= 7 && sample <= 12) {
        unsigned bit = sample - 7;
        fsr |= UINT64_C(1) << bit;
        fcr &= ~(UINT64_C(1) << bit);
        vector = 0xe900; isr = 16u << 16;
    }
    switch (sample) {
    case 13: fsr |= 0x80; vector = 0xe900; isr = 16u << 16; break;
    case 14: break; /* only SSE status must never produce x87 #MF */
    case 15: extra_psr = UINT64_C(1) << 18; vector = 0xd500; isr = 1; break;
    case 16: extra_psr = UINT64_C(1) << 18; fsr |= 0x80; cflg |= 0xa;
             vector = 0xd500; isr = 1; break;
    case 17: extra_psr = UINT64_C(1) << 19; break;
    case 18: cflg |= 2; break; /* MP alone */
    case 19: cflg |= 8; break; /* TS without MP does not fault FWAIT */
    case 20: cflg |= 0xa; vector = 0xe900; isr = 7u << 16; break;
    case 21: cflg |= 4; break; /* EM does not disable FWAIT */
    case 22: cflg |= 0xe; vector = 0xe900; isr = 7u << 16; break;
    case 23: fsr |= 0x80; vector = 0xe900; isr = 16u << 16; break; /* NE=0 */
    case 24: fsr |= 0x80; cflg |= 0x20; vector = 0xe900; isr = 16u << 16; break;
    case 25: fsr |= 0x80; cflg |= 0xa; vector = 0xe900; isr = 7u << 16; break;
    case 26: vector = 0xea00; isr = 0x100e; break;
    case 27: extra_psr = UINT64_C(1) << 18; fsr |= 0x80; cflg |= 0xa;
             vector = 0xea00; isr = 0x100e; break;
    case 28: fsr |= 0x8000; break; /* inconsistent B alone is not ES */
    case 29: fsr |= 0x7f; break; /* masked exceptions plus stack-fault flag */
    default: break;
    }
    f.memory[0x1000] = locked ? 0xf0 : 0x9b;
    if (locked) f.memory[0x1001] = 0x9b;
    const uint64_t nop = UINT64_C(0x08000000);
    if (vector) rse_test_bundle(&f, vector, 0x11, nop, nop, UINT64_C(8) << 27);
    MercedBus bus = {.ud = &f, .read = rse_test_read, .write = rse_test_write,
                     .ram_size = sizeof(f.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 34) |
               (UINT64_C(1) << 44) | extra_psr;
    cpu->gr_static[25] = UINT64_C(1) << 62;
    cpu->cr[2] = 0x8000;
    cpu->cr[72] = UINT64_C(1) << 16;
    cpu->ar[21] = fcr; cpu->ar[28] = fsr; cpu->ar[27] = cflg;
    cpu->ar[29] = UINT64_C(0x0123001b76543210);
    cpu->ar[30] = UINT64_C(0x0000002376543210);
    cpu->ar[40] = UINT64_C(0x123456789abcdef0);
    cpu->ar[24] = UINT64_C(0x1234567800000ed7);
    cpu->cr[20] = UINT64_C(0x1122334455667788);
    cpu->cr[24] = UINT64_C(0x8877665544332211);
#define WAIT_EXPECT(actual, expected, what) \
    rse_test_expect((actual), (expected), (what), fast, sample, 0, 0, 0)
    WAIT_EXPECT(handoff_step(cpu, fast), MERCED_OK, "FWAIT status");
    WAIT_EXPECT(cpu->ip, vector ? vector : 0x1001, "FWAIT native vector or one-byte completion");
    WAIT_EXPECT(cpu->ar[21], fcr, "FWAIT preserves FCR and SSE controls");
    WAIT_EXPECT(cpu->ar[28], fsr, "FWAIT preserves FSR and pending exception information");
    WAIT_EXPECT(cpu->ar[27], cflg, "FWAIT preserves CFLG");
    WAIT_EXPECT(cpu->ar[29], UINT64_C(0x0123001b76543210), "FWAIT preserves FIR/FOP");
    WAIT_EXPECT(cpu->ar[30], UINT64_C(0x0000002376543210), "FWAIT preserves FDR");
    WAIT_EXPECT(cpu->ar[40], UINT64_C(0x123456789abcdef0), "FWAIT does not use native FPSR");
    WAIT_EXPECT(cpu->ar[24], UINT64_C(0x1234567800000ed7), "FWAIT preserves integer flags");
    WAIT_EXPECT(cpu->cr[20], UINT64_C(0x1122334455667788), "FP fault does not write IFA");
    WAIT_EXPECT(cpu->cr[24], locked ? 0x9b : UINT64_C(0x8877665544332211),
                    "only invalid LOCK instruction intercept writes IIM");
    WAIT_EXPECT(cpu->nfaults, vector != 0, "exactly expected FWAIT fault count");
    if (vector) {
        WAIT_EXPECT(cpu->cr[17], isr, "FWAIT ISR vector and error code");
        WAIT_EXPECT(cpu->cr[19], 0x1000, "fault IIP retains FWAIT prefix address");
        WAIT_EXPECT(cpu->cr[22], 0x1000, "fault IIPA retains FWAIT prefix address");
        WAIT_EXPECT((cpu->psr >> 34) & 1, 0, "handler enters native instruction set");
        cpu->cr[16] &= ~(UINT64_C(1) << 18);
        cpu->ar[27] &= ~UINT64_C(0xe);
        cpu->ar[28] &= ~UINT64_C(0x80ff);
        if (locked) f.memory[0x1000] = 0x66; /* replace LOCK with harmless prefix */
        for (unsigned slot = 0; slot < 3; slot++)
            WAIT_EXPECT(handoff_step(cpu, fast), MERCED_OK, "native FP handler RFI");
        WAIT_EXPECT(cpu->ip, 0x1000, "RFI retries the waiting instruction");
        WAIT_EXPECT(handoff_step(cpu, fast), MERCED_OK, "FWAIT completes after repair");
        WAIT_EXPECT(cpu->ip, 0x1001 + locked, "repaired FWAIT advances exactly once");
        WAIT_EXPECT(cpu->nfaults, 1, "retry does not duplicate FP fault");
    }
#undef WAIT_EXPECT
    merced_destroy(cpu);
}

static void check_actual_control87_sequence(bool fast) {
    RseFixture f = {0};
    /* Literal MSVCRT _control87 instructions, relocated into the fixture. */
    const uint8_t code[] = {0x9b, 0xd9, 0x7d, 0xfc, 0xd9, 0x6d, 0x0c};
    memcpy(f.memory + 0x1000, code, sizeof(code));
    rse_test_write(&f, 0x4ffc, 0xfeedbeef, 4);
    rse_test_write(&f, 0x500c, 0x037f, 2);
    rse_test_write(&f, 0x5ffc, 0xdeadbeef, 4); /* distinct DS address */
    MercedBus bus = {.ud = &f, .read = rse_test_read, .write = rse_test_write,
                     .ram_size = sizeof(f.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 34) |
               (UINT64_C(1) << 44) | (UINT64_C(1) << 19); /* live checkpoint DFH */
    cpu->gr_static[25] = UINT64_C(1) << 62;
    cpu->gr_static[26] = (UINT64_C(1) << 62) | 0x1000;
    cpu->gr_static[27] = 0x2000;
    cpu->gr_static[13] = 0x4000;
    cpu->ar[21] = UINT64_C(0x00001f800000027f);
    cpu->ar[28] = UINT64_C(0x00000000ffff0000);
    cpu->ar[27] = UINT64_C(0x0000060100000101);
    cpu->ar[29] = UINT64_C(0x0123001b76543210);
    cpu->ar[30] = UINT64_C(0x0000002376543210);
    cpu->ar[24] = 0x247;
    cpu->cr[72] = UINT64_C(1) << 16;
#define CONTROL87_EXPECT(actual, expected, what) \
    rse_test_expect((actual), (expected), (what), fast, 0, 0, 0, 0)
    CONTROL87_EXPECT(handoff_step(cpu, fast), MERCED_OK, "MSVCRT FWAIT executes");
    CONTROL87_EXPECT(cpu->ip, 0x1001, "FWAIT advances one byte");
    CONTROL87_EXPECT(handoff_step(cpu, fast), MERCED_OK, "MSVCRT FNSTCW [EBP-4] executes");
    CONTROL87_EXPECT(rse_test_read(&f, 0x4ffc, 4), UINT32_C(0xfeed027f),
                        "FNSTCW stores exactly FCW to SS:EBP-4");
    CONTROL87_EXPECT(rse_test_read(&f, 0x5ffc, 4), UINT32_C(0xdeadbeef),
                        "FNSTCW does not use DS for EBP base");
    CONTROL87_EXPECT(handoff_step(cpu, fast), MERCED_OK, "MSVCRT FLDCW [EBP+12] executes");
    CONTROL87_EXPECT(cpu->ip, 0x1007, "three literal instructions consume seven bytes");
    CONTROL87_EXPECT(cpu->ar[21], UINT64_C(0x00001f800000037f),
                        "FLDCW changes precision while preserving SSE controls");
    CONTROL87_EXPECT(cpu->ar[28], UINT64_C(0x00000000ffff0000), "control sequence preserves tags/status");
    CONTROL87_EXPECT(cpu->ar[29], UINT64_C(0x0123001b76543210), "control sequence preserves FIR/FOP");
    CONTROL87_EXPECT(cpu->ar[30], UINT64_C(0x0000002376543210), "control sequence preserves FDR");
    CONTROL87_EXPECT(cpu->ar[24], 0x247, "control sequence preserves EFLAGS");
    CONTROL87_EXPECT(cpu->nfaults, 0, "actual quiet state does not fault");
#undef CONTROL87_EXPECT
    merced_destroy(cpu);
}

/* Independent bit-at-a-time oracle for SHLD/SHRD; undefined flags/results
 * are deliberately excluded from assertions rather than tied to the decoder. */
static uint32_t ia32_shiftd_oracle(uint32_t dest, uint32_t src, unsigned width,
                                  unsigned count, bool right, uint64_t *flags,
                                  uint64_t *check_flags) {
    const uint32_t mask = width == 32 ? UINT32_MAX : 0xffff;
    const uint32_t sign = UINT32_C(1) << (width - 1);
    uint32_t before = dest & mask;
    dest &= mask;
    src &= mask;
    count &= 31;
    *check_flags = UINT64_MAX;
    if (!count) return dest;
    if (count > width) {
        *check_flags &= ~UINT64_C(0x8d5);
        return dest; /* destination value must not be asserted in this case */
    }
    unsigned cf = 0;
    for (unsigned i = 0; i < count; i++) {
        if (right) {
            cf = dest & 1;
            dest = (dest >> 1) | ((src & 1) ? sign : 0);
            src >>= 1;
        } else {
            cf = (dest & sign) != 0;
            dest = ((dest << 1) & mask) | ((src & sign) != 0);
            src = (src << 1) & mask;
        }
    }
    *flags &= ~UINT64_C(0xc5);
    if (cf) *flags |= 1;
    if (!dest) *flags |= 0x40;
    if (dest & sign) *flags |= 0x80;
    unsigned ones = 0;
    for (unsigned b = 0; b < 8; b++) ones += (dest >> b) & 1;
    if (!(ones & 1)) *flags |= 4;
    *check_flags &= ~UINT64_C(0x10); /* AF undefined whenever a shift occurs */
    if (count == 1) {
        *flags &= ~UINT64_C(0x800);
        if ((before ^ dest) & sign) *flags |= 0x800;
    } else {
        *check_flags &= ~UINT64_C(0x800);
    }
    return dest;
}

/* form: distinct EAX/EDX, identical EAX/EAX, dest ECX, source ECX,
 * identical ECX/ECX, and memory [EDI]/EDX. scenario: normal, split,
 * read-only first, absent second, read-only second, invalid LOCK. */
static void check_ia32_shiftd(bool fast, unsigned size, unsigned opcode,
                              unsigned count, unsigned form, unsigned sample,
                              unsigned scenario) {
    Ia32CmpxchgFixture f = {0};
    bool right = opcode & 8, immediate = !(opcode & 1), memory = form == 5;
    bool crossing = scenario == 1 || scenario == 3 || scenario == 4;
    bool fault = scenario >= 2 && scenario <= 4, locked = scenario == 5;
    unsigned address = crossing ? 0x4fff : 0x4000;
    static const uint8_t modrm[] = {0xd0,0xc0,0xd1,0xc8,0xc9,0x17};
    static const uint32_t values[][2] = {
        {0,0}, {UINT32_MAX,0}, {0x80008000,0x7fff7fff},
        {0x13579bdf,0x2468ace0}, {0x7fffffff,0x80000001},
    };
    unsigned dst = modrm[form] & 7, src = (modrm[form] >> 3) & 7, len = 0;
    if (locked) f.ram.memory[0x1000 + len++] = 0xf0;
    if (size == 2) f.ram.memory[0x1000 + len++] = 0x66;
    f.ram.memory[0x1000 + len++] = 0x0f;
    f.ram.memory[0x1000 + len++] = opcode;
    f.ram.memory[0x1000 + len++] = modrm[form];
    if (immediate) f.ram.memory[0x1000 + len++] = count;
    uint64_t initial[8], expected[8];
    for (unsigned i = 0; i < 8; i++) initial[i] = UINT64_C(0x10203040a0b0c000) | i;
    if (!memory) ia32_test_set_register(initial, dst, size, values[sample][0]);
    ia32_test_set_register(initial, src, size, values[sample][1]);
    if (memory) initial[7] = UINT64_C(0x1020304000000000) | address;
    /* CL may alias either or both data operands; overwrite it last so the
     * oracle uses the actual original architectural value, not the sample. */
    if (!immediate) ia32_test_set_register(initial, 1, 1, count);
    memcpy(expected, initial, sizeof(initial));
    const uint32_t mask = size == 4 ? UINT32_MAX : 0xffff;
    uint32_t dest_value = memory ? values[sample][0] & mask : initial[dst] & mask;
    uint32_t src_value = initial[src] & mask;
    const uint64_t initial_flags = sample & 1 ? UINT64_C(0x9182736400200ed7) :
                                               UINT64_C(0x9182736400200202);
    uint64_t wanted_flags = initial_flags, flag_mask;
    uint32_t result = ia32_shiftd_oracle(dest_value, src_value, size * 8, count,
                                        right, &wanted_flags, &flag_mask);
    bool defined = (count & 31) <= size * 8;
    if (!memory && defined) ia32_test_set_register(expected, dst, size, result);
    memset(f.ram.memory + 0x5ff0, 0xa5, 0x20);
    memset(f.ram.memory + 0x7000, 0xa5, 8);
    for (unsigned i = 0; i < size; i++) {
        unsigned off = (address & 0xfff) + i;
        unsigned pa = off < 0x1000 ? 0x5000 + off : 0x7000 + off - 0x1000;
        rse_test_write(&f.ram, pa, dest_value >> (8 * i), 1);
    }
    unsigned vector = scenario == 3 ? 0xd000 : 0xd300;
    rse_test_bundle(&f.ram, vector, 0x11, 0x08000000, 0x08000000, UINT64_C(8) << 27);
    MercedBus bus = {.ud = &f, .read = ia32_cmpxchg_read,
                     .write = ia32_cmpxchg_write, .ram_size = sizeof(f.ram.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 17) |
               (UINT64_C(1) << 34) | (UINT64_C(1) << 44);
    cpu->gr_static[25] = UINT64_C(1) << 62;
    memcpy(cpu->gr_static + 8, initial, sizeof(initial));
    cpu->ar[24] = initial_flags;
    cpu->cr[2] = 0x8000;
    cpu->cr[72] = UINT64_C(1) << 16;
    cpu->rr[0] = (1 << 8) | (12 << 2);
    for (unsigned i = 0; i < 2; i++) {
        uint64_t va = 0x4000 + 0x1000 * i, pa = 0x5000 + 0x2000 * i;
        cpu->dtr[i] = (MercedTlbEntry) {
            .va_start = va, .va_end = va + 0xfff, .pfn_base = pa,
            .rid = 1, .ps = 12, .valid = 1, .itir = (1 << 8) | (12 << 2),
            .pte = pa | 0x461,
        };
        if ((scenario == 2 && !i) || (scenario == 4 && i)) cpu->dtr[i].pte &= ~UINT64_C(0x600);
        if (scenario == 3 && i) cpu->dtr[i].pte &= ~UINT64_C(1);
        if (locked) cpu->dtr[i].pte &= ~UINT64_C(1); /* LOCK precedes operand fault */
    }
#define SHIFTD_EXPECT(actual, expected_, what) do { \
    uint64_t a_ = (actual), e_ = (expected_); \
    if (a_ != e_) { \
        fprintf(stderr, "IA32 shiftd %s size=%u op=%02x count=%u form=%u sample=%u scenario=%u: " \
                "%s: got 0x%" PRIx64 ", expected 0x%" PRIx64 "\n", \
                fast ? "fast" : "slow", size, opcode, count, form, sample, scenario, \
                what, a_, e_); failures++; \
    } \
} while (0)
    SHIFTD_EXPECT(handoff_step(cpu, fast), MERCED_OK, "instruction status");
    if (locked || fault) {
        SHIFTD_EXPECT(cpu->ip, locked ? 0xea00 : vector, "fault vector");
        SHIFTD_EXPECT(cpu->cr[19], 0x1000, "restart prefix address");
        SHIFTD_EXPECT(cpu->ar[24], initial_flags, "precise flags");
        for (unsigned i = 0; i < 8; i++)
            SHIFTD_EXPECT(cpu->gr_static[8 + i], initial[i], "precise register/CL state");
        SHIFTD_EXPECT(f.reads, 0, "no operand read before full preflight");
        SHIFTD_EXPECT(f.writes, 0, "no write before full preflight");
        if (locked) {
            SHIFTD_EXPECT(cpu->cr[17] & 0xffff, size == 2 ? 0x200c : 0x100e, "LOCK metadata");
            SHIFTD_EXPECT(cpu->cr[24], 0x0f | (opcode << 8) | (modrm[form] << 16), "invalid opcode bytes");
        } else {
            SHIFTD_EXPECT(cpu->cr[20], crossing ? 0x5000 : address, "faulting byte");
            SHIFTD_EXPECT(cpu->cr[17] & (UINT64_C(3) << 33), UINT64_C(3) << 33, "RMW metadata");
            cpu->dtr[scenario != 2].pte |= 0x461;
            merced_flush_translation_cache(cpu);
            for (unsigned slot = 0; slot < 3; slot++)
                SHIFTD_EXPECT(handoff_step(cpu, fast), MERCED_OK, "native RFI");
            SHIFTD_EXPECT(cpu->ip, 0x1000, "RFI resumes original instruction");
            SHIFTD_EXPECT(handoff_step(cpu, fast), MERCED_OK, "repaired operand succeeds");
        }
    }
    if (!locked) {
        SHIFTD_EXPECT(cpu->ip, 0x1000 + len, "instruction length");
        SHIFTD_EXPECT(cpu->nfaults, fault, "fault count");
        SHIFTD_EXPECT(cpu->ar[24] & flag_mask, wanted_flags & flag_mask, "defined flags");
        for (unsigned i = 0; i < 8; i++) {
            uint64_t checked = !memory && !defined && i == dst ? ~(uint64_t)mask : UINT64_MAX;
            SHIFTD_EXPECT(cpu->gr_static[8 + i] & checked, expected[i] & checked, "register aliases and preserved upper bits");
        }
        if (memory && defined) {
            for (unsigned i = 0; i < size; i++) {
                unsigned off = (address & 0xfff) + i;
                unsigned pa = off < 0x1000 ? 0x5000 + off : 0x7000 + off - 0x1000;
                SHIFTD_EXPECT(rse_test_read(&f.ram, pa, 1), (result >> (8 * i)) & 0xff, "translated destination bytes");
            }
        }
        SHIFTD_EXPECT(f.reads, memory ? (crossing ? size : 1) : 0, "one complete operand read");
        SHIFTD_EXPECT(f.writes, memory && (count & 31) ? (crossing ? size : 1) : 0, "one complete operand write");
    }
    SHIFTD_EXPECT(rse_test_read(&f.ram, 0x6000, 8), UINT64_C(0xa5a5a5a5a5a5a5a5), "physical gap guard");
    if (crossing) SHIFTD_EXPECT(rse_test_read(&f.ram, 0x7000 + size - 1, 1), 0xa5, "operand end guard");
#undef SHIFTD_EXPECT
    merced_destroy(cpu);
}

static void check_actual_comctl_dpi_sequence(bool fast, unsigned initial_top,
                                             int32_t dpi_x, int32_t dpi_y,
                                             uint64_t expected_x, uint64_t expected_y) {
    RseFixture f = {0};
    /* The actual COMCTL32 startup instructions, with only absolute data
     * addresses relocated. The two GetDeviceCaps calls are represented by
     * supplying their integer results in the same SS:[EBP-0x70] local. */
    static const uint8_t code[] = {
        0xdb,0x45,0x90,                         /* 1000 fild [ebp-70] */
        0xdc,0x0d,0x00,0x20,0x00,0x00,          /* 1003 fmul [coefficient] */
        0xdd,0x1d,0x10,0x20,0x00,0x00,          /* 1009 fstp [scale_x] */
        0xdb,0x45,0x90,                         /* 100f fild [ebp-70] */
        0xdc,0x0d,0x00,0x20,0x00,0x00,          /* 1012 fmul [coefficient] */
        0xdd,0x15,0x18,0x20,0x00,0x00,          /* 1018 fst [scale_y] */
        0xdd,0x05,0x10,0x20,0x00,0x00,          /* 101e fld [scale_x] */
        0xdc,0x1d,0x08,0x20,0x00,0x00,          /* 1024 fcomp [one] */
        0xdf,0xe0,                              /* 102a fnstsw ax */
        0xdc,0x1d,0x08,0x20,0x00,0x00,          /* 102c fcomp [one] */
        0xdf,0xe0,                              /* 1032 fnstsw ax */
        0xdd,0xd8                               /* 1034 fstp st(0) */
    };
    memcpy(f.memory + 0x1000, code, sizeof(code));
    rse_test_write(&f, 0x2000, UINT64_C(0x3f85555555555555), 8); /* actual 1/96 */
    rse_test_write(&f, 0x2008, UINT64_C(0x3ff0000000000000), 8);
    rse_test_write(&f, 0x2010, UINT64_C(0xfeedfacefeedface), 8);
    rse_test_write(&f, 0x2018, UINT64_C(0xdeadbeefdeadbeef), 8);
    rse_test_write(&f, 0x2020, UINT64_C(0xa5c35a3ca5c35a3c), 8);
    rse_test_write(&f, 0x4f90, (uint32_t)dpi_x, 4);
    rse_test_write(&f, 0x3f90, UINT32_C(0x7fffffff), 4); /* wrong DS local */
    MercedBus bus = {.ud = &f, .read = rse_test_read, .write = rse_test_write,
                     .ram_size = sizeof(f.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 34) |
               (UINT64_C(1) << 44) | (UINT64_C(1) << 19);
    cpu->gr_static[25] = UINT64_C(1) << 62;
    cpu->gr_static[26] = (UINT64_C(1) << 62) | 0x1000;
    cpu->gr_static[27] = 0;
    cpu->gr_static[13] = 0x4000;
    cpu->gr_static[8] = UINT64_C(0xfeedface12340000);
    cpu->ar[21] = UINT64_C(0x00001f800000027f);
    cpu->ar[28] = UINT64_C(0xffff0000) | ((uint64_t)initial_top << 11);
    cpu->ar[27] = UINT64_C(0x0000060100000101);
    cpu->ar[24] = UINT64_C(0x246);
    cpu->cr[72] = UINT64_C(1) << 16;
#define DPI_EXPECT(actual, expected, what) \
    rse_test_expect((actual), (expected), (what), fast, initial_top, (unsigned)dpi_x, (unsigned)dpi_y, 0)
    for (unsigned i = 0; i < 3; i++)
        DPI_EXPECT(handoff_step(cpu, fast), MERCED_OK, "COMCTL32 X FILD/FMUL/FSTP executes");
    DPI_EXPECT(cpu->ip, 0x100f, "first DPI sequence instruction lengths");
    DPI_EXPECT(rse_test_read(&f, 0x2010, 8), expected_x, "DPI X scale binary64 result");
    DPI_EXPECT((cpu->ar[28] >> 11) & 7, initial_top, "first FSTP balances stack");
    DPI_EXPECT((cpu->ar[28] >> 16) & 0xffff, 0xffff, "first FSTP empties stack");
    rse_test_write(&f, 0x4f90, (uint32_t)dpi_y, 4);
    for (unsigned i = 0; i < 6; i++)
        DPI_EXPECT(handoff_step(cpu, fast), MERCED_OK, "COMCTL32 Y/compare sequence executes");
    DPI_EXPECT(cpu->ip, 0x102c, "second DPI sequence instruction lengths");
    DPI_EXPECT(rse_test_read(&f, 0x2018, 8), expected_y, "DPI Y scale binary64 result");
    DPI_EXPECT((cpu->ar[28] >> 11) & 7, (initial_top + 7) & 7,
               "FCOMP pops X and retains Y");
    uint32_t x_cc = dpi_x < 96 ? 0x100 : dpi_x == 96 ? 0x4000 : 0;
    DPI_EXPECT(cpu->gr_static[8] & 0x4500, x_cc, "FNSTSW exposes X comparison");
    DPI_EXPECT(cpu->gr_static[8] & ~UINT64_C(0xffff), UINT64_C(0xfeedface12340000),
               "FNSTSW AX preserves upper accumulator");
    if (dpi_x <= 96) {
        for (unsigned i = 0; i < 2; i++)
            DPI_EXPECT(handoff_step(cpu, fast), MERCED_OK, "COMCTL32 Y FCOMP/FNSTSW executes");
        uint32_t y_cc = dpi_y < 96 ? 0x100 : dpi_y == 96 ? 0x4000 : 0;
        DPI_EXPECT(cpu->gr_static[8] & 0x4500, y_cc, "FNSTSW exposes Y comparison");
    } else {
        /* The real TEST/Jcc skips the second FCOMP when X already exceeds1. */
        cpu->ip = 0x1034;
        DPI_EXPECT(handoff_step(cpu, fast), MERCED_OK, "COMCTL32 FSTP ST0 cleanup executes");
    }
    DPI_EXPECT((cpu->ar[28] >> 11) & 7, initial_top, "DPI sequence leaves balanced TOP");
    DPI_EXPECT((cpu->ar[28] >> 16) & 0xffff, 0xffff, "DPI sequence leaves all tags empty");
    DPI_EXPECT(cpu->ar[21], UINT64_C(0x00001f800000027f), "DPI sequence preserves FP controls");
    DPI_EXPECT(cpu->ar[24], UINT64_C(0x246), "x87 data instructions preserve EFLAGS");
    DPI_EXPECT(rse_test_read(&f, 0x2000, 8), UINT64_C(0x3f85555555555555), "DPI coefficient unchanged");
    DPI_EXPECT(rse_test_read(&f, 0x2008, 8), UINT64_C(0x3ff0000000000000), "DPI comparison constant unchanged");
    DPI_EXPECT(rse_test_read(&f, 0x2020, 8), UINT64_C(0xa5c35a3ca5c35a3c), "DPI output guard unchanged");
    DPI_EXPECT(cpu->nfaults, 0, "ordinary DPI calculation has no native faults");
#undef DPI_EXPECT
    merced_destroy(cpu);
}

static void check_ia32_fp_dirty_exit(bool fast, unsigned kind, bool dfl,
                                    bool mfl, bool mfh) {
    RseFixture f = {0};
    unsigned entry = 0x1000;
    if (!dfl) {
        const uint8_t fild[] = {0xdb, 0x05, 0x00, 0x40, 0x00, 0x00};
        memcpy(f.memory + entry, fild, sizeof(fild));
        rse_test_write(&f, 0x4000, 3, 4);
        entry += sizeof(fild);
    }
    const uint8_t direct[] = {0x0f, 0xb8, 0x00, 0x20, 0x00, 0x00};
    const uint8_t indirect[] = {0x0f, 0x00, 0xf0};
    const uint8_t fault[] = {0xf0, 0x9b}; /* Invalid LOCK, before DFL check. */
    if (!kind) memcpy(f.memory + entry, direct, sizeof(direct));
    else if (kind == 1) memcpy(f.memory + entry, indirect, sizeof(indirect));
    else if (kind == 2) memcpy(f.memory + entry, fault, sizeof(fault));
    else f.memory[entry] = 0x90;
    const uint64_t nop = UINT64_C(0x08000000);
    unsigned vector = kind == 2 ? 0xea00 : 0xb000;
    rse_test_bundle(&f, vector, 17, nop, nop, UINT64_C(8) << 27);
    MercedBus bus = {.ud = &f, .read = rse_test_read, .write = rse_test_write,
                     .ram_size = sizeof(f.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 34) |
               (UINT64_C(1) << 44) | ((uint64_t)dfl << 18) |
               ((uint64_t)mfl << 4) | ((uint64_t)mfh << 5);
    cpu->gr_static[25] = UINT64_C(1) << 62;
    cpu->gr_static[26] = UINT64_C(1) << 62;
    cpu->gr_static[8] = 0x2000;
    cpu->ar[21] = UINT64_C(0x1f800000037f);
    cpu->ar[28] = UINT64_C(0xffff0000);
    cpu->ar[24] = 0x202;
    cpu->cr[2] = 0x8000;
    cpu->cr[72] = UINT64_C(1) << 16;
#define DIRTY_EXPECT(actual, expected, what) \
    rse_test_expect((actual), (expected), (what), fast, kind, dfl, mfl, mfh)
    if (!dfl) {
        DIRTY_EXPECT(handoff_step(cpu, fast), MERCED_OK, "FP value created before native exit");
        DIRTY_EXPECT(cpu->fr[15].sig, UINT64_C(0xc000000000000000), "FILD exact significand");
        DIRTY_EXPECT(cpu->fr[15].exp, 0x10000, "FILD exact exponent");
    }
    if (kind == 3) {
        cpu->psr |= UINT64_C(1) << 14;
        merced_raise_external(cpu, 0x30);
    }
    DIRTY_EXPECT(handoff_step(cpu, fast), MERCED_OK, "IA32 native transition");
    DIRTY_EXPECT(cpu->ip, kind < 2 ? 0x2000 : vector, "transition destination");
    DIRTY_EXPECT((cpu->psr >> 4) & 1, mfl || !dfl, "native handler has sticky low FP dirty bit");
    DIRTY_EXPECT((cpu->psr >> 5) & 1, mfh, "high FP dirty bit unchanged");
    if (kind >= 2) {
        DIRTY_EXPECT((cpu->cr[16] >> 4) & 1, mfl || !dfl, "saved process retains low FP dirty bit");
        DIRTY_EXPECT((cpu->cr[16] >> 5) & 1, mfh, "saved high FP dirty bit unchanged");
        merced_ack_external(cpu, 0x30);
        for (unsigned slot = 0; slot < 3; slot++)
            DIRTY_EXPECT(handoff_step(cpu, fast), MERCED_OK, "dirty FP context native RFI");
        DIRTY_EXPECT(cpu->ip, entry, "RFI restores interrupted IA32 instruction");
        DIRTY_EXPECT((cpu->psr >> 4) & 1, mfl || !dfl, "RFI retains dirty indication");
        DIRTY_EXPECT((cpu->psr >> 18) & 1, dfl, "RFI restores disabled state");
    }
    if (!dfl) {
        DIRTY_EXPECT(cpu->fr[15].sig, UINT64_C(0xc000000000000000), "FP value survives transition");
        DIRTY_EXPECT(cpu->ar[28], UINT64_C(0xbfff3800), "TOP and physical empty tag survive transition");
    }
#undef DIRTY_EXPECT
    merced_destroy(cpu);
}

/* Private numeric edge fixtures: expected encodings are literal binary80/
 * binary64 values, independent of the implementation's conversion helpers. */
static void check_ia32_x87_numeric_edge(bool fast, unsigned sample) {
    Ia32CmpxchgFixture f = {0};
    const uint64_t empty_tags = UINT64_C(0x55550000);
    uint64_t old_fsr = (UINT64_C(0x15) << 32) | empty_tags | 0x7b00;
    old_fsr &= ~(UINT64_C(1) << 30); /* TOP7 initially contains 3.0 */
    uint64_t wanted_fsr = old_fsr;
    uint16_t cw = 0x037f;
    uint8_t op = 0xdd, mr = 0;
    uint64_t memory = 1, wanted_memory = 1;
    MercedFpReg initial = {UINT64_C(0xc000000000000000),0x10000,0,0};
    MercedFpReg wanted = initial;
    bool push = false, pop = false, changed = false, pending = false;
    bool compare = false, store = false, no_wait = false, check_value = true;
    bool masked = !(sample & 1);
    unsigned target = 7, exceptions = 0, c1 = 0;
    uint64_t conditions = 0x4100;
    if (sample < 6) {
        cw &= masked ? UINT16_MAX : (uint16_t)~2;
        exceptions = 2;
        pending = !masked;
        if (sample < 2) {
            push = changed = true; /* FLD commits even an unmasked DE */
            wanted = (MercedFpReg){UINT64_C(0x8000000000000000),0xfbcd,0,0};
        } else if (sample < 4) {
            op = 0xdc; mr = 0x08;
            changed = masked;
            wanted = masked ? (MercedFpReg){UINT64_C(0xc000000000000000),0xfbce,0,0} : initial;
        } else {
            op = 0xdc; mr = 0x18; compare = true;
            pop = masked; conditions = masked ? 0 : conditions;
        }
    } else if (sample < 16) {
        cw &= masked ? UINT16_MAX : (uint16_t)~1;
        exceptions = 0x41;
        pending = !masked;
        if (sample < 8) {
            op = 0xdb; mr = 0; memory = wanted_memory = 3;
            old_fsr &= ~(UINT64_C(1) << 28); /* full target for FILD push */
            c1 = 0x200; push = changed = masked;
            wanted = (MercedFpReg){UINT64_C(0xc000000000000000),0x1ffff,1,0};
        } else {
            old_fsr |= UINT64_C(1) << 30; /* empty ST0 */
            if (sample < 10) {
                op = 0xdc; mr = 0x08; changed = masked;
                wanted = (MercedFpReg){UINT64_C(0xc000000000000000),0x1ffff,1,0};
            } else if (sample < 12) {
                op = 0xdc; mr = 0x18; compare = true;
                pop = masked; conditions = masked ? 0x4500 : conditions;
            } else if (sample < 14) {
                op = 0xdd; mr = 0x18; store = true; pop = masked;
                memory = UINT64_C(0xdeadbeefdeadbeef);
                wanted_memory = masked ? UINT64_C(0xfff8000000000000) : memory;
            } else {
                op = 0xdd; mr = 0xd8; pop = masked; check_value = false;
            }
        }
    } else if (sample < 19) {
        memory = wanted_memory = sample == 18 ? UINT64_C(0x7ff8000000001234) :
                                                UINT64_C(0x7ff0000000001234);
        if (sample == 17) cw &= ~1u;
        exceptions = sample == 18 ? 0 : 1;
        pending = sample == 17;
        push = changed = !pending;
        wanted = (MercedFpReg){UINT64_C(0xc00000000091a000),0x1ffff,0,0};
    } else if (sample < 23) {
        masked = sample == 19 || sample == 21;
        if (!masked) cw &= ~1u;
        exceptions = 1; pending = !masked;
        op = 0xdc;
        if (sample < 21) {
            mr = 0x08; initial = (MercedFpReg){0,0,0,0};
            memory = wanted_memory = UINT64_C(0x7ff0000000000000);
            changed = masked;
            wanted = masked ? (MercedFpReg){UINT64_C(0xc000000000000000),0x1ffff,1,0} : initial;
        } else {
            mr = 0x18; compare = true; pop = masked;
            memory = wanted_memory = UINT64_C(0x7ff8000000001234);
            conditions = masked ? 0x4500 : conditions;
        }
    } else if (sample < 39) {
        unsigned n = sample - 23, rc = n & 3, sign = (n >> 2) & 1;
        masked = !(n & 8);
        cw = 0x037f | (rc << 10);
        if (!masked) cw &= ~0x20u;
        op = 0xdd; mr = 0x18; store = true; pop = true;
        initial = (MercedFpReg){UINT64_C(0x8000000000000001),0xffff,sign,0};
        wanted = initial;
        memory = UINT64_C(0xdeadbeefdeadbeef);
        bool increment = rc == (sign ? 1 : 2);
        wanted_memory = (sign ? UINT64_C(0xbff0000000000000) : UINT64_C(0x3ff0000000000000)) + increment;
        exceptions = 0x20; c1 = increment ? 0x200 : 0;
        pending = !masked;
    } else if (sample < 45) {
        masked = sample & 1;
        op = 0xdd; mr = 0x18; store = true;
        memory = UINT64_C(0xdeadbeefdeadbeef);
        if (sample < 41) {
            if (!masked) cw &= ~8u;
            initial = (MercedFpReg){UINT64_C(0x8000000000000000),0x103ff,0,0};
            wanted_memory = masked ? UINT64_C(0x7ff0000000000000) : memory;
            exceptions = masked ? 0x28 : 8;
            c1 = masked ? 0x200 : 0;
        } else {
            if (!masked) cw &= ~0x10u;
            initial = (MercedFpReg){sample < 43 ? UINT64_C(0x8000000000000000) : UINT64_C(0xc000000000000000),
                                    sample < 43 ? 0xfbcd : 0xfbcc,0,0};
            wanted_memory = masked ? 1 : memory;
            exceptions = masked ? (sample < 43 ? 0 : 0x30) : 0x10;
            c1 = masked && sample >= 43 ? 0x200 : 0;
        }
        wanted = initial;
        pop = masked; pending = !masked;
    } else if (sample == 45) {
        op = 0xdf; mr = 0xe0; no_wait = true;
        cw &= ~1u;
        old_fsr |= 0x8081;
        exceptions = 1; c1 = 0x200; pending = true;
    } else if (sample == 46) {
        op = 0xdc; mr = 0x08;
        memory = wanted_memory = UINT64_C(0x7ff8000000001234);
        changed = true;
        wanted = (MercedFpReg){UINT64_C(0xc00000000091a000),0x1ffff,0,0};
    } else if (sample == 47) {
        memory = wanted_memory = UINT64_C(0xfff0000000000000);
        push = changed = true;
        wanted = (MercedFpReg){UINT64_C(0x8000000000000000),0x1ffff,1,0};
    } else {
        op = 0xdc; mr = 0x08;
        initial = (MercedFpReg){0,0,1,0};
        memory = wanted_memory = UINT64_C(0xc000000000000000); /* -2 */
        changed = true; wanted = (MercedFpReg){0,0,0,0};
    }
    wanted_fsr = (old_fsr & ~UINT64_C(0x82ff)) | exceptions | c1;
    if (pending) wanted_fsr |= 0x8080;
    if (push) {
        target = 6;
        wanted_fsr = (wanted_fsr & ~UINT64_C(0x3800)) | 0x3000;
        wanted_fsr &= ~(UINT64_C(1) << 28);
    } else if (pop) {
        wanted_fsr &= ~UINT64_C(0x3800);
        wanted_fsr |= UINT64_C(1) << 30;
    } else if (changed) wanted_fsr &= ~(UINT64_C(1) << 30);
    if (compare) wanted_fsr = (wanted_fsr & ~UINT64_C(0x4500)) | conditions;
    f.ram.memory[0x1000] = op; f.ram.memory[0x1001] = mr;
    f.ram.memory[0x1002] = 0xdf; f.ram.memory[0x1003] = 0xe0; /* no-wait status */
    f.ram.memory[0x1004] = 0x9b; /* delayed exception delivery */
    rse_test_write(&f.ram,0x5000,memory,8);
    rse_test_write(&f.ram,0x4ff8,UINT64_C(0xa5a5a5a5a5a5a5a5),8);
    rse_test_write(&f.ram,0x5008,UINT64_C(0xa5a5a5a5a5a5a5a5),8);
    rse_test_bundle(&f.ram,0xe900,0x11,0x08000000,0x08000000,UINT64_C(8)<<27);
    MercedBus bus = {.ud=&f,.read=ia32_cmpxchg_read,.write=ia32_cmpxchg_write,.ram_size=sizeof(f.ram.memory)};
    Merced *cpu=merced_create(&bus);
    if(!cpu){failures++;return;}
    cpu->ip=0x1000;
    cpu->psr=(UINT64_C(1)<<13)|(UINT64_C(1)<<17)|(UINT64_C(1)<<19)|
             (UINT64_C(1)<<34)|(UINT64_C(1)<<44);
    cpu->gr_static[25]=(UINT64_C(1)<<62)|0x800;
    cpu->gr_static[27]=0x1000;
    cpu->gr_static[17]=0x33; cpu->gr_static[16]=0x2b;
    cpu->gr_static[8]=UINT64_C(0x1234567800003000);
    cpu->fr[15]=initial;
    cpu->fr[14]=(MercedFpReg){UINT64_C(0x8000000000000000),0x10002,0,0};
    cpu->ar[21]=(UINT64_C(0x1f80)<<32)|cw;
    cpu->ar[28]=old_fsr;
    const uint64_t old_fir=UINT64_C(0x1234005500000600),old_fdr=UINT64_C(0x004400003333);
    cpu->ar[29]=old_fir;cpu->ar[30]=old_fdr;
    cpu->ar[24]=UINT64_C(0x7654321000000247);
    cpu->cr[2]=0x8000;cpu->cr[72]=UINT64_C(1)<<16;
    cpu->rr[0]=(1<<8)|(12<<2);
    cpu->dtr[0]=(MercedTlbEntry){.va_start=0x4000,.va_end=0x4fff,.pfn_base=0x5000,
        .rid=1,.ps=12,.valid=1,.itir=(1<<8)|(12<<2),.pte=0x5461};
#define X87_EDGE(actual,expected,what) \
    rse_test_expect((actual),(expected),(what),fast,sample,false,false,false)
    X87_EDGE(handoff_step(cpu,fast),MERCED_OK,"x87 numeric edge completes");
    X87_EDGE(cpu->ip,0x1002,"new numeric exception is delayed");
    uint64_t fsr_mask=UINT64_MAX;
    if(!compare && !no_wait) fsr_mask &= ~UINT64_C(0x4500); /* undefined C0/C2/C3 */
    X87_EDGE(cpu->ar[28]&fsr_mask,wanted_fsr&fsr_mask,"numeric TOP/tags/status");
    X87_EDGE(cpu->ar[21],(UINT64_C(0x1f80)<<32)|cw,"preserve FCR and MXCSR");
    X87_EDGE(cpu->ar[24],UINT64_C(0x7654321000000247),"numeric preserves integer flags");
    if (check_value && (changed || !pop)) {
        MercedFpReg actual=cpu->fr[8 + target];
        MercedFpReg expected=changed?wanted:initial;
        X87_EDGE(actual.sig,expected.sig,"binary80 significand");
        X87_EDGE(actual.exp,expected.exp,"binary80 exponent");
        X87_EDGE(actual.sign,expected.sign,"binary80 sign");
    }
    X87_EDGE(rse_test_read(&f.ram,0x5000,8),wanted_memory,"numeric memory result");
    X87_EDGE(f.writes,store && pop,"numeric exception commit store count");
    X87_EDGE(rse_test_read(&f.ram,0x4ff8,8),UINT64_C(0xa5a5a5a5a5a5a5a5),"numeric lower guard");
    X87_EDGE(rse_test_read(&f.ram,0x5008,8),UINT64_C(0xa5a5a5a5a5a5a5a5),"numeric upper guard");
    uint64_t fir=no_wait?old_fir:((uint64_t)(((op&7)<<8)|mr)<<48)|(UINT64_C(0x33)<<32)|0x800;
    uint64_t fdr=(mr>=0xc0)?old_fdr:(UINT64_C(0x2b)<<32)|0x3000;
    X87_EDGE(cpu->ar[29],fir,"numeric FIR opcode/selector/offset");
    if(mr<0xc0 || no_wait) X87_EDGE(cpu->ar[30],fdr,"numeric FDR selector/effective offset");
    X87_EDGE(handoff_step(cpu,fast),MERCED_OK,"FNSTSW executes with pending exception");
    X87_EDGE(cpu->gr_static[8]&0xffff,wanted_fsr&0xffff,"FNSTSW exposes pending status");
    X87_EDGE(cpu->gr_static[8]&~UINT64_C(0xffff),UINT64_C(0x1234567800000000),"FNSTSW preserves high EAX");
    X87_EDGE(cpu->ar[29],fir,"FNSTSW preserves FIR");
    X87_EDGE(handoff_step(cpu,fast),MERCED_OK,"waiting instruction status");
    if(pending) {
        X87_EDGE(cpu->ip,0xe900,"new exception delivered at following FWAIT");
        X87_EDGE(cpu->cr[19],0x1004,"delayed fault IIP");
        X87_EDGE(cpu->cr[17],UINT64_C(16)<<16,"delayed #MF vector");
        X87_EDGE(cpu->ar[29],fir,"delayed fault preserves originating FIR");
        cpu->ar[28]&=~UINT64_C(0x80ff);
        for(unsigned i=0;i<3;i++) X87_EDGE(handoff_step(cpu,fast),MERCED_OK,"numeric recovery RFI");
        X87_EDGE(cpu->ip,0x1004,"numeric recovery replays FWAIT");
        X87_EDGE(handoff_step(cpu,fast),MERCED_OK,"cleared pending condition retries");
    }
    X87_EDGE(cpu->ip,0x1005,"numeric edge sequence finished");
#undef X87_EDGE
    merced_destroy(cpu);
}

static void check_ia32_bswap(bool fast, bool code32, bool prefix, bool locked,
                             unsigned reg) {
    RseFixture f = {0};
    unsigned len = 0;
    if (locked) f.memory[0x1000 + len++] = 0xf0;
    if (prefix) f.memory[0x1000 + len++] = 0x66;
    f.memory[0x1000 + len++] = 0x0f;
    f.memory[0x1000 + len++] = 0xc8 + reg;
    MercedBus bus = {.ud = &f, .read = rse_test_read, .write = rse_test_write,
                     .ram_size = sizeof(f.memory)};
    Merced *cpu = merced_create(&bus);
    if (!cpu) { failures++; return; }
    cpu->ip = 0x1000;
    cpu->psr = (UINT64_C(1) << 13) | (UINT64_C(1) << 34) | (UINT64_C(1) << 44);
    cpu->gr_static[25] = (uint64_t)code32 << 62;
    cpu->ar[24] = UINT64_C(0x9876543200010ed7);
    cpu->cr[2] = 0x8000;
    cpu->cr[72] = UINT64_C(1) << 16;
    for (unsigned r = 0; r < 8; r++)
        cpu->gr_static[8 + r] = UINT64_C(0xfeedface80010203) + ((uint64_t)r << 8);
#define BSWAP_EXPECT(actual, expected, what) \
    rse_test_expect((actual), (expected), (what), fast, reg, code32, prefix, locked)
    BSWAP_EXPECT(handoff_step(cpu, fast), MERCED_OK, "BSWAP status");
    BSWAP_EXPECT(cpu->ip, locked ? 0xea00 : 0x1000 + len, "BSWAP advances or intercepts LOCK");
    for (unsigned r = 0; r < 8; r++) {
        uint64_t before = UINT64_C(0xfeedface80010203) + ((uint64_t)r << 8);
        uint64_t expected = before;
        if (!locked && r == reg) {
            if (code32 != prefix)
                expected = UINT64_C(0xfeedface03000180) | ((uint64_t)(2 + r) << 16);
            else expected &= ~UINT64_C(0xffff); /* deterministic undefined 16-bit policy */
        }
        BSWAP_EXPECT(cpu->gr_static[8 + r], expected, "BSWAP only changes selected operand bits");
    }
    BSWAP_EXPECT(cpu->ar[24], UINT64_C(0x9876543200010ed7), "BSWAP preserves every integer flag");
    if (locked) {
        BSWAP_EXPECT(cpu->cr[19], 0x1000, "invalid LOCK restarts at first prefix");
        BSWAP_EXPECT(cpu->cr[24], 0x0f | ((uint64_t)(0xc8 + reg) << 8), "BSWAP intercept instruction bytes");
        BSWAP_EXPECT(cpu->cr[17] & 0xffff,
                     ((len - 2) << 12) | ((code32 != prefix) << 1) | (code32 << 2) | 8,
                     "BSWAP prefix and size intercept metadata");
    }
#undef BSWAP_EXPECT
    merced_destroy(cpu);
}

/* Each numeric memory operand crosses a virtual page boundary whose PAs are
 * nonadjacent. A failed second-page lookup must precede every FP-state write. */
static void check_ia32_x87_numeric_fault(bool fast, unsigned kind) {
    Ia32CmpxchgFixture f={0};
    const uint8_t ops[]={0xdb,0xdd,0xdc,0xdc,0xdd,0xdd,0xdd};
    const uint8_t modrm[]={0x07,0x07,0x0f,0x1f,0x1f,0x17,0x1f};
    bool store=kind>=4, overflow=kind==6, push=kind<2, pop=kind==3||kind==4;
    unsigned size=kind==0?4:8,vector=store?0xd300:0xd000;
    f.ram.memory[0x1000]=ops[kind];f.ram.memory[0x1001]=modrm[kind];
    uint64_t operand=kind==0?3:UINT64_C(0x4000000000000000);
    if(store)operand=UINT64_C(0xdeadbeefdeadbeef);
    memset(f.ram.memory+0x6000,0xa5,16);
    memset(f.ram.memory+0x7000,0xa5,16);
    rse_test_write(&f.ram,0x5ffe,0xa5,1);
    for(unsigned i=0;i<size;i++)rse_test_write(&f.ram,i?0x6fff+i:0x5fff,operand>>(8*i),1);
    rse_test_bundle(&f.ram,vector,0x11,0x08000000,0x08000000,UINT64_C(8)<<27);
    MercedBus bus={.ud=&f,.read=ia32_cmpxchg_read,.write=ia32_cmpxchg_write,.ram_size=sizeof(f.ram.memory)};
    Merced *cpu=merced_create(&bus);if(!cpu){failures++;return;}
    cpu->ip=0x1000;
    cpu->psr=(UINT64_C(1)<<13)|(UINT64_C(1)<<17)|(UINT64_C(1)<<34)|(UINT64_C(1)<<44);
    cpu->gr_static[25]=UINT64_C(1)<<62;cpu->gr_static[15]=0x4fff;
    const uint64_t fcr=(UINT64_C(0x1f80)<<32)|(overflow?0x377:0x37f);
    const uint64_t fsr=UINT64_C(0x1555557b00)&~(UINT64_C(1)<<30);
    const uint64_t fir=UINT64_C(0x1234005500000600),fdr=UINT64_C(0x004400003333);
    cpu->ar[21]=fcr;cpu->ar[28]=fsr;cpu->ar[29]=fir;cpu->ar[30]=fdr;
    cpu->ar[24]=0x247;
    MercedFpReg original={overflow?UINT64_C(0x8000000000000000):UINT64_C(0xc000000000000000),
                           overflow?0x103ff:0x10000,0,0};
    cpu->fr[15]=original;
    cpu->cr[2]=0x8000;cpu->cr[72]=UINT64_C(1)<<16;cpu->rr[0]=(1<<8)|(12<<2);
    for(unsigned i=0;i<2;i++) {
        uint64_t va=0x4000+0x1000*i,pa=0x5000+0x2000*i;
        cpu->dtr[i]=(MercedTlbEntry){.va_start=va,.va_end=va+0xfff,.pfn_base=pa,
            .rid=1,.ps=12,.valid=1,.itir=(1<<8)|(12<<2),.pte=pa|0x461};
    }
    cpu->dtr[1].pte&=~(store?UINT64_C(0x600):UINT64_C(1));
#define X87_FAULT(a,e,w) rse_test_expect((a),(e),(w),fast,kind,false,false,true)
    X87_FAULT(handoff_step(cpu,fast),MERCED_OK,"numeric operand fault status");
    X87_FAULT(cpu->ip,vector,"numeric operand fault vector");
    X87_FAULT(cpu->cr[19],0x1000,"numeric fault original instruction");
    X87_FAULT(cpu->cr[20],0x5000,"numeric second-page byte");
    X87_FAULT(f.reads,0,"numeric complete preflight before operand read");
    X87_FAULT(f.writes,0,"numeric complete preflight before operand write");
    X87_FAULT(cpu->ar[21],fcr,"fault preserves numeric controls");
    X87_FAULT(cpu->ar[28],fsr,"fault preserves numeric stack/status/tags");
    X87_FAULT(cpu->ar[29],fir,"fault preserves previous numeric FIR");
    X87_FAULT(cpu->ar[30],fdr,"fault preserves previous numeric FDR");
    X87_FAULT(cpu->fr[15].sig,original.sig,"fault preserves numeric value");
    X87_FAULT(cpu->fr[15].exp,original.exp,"fault preserves numeric exponent");
    cpu->dtr[1].pte|=0x461;merced_flush_translation_cache(cpu);
    for(unsigned i=0;i<3;i++)X87_FAULT(handoff_step(cpu,fast),MERCED_OK,"numeric operand repair RFI");
    X87_FAULT(cpu->ip,0x1000,"numeric operand repair replays once");
    X87_FAULT(handoff_step(cpu,fast),MERCED_OK,"repaired numeric operand completes");
    X87_FAULT(cpu->ip,0x1002,"repaired numeric next instruction");
    X87_FAULT(cpu->nfaults,1,"single recoverable numeric fault");
    X87_FAULT((cpu->ar[28]>>11)&7,push?6:pop?0:7,"numeric stack commits once");
    X87_FAULT(cpu->ar[28]&0x8080,overflow?0x8080:0,"numeric pending after memory repair");
    if(push||kind==2) {
        unsigned fr=push?14:15;
        X87_FAULT(cpu->fr[fr].sig,kind==1?UINT64_C(0x8000000000000000):UINT64_C(0xc000000000000000),"repaired numeric significand");
        X87_FAULT(cpu->fr[fr].exp,kind==2?0x10001:0x10000,"repaired numeric exponent");
    }
    uint64_t expected=store&&!overflow?UINT64_C(0x4008000000000000):operand;
    for(unsigned i=0;i<size;i++)X87_FAULT(rse_test_read(&f.ram,i?0x6fff+i:0x5fff,1),(expected>>(8*i))&0xff,"numeric split destination bytes");
    X87_FAULT(f.reads,store?0:size,"numeric operand read exactly once");
    X87_FAULT(f.writes,store&&!overflow?size:0,"numeric store or suppression exactly once");
    X87_FAULT(rse_test_read(&f.ram,0x6000,8),UINT64_C(0xa5a5a5a5a5a5a5a5),"numeric physical gap guard");
    X87_FAULT(rse_test_read(&f.ram,0x5ffe,1),0xa5,"numeric preceding guard");
    X87_FAULT(rse_test_read(&f.ram,0x7000+size-1,1),0xa5,"numeric ending guard");
#undef X87_FAULT
    merced_destroy(cpu);
}

/* A register FSTP transfers binary80 encodings without numeric conversion.
 * Native x87 confirms SNaN, unnormal and pseudozero remain bit-identical,
 * including when IE is unmasked. The old ST0 is then marked empty. */
static void check_ia32_x87_register_copy(bool fast, unsigned sample,
                                        bool alias, bool masked) {
    RseFixture f={0};
    const MercedFpReg values[]={
        {UINT64_C(0x8000000000001234),0x1ffff,0,0},
        {UINT64_C(0x4000000000000000),0x10000,0,0},
        {0,0xffff,0,0},
    };
    f.memory[0x1000]=0xdd;f.memory[0x1001]=alias?0xd8:0xd9;
    f.memory[0x1002]=0x9b;
    MercedBus bus={.ud=&f,.read=rse_test_read,.write=rse_test_write,.ram_size=sizeof(f.memory)};
    Merced *cpu=merced_create(&bus);if(!cpu){failures++;return;}
    cpu->ip=0x1000;
    cpu->psr=(UINT64_C(1)<<13)|(UINT64_C(1)<<19)|(UINT64_C(1)<<34)|(UINT64_C(1)<<44);
    cpu->gr_static[25]=UINT64_C(1)<<62;
    cpu->fr[14]=values[sample];
    cpu->fr[15]=(MercedFpReg){UINT64_C(0x8000000000000000),0xffff,0,0};
    cpu->ar[21]=(UINT64_C(0x1f80)<<32)|(masked?0x37f:0x37e);
    uint64_t tags=UINT64_C(0x55550000)&~(UINT64_C(1)<<28)&~(UINT64_C(1)<<30);
    cpu->ar[28]=tags|0x3200; /* TOP6, C1 initially set */
    cpu->ar[24]=UINT64_C(0x3456789000000247);
    cpu->cr[2]=0x8000;cpu->cr[72]=UINT64_C(1)<<16;
#define X87_COPY(a,e,w) rse_test_expect((a),(e),(w),fast,sample,alias,masked,false)
    X87_COPY(handoff_step(cpu,fast),MERCED_OK,"raw FSTP status");
    X87_COPY(cpu->ip,0x1002,"raw FSTP completes");
    X87_COPY(cpu->ar[28]&~UINT64_C(0x4500),(tags|(UINT64_C(1)<<28)|0x3800),"raw FSTP tags/TOP/status");
    if(!alias) {
        X87_COPY(cpu->fr[15].sig,values[sample].sig,"raw FSTP significand");
        X87_COPY(cpu->fr[15].exp,values[sample].exp,"raw FSTP exponent");
        X87_COPY(cpu->fr[15].sign,values[sample].sign,"raw FSTP sign");
    } else {
        /* The popped source is empty; do not specify its inactive contents. */
        X87_COPY(cpu->fr[15].sig,UINT64_C(0x8000000000000000),"FSTP ST0 preserves other register");
        X87_COPY(cpu->fr[15].exp,0xffff,"FSTP ST0 preserves other exponent");
    }
    X87_COPY(cpu->ar[24],UINT64_C(0x3456789000000247),"raw FSTP preserves integer flags");
    X87_COPY(handoff_step(cpu,fast),MERCED_OK,"raw FSTP has no pending #MF");
    X87_COPY(cpu->ip,0x1003,"raw FSTP followed by FWAIT succeeds");
    X87_COPY(cpu->nfaults,0,"raw FSTP no exception even for SNaN");
#undef X87_COPY
    merced_destroy(cpu);
}

/* Architectural assertions cover index/ZF and untouched state. Additional
 * assertions label the chosen policy for architecturally undefined results. */
static void check_ia32_bit_scan(bool fast, unsigned size, bool reverse,
                                unsigned sample, unsigned form, unsigned scenario) {
    Ia32CmpxchgFixture f={0};
    bool memory=form>=2, crossing=scenario==1||scenario==2;
    bool fault=scenario==2, locked=scenario==3;
    unsigned width=size*8,address=crossing?0x4fff:0x4000;
    uint32_t value=sample==0?0:sample<=width?UINT32_C(1)<<(sample-1):
                   sample==width+1?UINT32_MAX:0xa5008100;
    uint32_t mask=size==4?UINT32_MAX:0xffff;
    value&=mask;
    const uint8_t mods[]={0xc2,0xc0,0x07,0x3f};
    unsigned dst=(mods[form]>>3)&7,src=mods[form]&7,len=0;
    if(locked)f.ram.memory[0x1000+len++]=0xf0;
    if(size==2)f.ram.memory[0x1000+len++]=0x66;
    f.ram.memory[0x1000+len++]=0x0f;
    f.ram.memory[0x1000+len++]=reverse?0xbd:0xbc;
    f.ram.memory[0x1000+len++]=mods[form];
    uint64_t regs[8],expected[8];
    for(unsigned r=0;r<8;r++)regs[r]=UINT64_C(0x12345678a0b01230)|r;
    if(memory)regs[7]=UINT64_C(0x9876543200000000)|address;
    else ia32_test_set_register(regs,src,size,value);
    memcpy(expected,regs,sizeof(regs));
    unsigned index=0;bool found=false;
    for(unsigned b=0;b<width;b++) if(value&(UINT32_C(1)<<b)) {
        if(reverse||!found)index=b;
        found=true;
    }
    if(value)ia32_test_set_register(expected,dst,size,index);
    memset(f.ram.memory+0x6000,0xa5,8);
    for(unsigned i=0;i<size;i++) {
        unsigned off=(address&0xfff)+i,pa=off<0x1000?0x5000+off:0x7000+off-0x1000;
        rse_test_write(&f.ram,pa,value>>(8*i),1);
    }
    rse_test_bundle(&f.ram,0xd000,0x11,0x08000000,0x08000000,UINT64_C(8)<<27);
    MercedBus bus={.ud=&f,.read=ia32_cmpxchg_read,.write=ia32_cmpxchg_write,.ram_size=sizeof(f.ram.memory)};
    Merced *cpu=merced_create(&bus);if(!cpu){failures++;return;}
    cpu->ip=0x1000;
    cpu->psr=(UINT64_C(1)<<13)|(UINT64_C(1)<<17)|(UINT64_C(1)<<34)|(UINT64_C(1)<<44);
    cpu->gr_static[25]=UINT64_C(1)<<62;
    memcpy(cpu->gr_static+8,regs,sizeof(regs));
    uint64_t flags=UINT64_C(0x1234567800200e97)|(sample&1?0x40:0);
    cpu->ar[24]=flags;cpu->cr[2]=0x8000;cpu->cr[72]=UINT64_C(1)<<16;
    cpu->rr[0]=(1<<8)|(12<<2);
    for(unsigned i=0;i<2;i++) {
        uint64_t va=0x4000+0x1000*i,pa=0x5000+0x2000*i;
        cpu->dtr[i]=(MercedTlbEntry){.va_start=va,.va_end=va+0xfff,.pfn_base=pa,
            .rid=1,.ps=12,.valid=1,.itir=(1<<8)|(12<<2),.pte=pa|0x61}; /* read-only */
        if((fault&&i)||locked)cpu->dtr[i].pte&=~UINT64_C(1);
    }
#define BIT_SCAN(a,e,w) do { \
    uint64_t av=(a),ev=(e);if(av!=ev) { \
        fprintf(stderr,"IA32 bit-scan %s size=%u reverse=%u sample=%u form=%u scenario=%u: %s got=%" PRIx64 " want=%" PRIx64 "\n", \
                fast?"fast":"slow",size,reverse,sample,form,scenario,w,av,ev);failures++; \
    } \
} while(0)
    BIT_SCAN(handoff_step(cpu,fast),MERCED_OK,"bit scan status");
    if(fault||locked) {
        BIT_SCAN(cpu->ip,locked?0xea00:0xd000,"bit scan fault vector");
        BIT_SCAN(cpu->cr[19],0x1000,"bit scan fault original prefix");
        BIT_SCAN(cpu->ar[24],flags,"bit scan fault preserves flags");
        for(unsigned r=0;r<8;r++)BIT_SCAN(cpu->gr_static[8+r],regs[r],"bit scan fault preserves registers");
        BIT_SCAN(f.reads,0,"bit scan validates full operand before read");
        if(fault) {
            BIT_SCAN(cpu->cr[20],0x5000,"bit scan absent second-page byte");
            cpu->dtr[1].pte|=1;merced_flush_translation_cache(cpu);
            for(unsigned s=0;s<3;s++)BIT_SCAN(handoff_step(cpu,fast),MERCED_OK,"bit scan RFI");
            BIT_SCAN(cpu->ip,0x1000,"bit scan resumes original instruction");
            BIT_SCAN(handoff_step(cpu,fast),MERCED_OK,"bit scan repaired operand");
        } else {
            BIT_SCAN(cpu->cr[24],0x0f|((reverse?0xbd:0xbc)<<8)|(mods[form]<<16),"LOCK bit scan bytes");
            BIT_SCAN(cpu->cr[17]&0xffff,size==2?0x200c:0x100e,"LOCK bit scan prefixes");
        }
    }
    if(!locked) {
        BIT_SCAN(cpu->ip,0x1000+len,"bit scan instruction length");
        for(unsigned r=0;r<8;r++)BIT_SCAN(cpu->gr_static[8+r],expected[r],value?"bit index and alias ordering":"zero destination retained policy");
        uint64_t wanted=(flags&~UINT64_C(0x40))|(value?0:0x40);
        BIT_SCAN(cpu->ar[24]&~UINT64_C(0x895),wanted&~UINT64_C(0x895),"bit scan defined flags");
        BIT_SCAN(cpu->ar[24]&0x895,flags&0x895,"undefined arithmetic flags retained policy");
        BIT_SCAN(f.reads,memory?(crossing?size:1):0,"bit scan operand read count");
        BIT_SCAN(cpu->nfaults,fault,"bit scan fault count");
    }
    BIT_SCAN(f.writes,0,"bit scan never writes source memory");
    BIT_SCAN(rse_test_read(&f.ram,0x6000,8),UINT64_C(0xa5a5a5a5a5a5a5a5),"bit scan physical gap guard");
#undef BIT_SCAN
    merced_destroy(cpu);
}


#if defined(__x86_64__) && defined(__linux__)
#include <sys/mman.h>
/* Differential x87 check: run one instruction on the host FPU and on the
 * emulated IA-32 engine from identical state, inside
 *   fninit; fldcw [edi]; fld tbyte [edi+10h]; fld tbyte [edi+20h]; fnclex;
 *   <insn [esi]>; fnsave [edi+40h]
 * and compare the saved control/status/tag words, all eight registers and
 * the memory operand.  The bytes decode identically in 64-bit and 32-bit
 * mode because they only address through (r/e)di and (r/e)si. */
typedef struct { uint8_t insn[3]; unsigned len; unsigned mem; bool cc; } X87DiffOp;

static uint64_t x87_diff_rng = UINT64_C(0x9e3779b97f4a7c15);
static uint64_t x87_diff_rand(void) {
    x87_diff_rng ^= x87_diff_rng << 13;
    x87_diff_rng ^= x87_diff_rng >> 7;
    x87_diff_rng ^= x87_diff_rng << 17;
    return x87_diff_rng;
}
/* A binary80 image: mostly ordinary values near 1, some extremes. */
static void x87_diff_value(uint8_t *p, int center) {
    uint64_t r = x87_diff_rand(), sig = x87_diff_rand() | (UINT64_C(1) << 63);
    unsigned kind = r % 32, exp;
    if (kind < 20) exp = (unsigned)(0x3fff + center + (int)((r >> 8) % 129) - 64);
    else if (kind < 22) exp = 0x7ffe - (unsigned)((r >> 8) % 4);
    else if (kind < 24) exp = 1 + (unsigned)((r >> 8) % 4);
    else if (kind < 25) { exp = 0; sig = 0; }
    else if (kind < 26) { exp = 0x7fff; sig = UINT64_C(1) << 63; }
    else if (kind < 27) { exp = 0x7fff; sig |= UINT64_C(1) << 62; }
    else if (kind < 28) { exp = 0; sig >>= 1 + (r >> 8) % 70; }
    else if (kind < 30) { exp = 0x3fff + (unsigned)((r >> 8) % 70); sig >>= ((r >> 16) % 3) ? 0 : 0; }
    else { exp = 0x3fff + (unsigned)((r >> 8) % 8); sig &= ~UINT64_C(0xffffffff); }
    if (exp && exp < 0x7fff && kind >= 28) sig = (sig >> ((r >> 20) % 50)) << ((r >> 20) % 50);
    for (unsigned i = 0; i < 8; i++) p[i] = (uint8_t)(sig >> (8 * i));
    unsigned se = exp | (unsigned)((r >> 40) & 1) << 15;
    p[8] = (uint8_t)se; p[9] = (uint8_t)(se >> 8);
}
static void x87_diff_memory(uint8_t *p, const X87DiffOp *op) {
    uint64_t r = x87_diff_rand(), v = x87_diff_rand();
    unsigned kind = r % 8;
    if (op->mem == 4 && (op->insn[0] == 0xd8 || op->insn[0] == 0xd9)) {
        uint32_t s = (uint32_t)v;
        if (kind < 5) s = (s & 0x807fffff) | (uint32_t)(127 + (int)((r >> 8) % 60) - 30) << 23;
        else if (kind == 5) s &= 0x807fffff;
        memcpy(p, &s, 4);
    } else if (op->mem == 8 && (op->insn[0] == 0xdc || op->insn[0] == 0xdd)) {
        if (kind < 5) v = (v & UINT64_C(0x800fffffffffffff)) |
                          (uint64_t)(1023 + (int)((r >> 8) % 120) - 60) << 52;
        else if (kind == 5) v &= UINT64_C(0x800fffffffffffff);
        memcpy(p, &v, 8);
    } else if (op->mem == 10) {
        x87_diff_value(p, 0);
    } else {
        if (kind < 4) v = (uint64_t)(int64_t)((int)(v % 2001) - 1000);
        memcpy(p, &v, 8);
    }
}
static void check_ia32_x87_differential(bool fast) {
    static const X87DiffOp ops[] = {
#define M(a, b, n) {{a, b}, 2, n, false}
#define MC(a, b, n) {{a, b}, 2, n, true}
#define R(a, b) {{a, b}, 2, 0, false}
#define RC(a, b) {{a, b}, 2, 0, true}
        M(0xd8,0x06,4), M(0xd8,0x0e,4), MC(0xd8,0x16,4), MC(0xd8,0x1e,4),
        M(0xd8,0x26,4), M(0xd8,0x2e,4), M(0xd8,0x36,4), M(0xd8,0x3e,4),
        M(0xdc,0x06,8), M(0xdc,0x0e,8), MC(0xdc,0x16,8), MC(0xdc,0x1e,8),
        M(0xdc,0x26,8), M(0xdc,0x2e,8), M(0xdc,0x36,8), M(0xdc,0x3e,8),
        M(0xda,0x06,4), M(0xda,0x0e,4), MC(0xda,0x16,4), M(0xda,0x26,4), M(0xda,0x3e,4),
        M(0xde,0x06,2), M(0xde,0x0e,2), MC(0xde,0x1e,2), M(0xde,0x2e,2), M(0xde,0x36,2),
        R(0xd8,0xc1), R(0xd8,0xc9), RC(0xd8,0xd1), RC(0xd8,0xd9), R(0xd8,0xe1),
        R(0xd8,0xe9), R(0xd8,0xf1), R(0xd8,0xf9),
        R(0xdc,0xc1), R(0xdc,0xc9), R(0xdc,0xe1), R(0xdc,0xe9), R(0xdc,0xf1), R(0xdc,0xf9),
        R(0xde,0xc1), R(0xde,0xc9), RC(0xde,0xd9), R(0xde,0xe1), R(0xde,0xe9),
        R(0xde,0xf1), R(0xde,0xf9),
        RC(0xda,0xe9), RC(0xdd,0xe1), RC(0xdd,0xe9), RC(0xd9,0xe4), RC(0xd9,0xe5),
        R(0xd9,0xc1), R(0xd9,0xc9), R(0xdd,0xd9), R(0xdd,0xd1), R(0xdd,0xc1),
        R(0xd9,0xe0), R(0xd9,0xe1), R(0xd9,0xe8), R(0xd9,0xe9), R(0xd9,0xea),
        R(0xd9,0xeb), R(0xd9,0xec), R(0xd9,0xed), R(0xd9,0xee), R(0xd9,0xf4),
        RC(0xd9,0xf5), RC(0xd9,0xf8), R(0xd9,0xfa), R(0xd9,0xfc), R(0xd9,0xfd),
        R(0xd9,0xf6), R(0xd9,0xf7), R(0xd9,0xd0),
        M(0xd9,0x06,4), M(0xdd,0x06,8), M(0xdb,0x06,4), M(0xdf,0x06,2),
        M(0xdf,0x2e,8), M(0xdb,0x2e,10),
        M(0xd9,0x16,4), M(0xd9,0x1e,4), M(0xdd,0x16,8), M(0xdd,0x1e,8),
        M(0xdb,0x16,4), M(0xdb,0x1e,4), M(0xdf,0x16,2), M(0xdf,0x1e,2),
        M(0xdf,0x3e,8), M(0xdb,0x3e,10),
#undef M
#undef MC
#undef R
#undef RC
    };
    uint8_t *host = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (host == MAP_FAILED) { failures++; return; }
    static const uint8_t pre[] = {0xdb,0xe3, 0xd9,0x2f, 0xdb,0x6f,0x10,
                                  0xdb,0x6f,0x20, 0xdb,0xe2};
    static const uint8_t post[] = {0xdd,0x77,0x40};
    unsigned mismatches = 0, cases = 0;
    for (unsigned o = 0; o < sizeof(ops) / sizeof(ops[0]); o++) {
        const X87DiffOp *op = &ops[o];
        uint8_t code[32];
        unsigned n = 0;
        memcpy(code, pre, sizeof(pre)); n = sizeof(pre);
        memcpy(code + n, op->insn, op->len); n += op->len;
        memcpy(code + n, post, sizeof(post)); n += sizeof(post);
        memcpy(host, code, n);
        host[n] = 0xc3;
        void (*thunk)(void *, void *) = (void (*)(void *, void *))(void *)host;
        bool reported = false;
        for (unsigned t = 0; t < 400; t++) {
            uint8_t state[0xb0] = {0}, mem[16] = {0}, hstate[0xb0], hmem[16];
            uint64_t r = x87_diff_rand();
            uint16_t cw = 0x007f | (uint16_t)(((r >> 2) & 3) << 10) |
                          (uint16_t)((r & 1) ? 0x300 : ((r >> 4) & 1) ? 0x200 : 0);
            state[0] = (uint8_t)cw; state[1] = (uint8_t)(cw >> 8);
            x87_diff_value(state + 0x10, 0);
            x87_diff_value(state + 0x20, (r >> 5) & 1 ? (int)((r >> 6) % 40) : 0);
            x87_diff_memory(mem, op);
            memcpy(hstate, state, sizeof(state));
            memcpy(hmem, mem, sizeof(mem));
            thunk(hstate, hmem);

            Ia32CmpxchgFixture f = {0};
            memcpy(f.ram.memory + 0x1000, code, n);
            memcpy(f.ram.memory + 0x5000, state, sizeof(state));
            memcpy(f.ram.memory + 0x5100, mem, sizeof(mem));
            MercedBus bus = {.ud=&f,.read=ia32_cmpxchg_read,.write=ia32_cmpxchg_write,
                             .ram_size=sizeof(f.ram.memory)};
            Merced *cpu = merced_create(&bus);
            if (!cpu) { failures++; break; }
            cpu->ip = 0x1000;
            cpu->psr = (UINT64_C(1)<<13)|(UINT64_C(1)<<17)|(UINT64_C(1)<<19)|
                       (UINT64_C(1)<<34)|(UINT64_C(1)<<44);
            cpu->gr_static[25] = (UINT64_C(1)<<62)|0x800;
            cpu->gr_static[27] = 0x1000;
            cpu->gr_static[17] = 0x33; cpu->gr_static[16] = 0x2b;
            cpu->gr_static[14] = 0x3100;  /* ESI */
            cpu->gr_static[15] = 0x3000;  /* EDI */
            cpu->ar[21] = (UINT64_C(0x1f80)<<32)|0x037f;
            cpu->ar[28] = UINT64_C(0x55550000);
            cpu->ar[24] = UINT64_C(0x246);
            cpu->cr[2] = 0x8000; cpu->cr[72] = UINT64_C(1)<<16;
            cpu->rr[0] = (1<<8)|(12<<2);
            cpu->dtr[0] = (MercedTlbEntry){.va_start=0x4000,.va_end=0x4fff,.pfn_base=0x5000,
                .rid=1,.ps=12,.valid=1,.itir=(1<<8)|(12<<2),.pte=0x5461};
            for (unsigned s = 0; s < 8 && cpu->ip != 0x1000 + n; s++)
                if (handoff_step(cpu, fast) != MERCED_OK) break;
            const uint8_t *es = f.ram.memory + 0x5000, *em = f.ram.memory + 0x5100;
            cases++;
            bool host_c2 = hstate[0x45] & 4;
            if ((op->insn[1] == 0xf5 || op->insn[1] == 0xf8) && op->insn[0] == 0xd9 && host_c2)
                continue; /* hardware partial remainder; ours reduces fully */
            uint16_t sw_mask = op->cc ? 0xffff : (uint16_t)~0x4500;
            uint16_t hsw = (uint16_t)(hstate[0x44] | hstate[0x45] << 8);
            uint16_t esw = (uint16_t)(es[0x44] | es[0x45] << 8);
            bool same = cpu->ip == 0x1000 + n &&
                        !memcmp(hstate + 0x40, es + 0x40, 2) &&
                        (hsw & sw_mask) == (esw & sw_mask) &&
                        !memcmp(hstate + 0x48, es + 0x48, 2) &&
                        !memcmp(hmem, em, sizeof(mem));
            /* Empty registers keep stale host contents; compare live ones. */
            unsigned htop = (hsw >> 11) & 7;
            for (unsigned j = 0; j < 8 && same; j++) {
                unsigned phys = (htop + j) & 7;
                unsigned tag = ((hstate[0x48] | hstate[0x49] << 8) >> (2 * phys)) & 3;
                if (tag != 3 && memcmp(hstate + 0x5c + 10 * j, es + 0x5c + 10 * j, 10))
                    same = false;
            }
            if (!same) {
                mismatches++;
                if (!reported) {
                    reported = true;
                    fprintf(stderr, "x87 diff %02X %02X cw=%04X ip=%" PRIx64 " sw host=%04X emu=%04X\n",
                            op->insn[0], op->insn[1], cw, cpu->ip, hsw, esw);
                    fprintf(stderr, "  in  a=");
                    for (int k = 9; k >= 0; k--) fprintf(stderr, "%02X", state[0x20 + k]);
                    fprintf(stderr, " b=");
                    for (int k = 9; k >= 0; k--) fprintf(stderr, "%02X", state[0x10 + k]);
                    fprintf(stderr, " mem=");
                    for (int k = 9; k >= 0; k--) fprintf(stderr, "%02X", mem[k]);
                    fprintf(stderr, "\n  tags host=%02X%02X emu=%02X%02X\n",
                            hstate[0x49], hstate[0x48], es[0x49], es[0x48]);
                    for (unsigned j = 0; j < 2; j++) {
                        fprintf(stderr, "  ST%u host=", j);
                        for (int k = 9; k >= 0; k--) fprintf(stderr, "%02X", hstate[0x5c + 10 * j + k]);
                        fprintf(stderr, " emu=");
                        for (int k = 9; k >= 0; k--) fprintf(stderr, "%02X", es[0x5c + 10 * j + k]);
                        fprintf(stderr, "\n");
                    }
                    fprintf(stderr, "  mem host=");
                    for (int k = 9; k >= 0; k--) fprintf(stderr, "%02X", hmem[k]);
                    fprintf(stderr, " emu=");
                    for (int k = 9; k >= 0; k--) fprintf(stderr, "%02X", em[k]);
                    fprintf(stderr, "\n");
                }
            }
            merced_destroy(cpu);
        }
    }
    munmap(host, 4096);
    if (mismatches) {
        fprintf(stderr, "x87 differential (%s): %u of %u cases differ from host FPU\n",
                fast ? "fast" : "slow", mismatches, cases);
        failures++;
    } else printf("Merced x87 differential (%s): %u cases match the host FPU\n",
                  fast ? "fast" : "slow", cases);
}
#endif

int main(void) {
#if defined(__x86_64__) && defined(__linux__)
    for (unsigned fast = 0; fast < 2; fast++)
        check_ia32_x87_differential(fast);
#endif
    for (unsigned fast=0;fast<2;fast++)
        for (unsigned size=2;size<=4;size+=2)
            for (unsigned reverse=0;reverse<2;reverse++) {
                for (unsigned sample=0;sample<size*8+3;sample++)
                    for (unsigned form=0;form<4;form++)
                        check_ia32_bit_scan(fast,size,reverse,sample,form,0);
                for (unsigned scenario=1;scenario<=2;scenario++)
                    for (unsigned form=2;form<4;form++) {
                        check_ia32_bit_scan(fast,size,reverse,0,form,scenario);
                        check_ia32_bit_scan(fast,size,reverse,size*8,form,scenario);
                    }
                check_ia32_bit_scan(fast,size,reverse,1,0,3);
                check_ia32_bit_scan(fast,size,reverse,1,2,3);
            }

    for (unsigned fast = 0; fast < 2; fast++)
        for (unsigned sample = 0; sample < 3; sample++)
            for (unsigned alias = 0; alias < 2; alias++)
                for (unsigned masked = 0; masked < 2; masked++)
                    check_ia32_x87_register_copy(fast, sample, alias, masked);

    for (unsigned fast = 0; fast < 2; fast++)
        for (unsigned kind = 0; kind < 7; kind++)
            check_ia32_x87_numeric_fault(fast, kind);

    for (unsigned fast = 0; fast < 2; fast++)
        for (unsigned code32 = 0; code32 < 2; code32++)
            for (unsigned prefix = 0; prefix < 2; prefix++)
                for (unsigned locked = 0; locked < 2; locked++)
                    for (unsigned reg = 0; reg < 8; reg++)
                        check_ia32_bswap(fast, code32, prefix, locked, reg);
    for (unsigned fast = 0; fast < 2; fast++)
        for (unsigned sample = 0; sample < 49; sample++)
            check_ia32_x87_numeric_edge(fast, sample);

    for (unsigned fast = 0; fast < 2; fast++)
        for (unsigned kind = 0; kind < 4; kind++)
            for (unsigned dfl = 0; dfl < 2; dfl++)
                for (unsigned mfl = 0; mfl < 2; mfl++)
                    for (unsigned mfh = 0; mfh < 2; mfh++)
                        check_ia32_fp_dirty_exit(fast, kind, dfl, mfl, mfh);
    for (unsigned fast = 0; fast < 2; fast++)
        for (unsigned top = 0; top < 8; top++) {
            check_actual_comctl_dpi_sequence(fast, top, 96, 96,
                                            UINT64_C(0x3ff0000000000000), UINT64_C(0x3ff0000000000000));
            check_actual_comctl_dpi_sequence(fast, top, 72, 144,
                                            UINT64_C(0x3fe8000000000000), UINT64_C(0x3ff8000000000000));
            check_actual_comctl_dpi_sequence(fast, top, 144, 72,
                                            UINT64_C(0x3ff8000000000000), UINT64_C(0x3fe8000000000000));
            check_actual_comctl_dpi_sequence(fast, top, 192, 192,
                                            UINT64_C(0x4000000000000000), UINT64_C(0x4000000000000000));
            check_actual_comctl_dpi_sequence(fast, top, 0, -96,
                                            UINT64_C(0), UINT64_C(0xbff0000000000000));
        }

    for (unsigned fast = 0; fast < 2; fast++) {
        static const unsigned opcodes[] = {0xa4,0xa5,0xac,0xad};
        static const unsigned counts[] = {0,1,2,15,16,17,24,31,32,33,255};
        for (unsigned size = 2; size <= 4; size += 2) {
            for (unsigned op = 0; op < 4; op++) {
                for (unsigned ci = 0; ci < sizeof(counts) / sizeof(counts[0]); ci++) {
                    for (unsigned form = 0; form < 6; form++)
                        check_ia32_shiftd(fast,size,opcodes[op],counts[ci],form,3,0);
                }
                for (unsigned sample = 0; sample < 5; sample++) {
                    check_ia32_shiftd(fast,size,opcodes[op],1,0,sample,0);
                    check_ia32_shiftd(fast,size,opcodes[op],7,5,sample,1);
                }
                for (unsigned scenario = 2; scenario <= 4; scenario++) {
                    check_ia32_shiftd(fast,size,opcodes[op],0,5,2,scenario);
                    check_ia32_shiftd(fast,size,opcodes[op],32,5,2,scenario);
                    check_ia32_shiftd(fast,size,opcodes[op],1,5,2,scenario);
                    check_ia32_shiftd(fast,size,opcodes[op],24,5,2,scenario);
                }
                check_ia32_shiftd(fast,size,opcodes[op],0,0,3,5);
                check_ia32_shiftd(fast,size,opcodes[op],1,5,3,5);
            }
        }
    }

    check_actual_control87_sequence(false);
    check_actual_control87_sequence(true);
    for (unsigned fast = 0; fast < 2; fast++)
        for (unsigned sample = 0; sample < 30; sample++)
            check_ia32_fwait_priority(fast, sample);

    for (unsigned fast = 0; fast < 2; fast++)
        for (unsigned kind = 0; kind < 3; kind++)
            for (unsigned op_prefix = 0; op_prefix < 2; op_prefix++)
                for (unsigned address32 = 0; address32 < 2; address32++)
                    for (unsigned crossing = 0; crossing < 2; crossing++)
                        for (unsigned scenario = 0; scenario < 6; scenario++)
                            for (unsigned unmask = 0; unmask < (kind == 1 ? 2u : 1u); unmask++)
                                check_ia32_x87_control(fast, kind, op_prefix, address32,
                                                        crossing, scenario, unmask);
    for (unsigned fast = 0; fast < 2; fast++)
        for (unsigned size = 2; size <= 4; size += 2) {
            for (unsigned call = 0; call < 2; call++)
                for (unsigned fault = 0; fault < 3; fault++)
                    check_ia32_split_instruction(fast, size, call, fault);
            for (unsigned far_return = 0; far_return < 2; far_return++)
                for (unsigned fault = 0; fault < 3; fault++)
                    check_ia32_ret_fetch(fast, size, far_return, fault);
            for (unsigned crossing = 0; crossing < 2; crossing++)
                check_ia32_wide_io(fast, size, crossing);
        }

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
                    ((uint64_t)ram_mb << 20) - UINT64_C(0x0106e000), false);
                check_efi_handoff(ram_mb, fast, timer, UINT64_C(0x08000000), false);
                check_efi_handoff(ram_mb, fast, timer, UINT64_C(0x01042000), true);
            }
    }
    for (unsigned fast = 0; fast < 2; fast++)
        for (unsigned timer = 0; timer < 2; timer++)
            for (unsigned scenario = 0; scenario < 9; scenario++)
                check_low_efi_interrupt(fast, timer, scenario);
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
        for (unsigned size = 2; size <= 4; size *= 2)
            for (unsigned first = 1; first < size; first++)
                for (unsigned kind = 0; kind < 4; kind++)
                    for (unsigned fault = 0; fault < (kind ? 3 : 2); fault++)
                        check_ia32_split_operand(fast, size, first, kind, fault);
        check_ia32_sib_ntdll_jump(fast);
        for (unsigned code32 = 0; code32 < 2; code32++)
            for (unsigned index = 0; index < 8; index++)
                for (unsigned scale = 0; scale < 4; scale++)
                    for (unsigned wrapped = 0; wrapped < 2; wrapped++)
                        for (unsigned override = 0; override < 2; override++) {
                            check_ia32_sib_no_base(fast, code32, index, scale, wrapped, override, false);
                            check_ia32_sib_no_base(fast, code32, index, scale, wrapped, override, true);
                        }
        for (unsigned code32 = 0; code32 < 2; code32++)
            for (unsigned op_prefix = 0; op_prefix < 2; op_prefix++)
                for (unsigned byte = 0; byte < 2; byte++)
                    for (unsigned addr_prefix = 0; addr_prefix < 2; addr_prefix++)
                        for (unsigned backward = 0; backward < 2; backward++)
                            for (unsigned repeat = 0; repeat < 3; repeat++)
                                for (unsigned scenario = 0; scenario < 4; scenario++)
                                    for (unsigned segment = 0; segment < 3; segment++)
                                        check_ia32_cmps(fast, code32, op_prefix, byte,
                                                       addr_prefix, backward, repeat, scenario, segment);
        for (unsigned size = 1; size <= 4; size *= 2)
            for (unsigned repeated = 0; repeated < 2; repeated++)
                check_ia32_cmps_locked(fast, size, repeated);
        for (unsigned size = 1; size <= 4; size *= 2)
            for (unsigned address32 = 0; address32 < 2; address32++)
                for (unsigned repe = 0; repe < 2; repe++)
                    for (unsigned source_fault = 0; source_fault < 2; source_fault++) {
                        check_ia32_cmps_fault(fast, size, address32, repe, false, source_fault);
                        if (size > 1)
                            check_ia32_cmps_fault(fast, size, address32, repe, true, source_fault);
                    }

        for (unsigned size = 1; size <= 4; size *= 2)
            for (unsigned address32 = 0; address32 < 2; address32++)
                for (unsigned crossing = 0; crossing < (size > 1 ? 2u : 1u); crossing++) {
                    for (unsigned fault_mode = 0; fault_mode < 3; fault_mode++)
                        check_ia32_string_restart(fast, 0, size, address32, crossing, fault_mode);
                    check_ia32_string_restart(fast, 1, size, address32, crossing, 0);
                    for (unsigned fault_mode = 1; fault_mode < 3; fault_mode++)
                        check_ia32_string_restart(fast, 2, size, address32, crossing, fault_mode);
                }
        for (unsigned size = 1; size <= 4; size *= 2) {
            for (unsigned sample = 0; sample < 4; sample++) {
                for (unsigned form = 0; form < (size == 1 ? 7 : 3); form++)
                    check_ia32_xadd(fast, size, form, sample, 0);
                check_ia32_xadd(fast, size, 0, sample, 2);
                if (size > 1) {
                    check_ia32_xadd(fast, size, 0, sample, 1);
                    check_ia32_xadd(fast, size, 0, sample, 3);
                }
            }
            check_ia32_xadd(fast, size, 7, 0, 0);
            check_ia32_xadd(fast, size, 7, 0, 2);
            if (size > 1) {
                check_ia32_xadd(fast, size, 7, 0, 1);
                check_ia32_xadd(fast, size, 7, 0, 3);
            }
            check_ia32_xadd_locked_register(fast, size);
        }
        for (unsigned code32 = 0; code32 < 2; code32++)
            for (unsigned op_prefix = 0; op_prefix < 2; op_prefix++)
                for (unsigned byte = 0; byte < 2; byte++)
                    for (unsigned addr_prefix = 0; addr_prefix < 2; addr_prefix++)
                        for (unsigned backward = 0; backward < 2; backward++)
                            for (unsigned repeat = 0; repeat < 3; repeat++)
                                for (unsigned scenario = 0; scenario < 4; scenario++)
                                    check_ia32_scas(fast, code32, op_prefix, byte,
                                                      addr_prefix, backward, repeat, scenario);
        for (unsigned size = 1; size <= 4; size *= 2)
            for (unsigned address32 = 0; address32 < 2; address32++)
                for (unsigned repe = 0; repe < 2; repe++) {
                    check_ia32_scas_fault(fast, size, address32, repe, false);
                    if (size > 1) check_ia32_scas_fault(fast, size, address32, repe, true);
                }
        for (unsigned code32 = 0; code32 < 2; code32++)
            for (unsigned op_prefix = 0; op_prefix < 2; op_prefix++)
                for (unsigned addr_prefix = 0; addr_prefix < 2; addr_prefix++)
                    for (unsigned locked = 0; locked < 2; locked++)
                        for (unsigned sample = 0; sample < 3; sample++)
                            for (unsigned scenario = 0; scenario < 4; scenario++)
                                check_ia32_cmpxchg8b(fast, code32, op_prefix,
                                                      addr_prefix, locked, sample, scenario);
        for (unsigned locked = 0; locked < 2; locked++)
            for (unsigned op_prefix = 0; op_prefix < 2; op_prefix++)
                for (unsigned addr_prefix = 0; addr_prefix < 2; addr_prefix++)
                    check_ia32_cmpxchg8b_invalid(fast, locked, op_prefix, addr_prefix);
        for (unsigned code32 = 0; code32 < 2; code32++)
            for (unsigned op_prefix = 0; op_prefix < 2; op_prefix++)
                for (unsigned addr_prefix = 0; addr_prefix < 2; addr_prefix++)
                    for (unsigned form = 0; form < 3; form++)
                        for (unsigned wrap = 0; wrap < 2; wrap++)
                            for (unsigned disabled = 0; disabled < 2; disabled++)
                                check_ia32_jmpe(fast, code32, op_prefix,
                                                 addr_prefix, form, wrap, disabled);
        for (unsigned size = 2; size <= 4; size *= 2)
            for (unsigned fault = 0; fault < 2; fault++)
                for (unsigned indirect = 0; indirect < 2; indirect++)
                    for (unsigned disabled = 0; disabled <= fault; disabled++)
                        check_ia32_jmpe_fetch(fast, size, fault, indirect, disabled);
        for (unsigned code32 = 0; code32 < 2; code32++)
            for (unsigned stack32 = 0; stack32 < 2; stack32++)
                for (unsigned op_prefix = 0; op_prefix < 2; op_prefix++)
                    for (unsigned addr_prefix = 0; addr_prefix < 2; addr_prefix++)
                        for (unsigned scenario = 0; scenario < (stack32 ? 4 : 5); scenario++)
                            check_ia32_leave(fast, code32, stack32, op_prefix,
                                              addr_prefix, scenario);
        for (unsigned size = 2; size <= 4; size *= 2)
            for (unsigned equal = 0; equal < 2; equal++)
                for (unsigned fault = 0; fault < 2; fault++)
                    check_ia32_cmpxchg_cross_page(fast, size, equal, fault);
        for (unsigned size = 1; size <= 4; size *= 2)
            for (unsigned sample = 0; sample < 3; sample++) {
                for (unsigned form = 0; form < (size == 1 ? 4 : 2); form++)
                    check_ia32_cmpxchg(fast, size, form, sample, 0);
                for (unsigned fault = 1; fault <= 2; fault++)
                    check_ia32_cmpxchg(fast, size, 0, sample, fault);
            }
        for (unsigned source = 0; source < 7; source++)
            for (unsigned backward = 0; backward < 2; backward++)
                for (unsigned qualified = 0; qualified < 2; qualified++)
                    for (unsigned nat = 0; nat < 2; nat++)
                        for (unsigned disabled = 0; disabled < 4; disabled++)
                            check_control_branch(fast, source, backward,
                                                  qualified, nat, disabled);
        for (unsigned form = 0; form < 4; form++)
            for (unsigned backward = 0; backward < 2; backward++)
                for (unsigned qualified = 0; qualified < 2; qualified++)
                    for (unsigned warm = 0; warm < 2; warm++)
                        check_advanced_branch(fast, form, backward, qualified, warm);
        for (unsigned scenario = 0; scenario < 4; scenario++)
            check_region7_translation(fast, scenario);
        for (unsigned scenario = 0; scenario < 7; scenario++)
            check_epc(scenario, fast);
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
        check_rse_interrupted_context(fast, false, false);
        check_rse_interrupted_context(fast, false, true);
        check_rse_interrupted_context(fast, true, true);
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
    puts("Merced chk.a: signed recovery targets, integer/FP forms, ALAT hits and preserved frames passed");
    puts("Merced chk.s: FP/integer NaT checks, signed targets, predication, rotation and disabled banks passed");
    puts("Merced IA32 CMPXCHG: byte/word/dword, aliases, flags, memory writeback and fault retry passed");
    puts("Merced IA32 CMPXCHG8B: register pairs, fixed width, prefixes, cross-page faults and invalid forms passed");
    puts("Merced IA32 LEAVE: operand/stack widths, prefixes, wrap, cross-page reads and fault retry passed");
    puts("Merced IA32 JMPE: direct/indirect targets, widths, native state and precise faults passed");
    puts("Merced IA32 SCAS: widths, ES, repeat termination, direction, wrap and precise suffix retry passed");
    puts("Merced IA32 CMPS: source segments, widths, repetition, direction, wrap and precise pair retry passed");
    puts("Merced IA32 SIB: no-base scaled indices, displacement width, wrap, segments and ntdll jump table passed");
    puts("Merced IA32 operands: discontiguous reads/stores, relocation writes and precise ADC fault retry passed");
    puts("Merced IA32 fetch/I/O: split immediate fault retry and wide high-aperture transactions passed");
    puts("Merced IA32 RET: split immediate fetch faults preserve return address, CS and stack cleanup passed");
    puts("Merced IA32 x87 control: FCW width/masks, pending faults, no-wait forms and split operands passed");
    puts("Merced IA32 FWAIT: masked/unmasked status, disabled priority, CFLG and native exception retry passed");
    puts("Merced IA32 x87 arithmetic: COMCTL32 sequence, rounding, exceptions, raw stack copies and fault retry passed");
    puts("Merced IA32 FP exits: sticky MFL, unchanged MFH, disabled state and native interrupt RFI passed");
    puts("Merced IA32 SHLD/SHRD: widths, count masking, aliases, flags and memory fault retry passed");
    puts("Merced IA32 BSF/BSR: bit indices, widths, aliases, zero policy, read faults and invalid LOCK passed");
    puts("Merced IA32 BSWAP: register byte order, width policy, preserved flags and invalid LOCK passed");
    puts("Merced IA32 XADD: widths, aliases, ADD flags, RMW fault retry and invalid LOCK forms passed");
    puts("Merced IA32 transfers: exact REP suffix counts, split elements and guarded fault retries passed");
    puts("Merced integer loads: precise fault restart and speculative deferral passed");
    puts("Merced tbit/tnat: unconditional predicate clearing and guarded branches passed");
    puts("Merced fmix/fsxt: lanes, source order, sign extension, format, NaTVal and predication passed");
    puts("Merced RSE: interrupted contexts, common-IP resumes, automatic fills, RNAT and fault retry passed");
    puts("Merced zero-fill: physical acceleration, discontiguous mappings and fault retry passed");
    puts("Merced mux1: all byte permutations, low-byte broadcast, NaT and predication passed");
    puts("Merced rotating predicates: IRQ save, cover, restore and rfi preserve pipeline stages");
    puts("Merced predicate transfers: physical PR bits, immediate sign extension, qualifiers and NaT passed");
    puts("Merced epc/br.ret: gateway promotion, PFS.ppl fault, physical mode and demotion passed");
    return 0;
}
