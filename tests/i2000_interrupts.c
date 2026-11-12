/* ROM-free ATAPI/SAPIC integration and snapshot regressions.
 * Run: python3 tools/ia64/run_i2000_interrupt_test.py
 * Include the machine to exercise its static device polling without a UI. */
#include "../hardware/machine/ia64/machine_i2000.c"
#include <assert.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

static Ia64I2000State *machine(void) {
    Ia64Config cfg = {.ram_size = UINT64_C(64) << 20,
                     .display_type = GEMU_DISPLAY_NONE};
    Ia64I2000State *s = ia64_i2000_create(&cfg);
    CHECK(s);
    return s;
}
static bool irq(Ia64I2000State *s, unsigned vector) {
    return (s->cpu->external_pending[vector >> 3] >> (vector & 7)) & 1;
}
static void test_gpio_straps(void) {
    Ia64I2000State *s = machine();
    const uint64_t gpio = I2000_IO_BASE + 0x404;
    for (unsigned reset = 0; reset < 2; reset++) {
        /* Both active-low board straps must stay deasserted, including
         * when firmware reads the high bytes of the GPIO data register. */
        CHECK(bus_read(s, gpio, 4) == UINT32_C(0x01040000));
        CHECK(bus_read(s, gpio, 2) == 0);
        CHECK(bus_read(s, gpio + 2, 2) == 0x0104);
        CHECK(bus_read(s, gpio + 2, 1) == 4);
        CHECK(bus_read(s, gpio + 3, 1) == 1);
        CHECK(bus_read(s, gpio | UINT64_C(0xc000000000000000), 4) ==
              UINT32_C(0x01040000));
        bus_write(s, gpio, 0, 4);
        CHECK(bus_read(s, gpio, 4) == UINT32_C(0x01040000));
        bus_write(s, gpio + 2, 0xffff, 2);
        bus_write(s, gpio + 3, 0, 1);
        CHECK(bus_read(s, gpio, 4) == UINT32_C(0x01040000));
        i2000_reset(s, false);
    }
    ia64_i2000_destroy(s);
}
static void route(Ia64I2000State *s, unsigned vector) {
    bus_write(s, I460GX_IOSAPIC_BASE, 0x2e, 4);
    bus_write(s, I460GX_IOSAPIC_BASE + 0x10, vector, 4);
}
static void ready(Ia64I2000State *s) {
    s->cpu->ninsts += ATAPI_IRQ_LATENCY_NINSTS;
    i2000_poll_interrupts(s);
}
static void identify(Ia64I2000State *s) {
    io_port_write(s, 0x177, 0xa1, 1);
    CHECK(io_port_read(s, 0x376, 1) == 0x80);
    CHECK(!irq(s, 0x60));
    ready(s);
}
static void test_identify(void) {
    Ia64I2000State *s = machine();
    route(s, 0x60);
    identify(s);
    CHECK(irq(s, 0x60) && !irq(s, 0));
    CHECK(s->atapi_irq_pending && s->atapi_irq_asserted);
    merced_ack_external(s->cpu, 0x60);
    i2000_poll_interrupts(s);
    CHECK(!irq(s, 0x60)); /* CPU acceptance must not produce another edge. */
    CHECK(io_port_read(s, 0x376, 1) == 0x48);
    CHECK(s->atapi_irq_pending); /* Alternate Status does not acknowledge. */
    CHECK(io_port_read(s, 0x177, 1) == 0x48);
    CHECK(!s->atapi_irq_pending && !s->atapi_irq_asserted);
    CHECK(io_port_read(s, 0x170, 2) == 0x85c0);
    for (unsigned i = 1; i < 256; i++) io_port_read(s, 0x170, 2);
    CHECK(io_port_read(s, 0x376, 1) == 0x40);
    ready(s);
    CHECK(!irq(s, 0x60) && !s->atapi_irq_pending);
    ia64_i2000_destroy(s);
}
static void test_control(void) {
    Ia64I2000State *s = machine();
    route(s, 0x60);
    io_port_write(s, 0x376, 2, 1); /* nIEN */
    identify(s);
    CHECK(!irq(s, 0x60) && !irq(s, 0));
    CHECK(s->atapi_irq_pending && !s->atapi_irq_asserted);
    CHECK(s->atapi_status == 0x48 && s->atapi_data_pos == 0);
    io_port_write(s, 0x376, 0, 1);
    CHECK(s->atapi_status == 0x48 && s->atapi_irq_pending);
    i2000_poll_interrupts(s);
    CHECK(irq(s, 0x60));
    merced_ack_external(s->cpu, 0x60);
    io_port_read(s, 0x177, 1);
    io_port_read(s, 0x170, 2);
    io_port_write(s, 0x376, 2, 1);
    io_port_write(s, 0x376, 0, 1);
    CHECK(s->atapi_status == 0x48 && s->atapi_data_pos == 2);
    CHECK(!s->atapi_irq_pending);
    io_port_write(s, 0x376, 4, 1);
    CHECK(s->atapi_status == 0x80 && !s->atapi_irq_pending);
    io_port_write(s, 0x376, 0, 1);
    CHECK(s->atapi_status == 0x40 && s->atapi_count == 1);
    CHECK(s->atapi_lba_mid == 0x14 && s->atapi_lba_high == 0xeb);
    ia64_i2000_destroy(s);
}
static void test_packet_phases(void) {
    Ia64I2000State *s = machine();
    route(s, 0x60);
    io_port_write(s, 0x174, 8, 1);
    io_port_write(s, 0x175, 0, 1);
    io_port_write(s, 0x177, 0xa0, 1);
    ready(s);
    CHECK(s->atapi_status == 0x48 && !irq(s, 0x60));
    const uint8_t packet[12] = {0x12, 0, 0, 0, 36}; /* INQUIRY, no media needed. */
    for (unsigned i = 0; i < 12; i += 2)
        io_port_write(s, 0x170, packet[i] | packet[i + 1] << 8, 2);
    unsigned remaining = 36;
    while (remaining) {
        ready(s);
        CHECK(irq(s, 0x60));
        merced_ack_external(s->cpu, 0x60);
        CHECK(io_port_read(s, 0x177, 1) == 0x48);
        if (remaining == 36) {
            CHECK(i2000_save_snapshot(s, "packet.vmstate"));
            s->atapi_packet_command = false;
            CHECK(i2000_load_snapshot(s, "packet.vmstate"));
            CHECK(s->atapi_packet_command && !s->atapi_irq_pending);
        }
        unsigned phase = remaining < 8 ? remaining : 8;
        CHECK(io_port_read(s, 0x174, 1) == phase);
        for (unsigned i = 0; i < phase; i += 2) io_port_read(s, 0x170, 2);
        remaining -= phase;
    }
    ready(s);
    CHECK(irq(s, 0x60)); /* Distinct PACKET command-complete interrupt. */
    merced_ack_external(s->cpu, 0x60);
    CHECK(io_port_read(s, 0x177, 1) == 0x40);
    CHECK(io_port_read(s, 0x172, 1) == 3);
    ready(s);
    CHECK(!irq(s, 0x60));
    ia64_i2000_destroy(s);
}
static void test_legacy(void) {
    Ia64I2000State *s = machine();
    /* Reset PIC masks IRQ15: an 8259 shows the request in IRR but doesn't
     * deliver it.  AMI's IA-32 BIOS polls IRR this way with IRQ15 masked
     * while EFI reads the CD; the master's IRQ2 cascade stays clear. */
    identify(s);
    CHECK(!irq(s, 0) && !irq(s, 0x60) && !s->atapi_legacy_irq_latched);
    CHECK((io_port_read(s, 0xA0, 1) & 0x80) && !(io_port_read(s, 0x20, 1) & 0x04));
    CHECK(io_port_read(s, 0x177, 1) == 0x48);  /* Status read lowers INTRQ */
    CHECK(!(io_port_read(s, 0xA0, 1) & 0x80));
    for (unsigned i = 0; i < 256; i++) io_port_read(s, 0x170, 2);
    ready(s);
    /* Unmasked (slave IRQ7, master cascade IRQ2): the firmware's reset-RTE
     * ExtINT path delivers it. */
    io_port_write(s, 0x21, 0xFB, 1);
    io_port_write(s, 0xA1, 0x7F, 1);
    identify(s);
    CHECK(irq(s, 0) && !irq(s, 0x60));
    CHECK(io_port_read(s, 0x177, 1) == 0x48);
    CHECK(!s->atapi_irq_pending && s->atapi_legacy_irq_latched);
    CHECK(i2000_save_snapshot(s, "legacy.vmstate"));
    s->atapi_legacy_irq_latched = false;
    CHECK(i2000_load_snapshot(s, "legacy.vmstate"));
    CHECK(!s->atapi_irq_pending && s->atapi_legacy_irq_latched);
    /* The PIC still owns IRQ15 after the device's INTRQ is cleared. */
    CHECK(bus_read(s, I2000_LEGACY_ACK_ADDR, 1) == s->pic_slave_base + 7u);
    CHECK(!s->atapi_irq_pending && !irq(s, 0));
    ready(s);
    CHECK(!irq(s, 0));
    ia64_i2000_destroy(s);
}
static void test_os_mask(void) {
    Ia64I2000State *s = machine();
    route(s, 0x60 | I460GX_IOSAPIC_RTE_MASK);
    identify(s);
    CHECK(!irq(s, 0x60) && !irq(s, 0));
    CHECK(s->atapi_irq_pending && !s->atapi_irq_asserted);
    route(s, 0x60);
    i2000_poll_interrupts(s);
    CHECK(irq(s, 0x60) && !irq(s, 0));
    ia64_i2000_destroy(s);
}
static void route_gsi(Ia64I2000State *s, unsigned gsi, uint32_t rte) {
    bus_write(s, I460GX_IOSAPIC_BASE, 0x10 + 2 * gsi, 4);
    bus_write(s, I460GX_IOSAPIC_BASE + 0x10, rte, 4);
}
/* Windows' i8042 driver takes IRQ1 through the fixed vector it programs in
 * GSI1 (0x80 in XP Setup).  One edge per byte the 8042 makes available:
 * accepting the vector doesn't re-raise it, a data-port read lets the next
 * byte raise a fresh edge, and a masked OS vector never falls back to the
 * legacy ExtINT path. */
static void test_keyboard(void) {
    Ia64I2000State *s = machine();
    route_gsi(s, 1, 0x80);
    i2000_type_key(s, 'a');            /* make + break scancodes */
    i2000_poll_interrupts(s);
    CHECK(irq(s, 0x80) && !irq(s, 0) && s->keyboard_gsi_asserted);
    merced_ack_external(s->cpu, 0x80);
    i2000_poll_interrupts(s);
    CHECK(!irq(s, 0x80));              /* acceptance is not a new edge */
    io_port_read(s, 0x60, 1);
    CHECK(!s->keyboard_gsi_asserted);
    i2000_poll_interrupts(s);
    CHECK(irq(s, 0x80));               /* second byte: fresh edge */
    merced_ack_external(s->cpu, 0x80);
    for (unsigned i = 0; i < 8 && (s->kbc_out_pos < s->kbc_out_len ||
                                   ps2_keyboard_has_data(&s->ps2_keyboard)); i++)
        io_port_read(s, 0x60, 1);
    i2000_poll_interrupts(s);
    CHECK(!irq(s, 0x80) && !irq(s, 0));
    s->pic_master_mask = 0xf8;         /* PIC IRQ1 open, as Windows leaves it */
    route_gsi(s, 1, 0x80 | I460GX_IOSAPIC_RTE_MASK);
    i2000_type_key(s, 'b');
    s->cpu->ninsts += 1000000;
    i2000_poll_interrupts(s);
    CHECK(!irq(s, 0x80) && !irq(s, 0)); /* masked OS vector: no fallback */
    route_gsi(s, 1, 0x80);
    i2000_poll_interrupts(s);
    CHECK(irq(s, 0x80));
    /* Function keys (Setup's F8=I agree) reach the 8042 as set-1 codes. */
    while (s->kbc_out_pos < s->kbc_out_len || ps2_keyboard_has_data(&s->ps2_keyboard))
        io_port_read(s, 0x60, 1);
    i2000_type_key(s, 0xffc5);         /* F8 */
    CHECK(io_port_read(s, 0x60, 1) == 0x42 && io_port_read(s, 0x60, 1) == 0xC2);
    i2000_type_key(s, 0xffc9);         /* F12 */
    CHECK(io_port_read(s, 0x60, 1) == 0x58 && io_port_read(s, 0x60, 1) == 0xD8);
    i2000_type_key(s, 'b');
    i2000_poll_interrupts(s);
    CHECK(i2000_save_snapshot(s, "kbd.vmstate"));
    s->keyboard_gsi_asserted = false;
    CHECK(i2000_load_snapshot(s, "kbd.vmstate"));
    CHECK(s->keyboard_gsi_asserted);
    ia64_i2000_destroy(s);
}
/* An unsupported version must be rejected from its header, without reading
 * a differently laid-out payload or changing the running machine. */
static void old_snapshot(unsigned version, uint64_t ram_size) {
    FILE *out = fopen("irq-old.vmstate", "wb");
    CHECK(out);
    uint64_t magic = I2000_SNAPSHOT_MAGIC;
    uint32_t v = version;
    CHECK(fwrite(&magic, sizeof(magic), 1, out) == 1);
    CHECK(fwrite(&v, sizeof(v), 1, out) == 1);
    CHECK(fwrite(&ram_size, sizeof(ram_size), 1, out) == 1);
    CHECK(fclose(out) == 0);
}
static void test_snapshots(void) {
    Ia64I2000State *s = machine();
    route(s, 0x60);
    identify(s);
    merced_ack_external(s->cpu, 0x60);
    s->cpu->gr_static[8] = UINT64_C(0x123456789abcdef0);
    CHECK(i2000_save_snapshot(s, "irq-current.vmstate"));
    atapi_clear_irq(s);
    CHECK(i2000_load_snapshot(s, "irq-current.vmstate"));
    CHECK(s->atapi_irq_pending && s->atapi_irq_asserted);
    i2000_poll_interrupts(s);
    CHECK(!irq(s, 0x60));
    io_port_read(s, 0x177, 1);
    CHECK(i2000_save_snapshot(s, "irq-current.vmstate"));
    CHECK(i2000_load_snapshot(s, "irq-current.vmstate"));
    CHECK(!s->atapi_irq_pending); /* Acknowledged DRQ must not be rearmed. */
    io_port_write(s, 0x376, 2, 1);
    identify(s);
    CHECK(i2000_save_snapshot(s, "irq-current.vmstate"));
    io_port_write(s, 0x376, 0, 1);
    CHECK(i2000_load_snapshot(s, "irq-current.vmstate"));
    CHECK(s->atapi_device_control == 2 && !s->atapi_packet_command);
    CHECK(s->atapi_irq_pending && !s->atapi_irq_asserted);
    i2000_poll_interrupts(s);
    CHECK(!irq(s, 0x60));
    io_port_write(s, 0x376, 0, 1);
    i2000_poll_interrupts(s);
    CHECK(irq(s, 0x60));
    for (unsigned version = 9; version < I2000_SNAPSHOT_VERSION; version++) {
        old_snapshot(version, s->ram_size);
        s->ram[0x1234] = 0x5a;
        s->chipset.sac_ccsr = 0x12345678;
        CHECK(!i2000_load_snapshot(s, "irq-old.vmstate"));
        CHECK(s->ram[0x1234] == 0x5a);
        CHECK(s->chipset.sac_ccsr == 0x12345678);
        CHECK(s->cpu->gr_static[8] == UINT64_C(0x123456789abcdef0));
        CHECK(s->atapi_irq_pending && s->atapi_irq_asserted);
        CHECK(irq(s, 0x60));
    }
    /* Rejected input did not overwrite the valid checkpoint. */
    CHECK(i2000_load_snapshot(s, "irq-current.vmstate"));
    CHECK(s->atapi_device_control == 2 && !s->atapi_irq_asserted);
    CHECK(s->cpu->gr_static[8] == UINT64_C(0x123456789abcdef0));
    ia64_i2000_destroy(s);
}
static void pci_write(Ia64I2000State *s, unsigned bus, unsigned dev,
                       unsigned fun, unsigned reg, uint32_t value) {
    io_port_write(s, PCI_CFG_ADDR, UINT32_C(0x80000000) | bus << 16 |
                  dev << 11 | fun << 8 | (reg & 0xfc), 4);
    io_port_write(s, PCI_CFG_DATA + (reg & 3), value, 4);
}
static void test_chipset_snapshot(void) {
    Ia64I2000State *source = machine();
    bus_write(source, I460GX_SAC_CBNR, 0x81, 4);
    bus_write(source, I460GX_SAC_CCSR, 0x80, 4);
    pci_write(source, 0, 0x10, 0, 0x40, 0x32); /* CBN */
    pci_write(source, 0x32, 0, 0, 0x44, 0x680);
    pci_write(source, 0x32, 5, 4, 0x48, 1);   /* memory-card row select */
    pci_write(source, 0, 3, 3, 0x20, 0x1a01); /* SMBus */
    pci_write(source, 0, 3, 3, 4, 1);
    io_port_write(source, 0x1a03, 2, 1);
    io_port_write(source, 0x1a04, 0xa1, 1);
    io_port_write(source, 0x1a02, 0x48, 1);
    pci_write(source, 0, 3, 0, 0x40, 0xa01);  /* PM */
    pci_write(source, 0, 3, 0, 0x44, 1);
    io_port_write(source, 0xa04, 0x400, 2);
    io_port_write(source, 0xb2, 0xa0, 1);
    pci_write(source, 0, 3, 2, 0x20, 0x1201); /* UHCI */
    pci_write(source, 0, 3, 2, 4, 1);
    io_port_write(source, 0x1206, 42, 2);
    io_port_write(source, 0x1200, 1, 2);
    route_gsi(source, 1, 0x80);
    CHECK(i2000_save_snapshot(source, "chipset.vmstate"));

    /* Restore into another board so stale callback pointers are observable. */
    Ia64I2000State *s = machine();
    CHECK(i2000_load_snapshot(s, "chipset.vmstate"));
    CHECK(s->chipset.sac_cbnr == 0x81 && s->chipset.sac_ccsr == 0x80);
    CHECK(s->chipset.chipset_bus == 0x32);
    CHECK(s->chipset.chipset_cfg[0][0][0x45] == 6);
    CHECK(s->chipset.memcard_cfg[0][4][0x48] == 1);
    CHECK(io_port_read(s, 0x1a00, 1) == 2);
    CHECK(io_port_read(s, 0x1a05, 1) == 4);
    CHECK(io_port_read(s, 0xa04, 2) == 0x401);
    CHECK(io_port_read(s, 0xb2, 1) == 0xa0);
    CHECK(io_port_read(s, 0x1206, 2) == 42);
    CHECK(s->chipset.config.opaque == s);
    s->spd[0][2] = 0x5a;
    io_port_write(s, 0x1a02, 0x48, 1);
    CHECK(io_port_read(s, 0x1a05, 1) == 0x5a);
    CHECK(io_port_read(source, 0x1a05, 1) == 4);
    io_port_write(s, 0xb2, 0xa1, 1);
    CHECK(io_port_read(s, 0xa04, 2) == 0x400);
    CHECK(io_port_read(source, 0xa04, 2) == 0x401);
    s->cpu->ar[44] += I2000_ITC_HZ / 1000;
    CHECK(io_port_read(s, 0xa08, 4) == 3579);
    CHECK(io_port_read(s, 0x1206, 2) == 43);
    CHECK(io_port_read(source, 0x1206, 2) == 42);
    i2000_type_key(s, 'a');
    i2000_poll_interrupts(s);
    CHECK(irq(s, 0x80) && !irq(source, 0x80));
    ia64_i2000_destroy(s);
    ia64_i2000_destroy(source);
}
/* Primary-channel ATA disk, as Windows' IDE driver uses it (GSI14). */
static void ata_wait(Ia64I2000State *s) {
    s->cpu->ninsts += ATAPI_IRQ_LATENCY_NINSTS;
    i2000_poll_interrupts(s);
}
static void ata_task(Ia64I2000State *s, uint8_t device, uint32_t lba,
                     uint8_t count, uint8_t cmd) {
    io_port_write(s, 0x1F6, device | ((lba >> 24) & 0x0F), 1);
    io_port_write(s, 0x1F2, count, 1);
    io_port_write(s, 0x1F3, lba & 0xFF, 1);
    io_port_write(s, 0x1F4, (lba >> 8) & 0xFF, 1);
    io_port_write(s, 0x1F5, (lba >> 16) & 0xFF, 1);
    io_port_write(s, 0x1F7, cmd, 1);
}
/* One interrupt: raised once, acknowledged by a normal Status read. */
static uint8_t ata_take_irq(Ia64I2000State *s) {
    CHECK(!irq(s, 0x70));
    ata_wait(s);
    CHECK(irq(s, 0x70));
    merced_ack_external(s->cpu, 0x70);
    CHECK(s->ata_irq_pending);
    uint8_t alt = (uint8_t)io_port_read(s, 0x3F6, 1);
    CHECK(s->ata_irq_pending);               /* Alternate Status doesn't ack */
    uint8_t st = (uint8_t)io_port_read(s, 0x1F7, 1);
    CHECK(st == alt && !s->ata_irq_pending);
    i2000_poll_interrupts(s);
    CHECK(!irq(s, 0x70));
    return st;
}
static void ata_read_sector(Ia64I2000State *s, uint32_t expect_lba) {
    uint8_t buf[512];
    for (unsigned i = 0; i < 512; i += 2) {
        uint16_t w = (uint16_t)io_port_read(s, 0x1F0, 2);
        buf[i] = (uint8_t)w; buf[i + 1] = (uint8_t)(w >> 8);
    }
    uint32_t tag;
    memcpy(&tag, buf, 4);
    CHECK(tag == expect_lba && buf[511] == (uint8_t)expect_lba);
}
static void ata_write_sector(Ia64I2000State *s, uint32_t tag) {
    uint8_t buf[512];
    memset(buf, 0xA5 ^ (uint8_t)tag, sizeof(buf));
    memcpy(buf, &tag, 4);
    for (unsigned i = 0; i < 512; i += 2)
        io_port_write(s, 0x1F0, buf[i] | (buf[i + 1] << 8), 2);
}
static void test_ata(void) {
    const uint32_t sectors = 2048;          /* 1 MiB image */
    FILE *f = fopen("ata.img", "wb");
    CHECK(f);
    for (uint32_t n = 0; n < sectors; n++) {
        uint8_t sec[512];
        memset(sec, (uint8_t)n, sizeof(sec));
        memcpy(sec, &n, 4);
        CHECK(fwrite(sec, 1, sizeof(sec), f) == sizeof(sec));
    }
    fclose(f);
    CHECK(gemu_drive_add_legacy(GEMU_DRIVE_IF_IDE, 0, GEMU_DRIVE_MEDIA_DISK, "ata.img"));
    Ia64I2000State *s = machine();
    CHECK(s->hda);

    /* Firmware phase, GSI14 at its reset value.  The disk is busy (BSY,
     * no interrupt) until ready, and a Status read in that window must not
     * lose the interrupt.  With cr.iva zero (the SDV's IA-32 INT 13h
     * "BIOS CallBacks"), completion sets the BIOS data area's disk flag
     * 40:8E that that code polls, instead of an ExtINT nothing takes. */
    s->cpu->cr[2] = 0;
    s->ram[0x48E] = 0;
    ata_task(s, 0xE0, 0, 1, 0x20);
    CHECK(io_port_read(s, 0x1F7, 1) == 0x80 && s->ata_irq_pending);
    ata_wait(s);
    CHECK(s->ram[0x48E] == 0xFF && !s->ata_legacy_irq_latched && !irq(s, 0));
    CHECK(io_port_read(s, 0x1F7, 1) == 0x58);
    ata_read_sector(s, 0);
    /* Inside an IA-32 BIOS callback (PSR.is), even with a firmware IVA:
     * EFI enters those with PSR.i clear, so the BIOS's IRQ14 handler can't
     * run; completion sets its 40:8E flag the same way. */
    s->cpu->cr[2] = 0x10000;
    s->cpu->psr |= UINT64_C(1) << 34;
    s->ram[0x48E] = 0;
    ata_task(s, 0xE0, 1, 1, 0x20);
    ata_wait(s);
    CHECK(s->ram[0x48E] == 0xFF && !s->ata_legacy_irq_latched && !irq(s, 0));
    CHECK(io_port_read(s, 0x1F7, 1) == 0x58);
    ata_read_sector(s, 1);
    s->cpu->psr &= ~(UINT64_C(1) << 34);
    /* With a firmware IVA but IRQ14 still masked (reset PIC), the request
     * only shows in the slave's IRR; it is not delivered. */
    s->cpu->cr[2] = 0x10000;
    s->ram[0x48E] = 0;
    ata_task(s, 0xE0, 2, 1, 0x20);
    ata_wait(s);
    CHECK(!irq(s, 0) && !s->ata_legacy_irq_latched &&
          (io_port_read(s, 0xA0, 1) & 0x40));
    CHECK(io_port_read(s, 0x1F7, 1) == 0x58 && !(io_port_read(s, 0xA0, 1) & 0x40));
    ata_read_sector(s, 2);
    /* Unmasked as EFI leaves it (slave IRQ6, master cascade IRQ2), the
     * legacy IRQ14 latch survives the Status read and is acknowledged as
     * slave PIC input 6. */
    io_port_write(s, 0x21, 0xFB, 1);
    io_port_write(s, 0xA1, 0xBF, 1);
    s->cpu->psr |= UINT64_C(1) << 14;              /* native code, PSR.i on */
    s->ram[0x48E] = 0;
    ata_task(s, 0xE0, 3, 1, 0x20);
    s->cpu->ninsts += ATAPI_IRQ_LATENCY_NINSTS;
    CHECK(io_port_read(s, 0x1F7, 1) == 0x58);      /* delivers, then acks */
    CHECK(s->ata_legacy_irq_latched && irq(s, 0) && !s->ata_irq_pending);
    CHECK(bus_read(s, I2000_LEGACY_ACK_ADDR, 1) == s->pic_slave_base + 6u);
    CHECK(!s->ata_legacy_irq_latched && s->ram[0x48E] == 0);
    ata_read_sector(s, 3);
    /* Native code with PSR.i clear can't take it either: BIOS flag. */
    s->cpu->psr &= ~(UINT64_C(1) << 14);
    ata_task(s, 0xE0, 4, 1, 0x20);
    ata_wait(s);
    CHECK(s->ram[0x48E] == 0xFF && !s->ata_legacy_irq_latched && !irq(s, 0));
    CHECK(io_port_read(s, 0x1F7, 1) == 0x58);
    ata_read_sector(s, 4);
    s->cpu->cr[2] = 0;
    route_gsi(s, 14, 0x70);

    /* IDENTIFY: data-in interrupt, then the words Windows relies on. */
    ata_task(s, 0xE0, 0, 0, 0xEC);
    CHECK(ata_take_irq(s) == 0x58);
    uint16_t id[256];
    for (unsigned i = 0; i < 256; i++) id[i] = (uint16_t)io_port_read(s, 0x1F0, 2);
    CHECK(id[0] == 0x0040 && (id[49] & 0x0200) && !(id[49] & 0x0100));
    CHECK(id[60] == sectors && id[61] == 0 && id[63] == 0 && id[88] == 0);
    CHECK((id[47] & 0xFF) == ATA_MAX_MULTIPLE);
    CHECK(io_port_read(s, 0x1F7, 1) == 0x50);
    ata_wait(s);
    CHECK(!irq(s, 0x70));                    /* no interrupt after data-in */

    /* Device 1 is absent: it reads as no device and runs nothing. */
    io_port_write(s, 0x1F6, 0xF0, 1);
    CHECK(io_port_read(s, 0x1F7, 1) == 0 && io_port_read(s, 0x3F6, 1) == 0);
    io_port_write(s, 0x1F7, 0xEC, 1);
    ata_wait(s);
    CHECK(!irq(s, 0x70) && !s->ata_irq_pending);
    io_port_write(s, 0x1F6, 0xE0, 1);
    CHECK(io_port_read(s, 0x1F7, 1) == 0x50);

    /* An ATA disk aborts IDENTIFY PACKET DEVICE. */
    ata_task(s, 0xE0, 0, 0, 0xA1);
    CHECK(ata_take_irq(s) == 0x51 && io_port_read(s, 0x1F1, 1) == 0x04);

    /* READ SECTORS: one DRQ block and interrupt per sector. */
    ata_task(s, 0xE0, 5, 3, 0x20);
    for (uint32_t n = 5; n < 8; n++) {
        CHECK(ata_take_irq(s) == 0x58);
        ata_read_sector(s, n);
    }
    CHECK(io_port_read(s, 0x1F7, 1) == 0x50);
    ata_wait(s);
    CHECK(!irq(s, 0x70));

    /* Multiple mode is on at power-up (firmware relies on it); disabled,
     * READ MULTIPLE aborts.  Then blocks of the SET MULTIPLE count. */
    CHECK(s->ata_multiple == ATA_MAX_MULTIPLE && (id[59] & 0x1FF) == 0x100 + ATA_MAX_MULTIPLE);
    ata_task(s, 0xE0, 0, 0, 0xC6);
    CHECK(ata_take_irq(s) == 0x50 && s->ata_multiple == 0);
    ata_task(s, 0xE0, 0, 3, 0xC4);
    CHECK(ata_take_irq(s) == 0x51);
    ata_task(s, 0xE0, 0, 3, 0xC6);          /* 3 is not a power of two */
    CHECK(ata_take_irq(s) == 0x51);
    ata_task(s, 0xE0, 0, 2, 0xC6);
    CHECK(ata_take_irq(s) == 0x50 && s->ata_multiple == 2);
    ata_task(s, 0xE0, 10, 5, 0xC4);
    for (uint32_t n = 10; n < 15;) {
        CHECK(ata_take_irq(s) == 0x58);
        unsigned block = n + 2 <= 15 ? 2 : 1;
        for (unsigned i = 0; i < block; i++) ata_read_sector(s, n++);
    }
    CHECK(io_port_read(s, 0x1F7, 1) == 0x50);

    /* WRITE SECTORS: first block without an interrupt, then one per block,
     * the last being the completion. */
    ata_task(s, 0xE0, 20, 2, 0x30);
    CHECK(io_port_read(s, 0x3F6, 1) == 0x58);
    ata_wait(s);
    CHECK(!irq(s, 0x70) && !s->ata_irq_pending);
    ata_write_sector(s, 0x1000);
    CHECK(ata_take_irq(s) == 0x58);
    ata_write_sector(s, 0x1001);
    CHECK(ata_take_irq(s) == 0x50);
    /* WRITE MULTIPLE (blocks of 2): 3 sectors at 30. */
    ata_task(s, 0xE0, 30, 3, 0xC5);
    ata_write_sector(s, 0x2000); ata_write_sector(s, 0x2001);
    CHECK(ata_take_irq(s) == 0x58);
    ata_write_sector(s, 0x2002);
    CHECK(ata_take_irq(s) == 0x50);
    const uint32_t written[][2] = {{20, 0x1000}, {21, 0x1001}, {30, 0x2000},
                                   {31, 0x2001}, {32, 0x2002}};
    for (unsigned i = 0; i < 5; i++) {
        uint8_t sec[512];
        uint32_t tag;
        CHECK(gemu_drive_read(s->hda, (uint64_t)written[i][0] * 512, sec, 512));
        memcpy(&tag, sec, 4);
        CHECK(tag == written[i][1] && sec[511] == (uint8_t)(0xA5 ^ written[i][1]));
    }
    ata_task(s, 0xE0, 22, 1, 0x20);         /* neighbours untouched */
    CHECK(ata_take_irq(s) == 0x58);
    ata_read_sector(s, 22);

    /* Out of range: IDNF.  CHS: C/H/S 0/1/1 is LBA 63. */
    ata_task(s, 0xE0, sectors - 1, 2, 0x20);
    CHECK(ata_take_irq(s) == 0x51 && io_port_read(s, 0x1F1, 1) == 0x10);
    io_port_write(s, 0x1F6, 0xA1, 1);       /* CHS, head 1 */
    io_port_write(s, 0x1F2, 1, 1);
    io_port_write(s, 0x1F3, 1, 1);          /* sector 1 */
    io_port_write(s, 0x1F4, 0, 1);
    io_port_write(s, 0x1F5, 0, 1);
    io_port_write(s, 0x1F7, 0x20, 1);
    CHECK(ata_take_irq(s) == 0x58);
    ata_read_sector(s, 63);

    /* nIEN holds the interrupt back without losing it. */
    io_port_write(s, 0x3F6, 2, 1);
    ata_task(s, 0xE0, 0, 1, 0x40);          /* READ VERIFY */
    ata_wait(s);
    CHECK(!irq(s, 0x70) && s->ata_irq_pending);
    io_port_write(s, 0x3F6, 0, 1);
    i2000_poll_interrupts(s);
    CHECK(irq(s, 0x70));
    merced_ack_external(s->cpu, 0x70);
    CHECK(io_port_read(s, 0x1F7, 1) == 0x50);

    /* SRST: ATA disk signature, device 0 selected, no interrupt. */
    io_port_write(s, 0x3F6, 4, 1);
    CHECK(io_port_read(s, 0x3F6, 1) == 0x80);
    io_port_write(s, 0x3F6, 0, 1);
    CHECK(io_port_read(s, 0x1F7, 1) == 0x50 && io_port_read(s, 0x1F1, 1) == 1);
    CHECK(io_port_read(s, 0x1F2, 1) == 1 && io_port_read(s, 0x1F3, 1) == 1);
    CHECK(io_port_read(s, 0x1F4, 1) == 0 && io_port_read(s, 0x1F5, 1) == 0);
    ata_wait(s);
    CHECK(!irq(s, 0x70));

    /* Current snapshot round trip preserves an outstanding ATA command. */
    ata_task(s, 0xE0, 0, 1, 0x40);
    s->cpu->gr_static[8] = UINT64_C(0x0fedcba987654321);
    CHECK(i2000_save_snapshot(s, "irq-current.vmstate"));
    ata_clear_irq(s);
    CHECK(i2000_load_snapshot(s, "irq-current.vmstate"));
    CHECK(s->ata_irq_pending && s->ata_multiple == 2);
    CHECK(s->cpu->gr_static[8] == UINT64_C(0x0fedcba987654321));
    ia64_i2000_destroy(s);
}
static void test_cf9_reset(void) {
    for (unsigned warm = 0; warm < 2; warm++) {
        Ia64I2000State *s = machine();
        /* The same hard-reset signal must work during SAL bootstrap and
         * after an OS has programmed PCI/IDE and fixed SAPIC vectors. */
        s->chipset.sac_cbnr = warm ? 0x81 : 1;
        s->chipset.sac_ccsr = 0x80;
        s->chipset.chipset_cfg[0][0][0x44] = 0x80;
        s->chipset.chipset_cfg[0][0][0x45] = 6;
        s->fw_shadow_enabled = warm;
        merced_set_cpu_revision(s->cpu, warm ? 6 : 0);
        io_port_write(s, PCI_CFG_ADDR, UINT32_C(0x80008040), 4);
        io_port_write(s, PCI_CFG_DATA, 4, 1); /* assigned chipset bus */
        io_port_write(s, PCI_CFG_ADDR, UINT32_C(0x80001910), 4);
        io_port_write(s, PCI_CFG_DATA, 0x3049, 4); /* IDE BAR */
        route_gsi(s, 14, 0x70);
        route_gsi(s, 15, 0x60);
        identify(s);
        CHECK(s->atapi_data && s->atapi_irq_pending);
        io_port_write(s, 0x376, 2, 1); /* IDE nIEN */
        bus_write(s, 0x100000, UINT64_C(0x0123456789abcdef), 8);

        io_port_write(s, RESET_CTRL_PORT, 2, 1);
        CHECK(!s->reset_requested);
        io_port_write(s, RESET_CTRL_PORT, 6, 1);
        CHECK(s->reset_requested == 1);
        CHECK(!i2000_after_step(s, MERCED_OK));
        CHECK(s->chipset.sac_cbnr == (warm ? 0x81u : 1u) && s->chipset.sac_ccsr == 0x80);
        CHECK(s->chipset.chipset_cfg[0][0][0x44] == 0x80 &&
              s->chipset.chipset_cfg[0][0][0x45] == 6);
        CHECK(s->fw_shadow_enabled && s->cpu->ip == IA64_RESET_VECTOR);
        CHECK(s->chipset.chipset_bus == 0xff && s->cmd649_cfg[0x10] == 1);
        for (unsigned i = 0; i < 64; i++) CHECK(s->chipset.iosapic_rte[i] == I460GX_IOSAPIC_RTE_MASK);
        CHECK(!s->atapi_data && !s->atapi_status_pending);
        CHECK(!s->atapi_irq_pending && !s->atapi_device_control);
        CHECK(s->atapi_status == 0x40 && s->atapi_lba_mid == 0x14 &&
              s->atapi_lba_high == 0xeb);
        CHECK(bus_read(s, 0x100000, 8) == UINT64_C(0x0123456789abcdef));
        io_port_write(s, 0x177, 0xa1, 1);
        ready(s);
        /* EFI legacy interrupt routing: reset masks IRQ15, so the request
         * shows in the slave's IRR; unmasking it (and the cascade)
         * delivers the ExtINT. */
        CHECK(!irq(s, 0) && !irq(s, 0x60) && (io_port_read(s, 0xA0, 1) & 0x80));
        io_port_write(s, 0x21, 0xFB, 1);
        io_port_write(s, 0xA1, 0x7F, 1);
        i2000_poll_interrupts(s);
        CHECK(irq(s, 0) && !irq(s, 0x60));

        /* Clearing SYS_RST requests only a processor reset. */
        route_gsi(s, 15, 0x60);
        io_port_write(s, RESET_CTRL_PORT, 4, 1);
        CHECK(s->reset_requested == 2);
        CHECK(!i2000_after_step(s, MERCED_OK));
        CHECK(s->chipset.iosapic_rte[15] == 0x60);
        ia64_i2000_destroy(s);
    }
}

/* A reset clears device state, but must retain host-owned buffers and the
 * pending flash overlay.  Windows Setup exposed a NULL high_dram pointer
 * when the first autosave after a monitor reset wrote its 64 MiB backing. */
static void test_reset_snapshot(void) {
    Ia64I2000State *s = machine();
    uint8_t *high_dram = s->high_dram;
    const uint64_t high = I2000_HIGH_DRAM_TAG | I2000_HIGH_DRAM_BASE;
    const uint64_t tail = high + I2000_HIGH_DRAM_SIZE - 8;
    const uint64_t first_value = UINT64_C(0x0123456789abcdef);
    const uint64_t last_value = UINT64_C(0xfedcba9876543210);
    bus_write(s, high, first_value, 8);
    bus_write(s, tail, last_value, 8);
    snprintf(s->nvram_path, sizeof(s->nvram_path), "reset-nvram.bin");
    s->mouse_enabled = true;

    for (unsigned reset = 0; reset < 2; reset++) {
        s->mouse_streaming = true;
        i2000_reset(s, false);             /* monitor/display reset */
        CHECK(s->high_dram == high_dram);
        CHECK(strcmp(s->nvram_path, "reset-nvram.bin") == 0);
        CHECK(s->mouse_enabled && !s->mouse_streaming);
        CHECK(bus_read(s, high, 8) == first_value);
        CHECK(bus_read(s, tail, 8) == last_value);

        /* Firmware's second bootstrap stage immediately requests CF9.
         * Reproduce the user's monitor reset -> CF9 reset -> autosave. */
        io_port_write(s, RESET_CTRL_PORT, 0x02, 1);
        io_port_write(s, RESET_CTRL_PORT, 0x06, 1);
        CHECK(s->reset_requested);
        CHECK(!i2000_after_step(s, MERCED_OK));
        CHECK(!s->reset_requested && s->fw_shadow_enabled);
        CHECK(s->cpu->ip == IA64_RESET_VECTOR);
        CHECK(i2000_save_snapshot(s, "reset.vmstate"));
        bus_write(s, high, 0, 8);
        bus_write(s, tail, 0, 8);
        CHECK(i2000_load_snapshot(s, "reset.vmstate"));
        CHECK(s->high_dram == high_dram);
        CHECK(bus_read(s, high, 8) == first_value);
        CHECK(bus_read(s, tail, 8) == last_value);
    }
    /* Complete a real flash byte-program command, then reset before
     * shutdown. No firmware file or user's NVRAM is involved. Do this
     * after the snapshot cycles, since loadvm deliberately clears dirty. */
    bus_write(s, I2000_FLASH_BASE + 0x20000, 0x40, 1);
    bus_write(s, I2000_FLASH_BASE + 0x20000, 0x5a, 1);
    CHECK(s->flash[0x20000] == 0x5a && s->nvram_dirty);
    i2000_reset(s, false);
    CHECK(s->nvram_dirty && s->flash[0x20000] == 0x5a);
    CHECK(strcmp(s->nvram_path, "reset-nvram.bin") == 0);
    ia64_i2000_destroy(s);          /* pending flash update must still save */
    FILE *f = fopen("reset-nvram.bin", "rb");
    CHECK(f);
    CHECK(fseek(f, 0x20000, SEEK_SET) == 0);
    CHECK(fgetc(f) == 0x5a);
    CHECK(fclose(f) == 0);
}

int main(void) {
    test_identify(); test_control(); test_packet_phases(); test_legacy(); test_os_mask();
    test_snapshots();
    test_chipset_snapshot();
    test_keyboard();
    test_gpio_straps();
    test_cf9_reset();
    test_reset_snapshot();
    test_ata();   /* last: registers a global -hda drive */
    printf("i2000 ATAPI interrupt regressions: %u checks passed\n", checks);
    return 0;
}
