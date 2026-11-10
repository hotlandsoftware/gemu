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
static void route(Ia64I2000State *s, unsigned vector) {
    bus_write(s, IOSAPIC_BASE + IOSAPIC_REGSEL_OFF, 0x2e, 4);
    bus_write(s, IOSAPIC_BASE + IOSAPIC_WINDOW_OFF, vector, 4);
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
    identify(s); /* Reset RTE15 is masked: firmware's ExtINT path. */
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
    route(s, 0x60 | IOSAPIC_RTE_MASK);
    identify(s);
    CHECK(!irq(s, 0x60) && !irq(s, 0));
    CHECK(s->atapi_irq_pending && !s->atapi_irq_asserted);
    route(s, 0x60);
    i2000_poll_interrupts(s);
    CHECK(irq(s, 0x60) && !irq(s, 0));
    ia64_i2000_destroy(s);
}
/* Produce exact prior-version tails from the current snapshot, preserving
 * the CPU position. This catches struct padding and migration mistakes. */
static void old_snapshot(unsigned version, size_t old_size) {
    FILE *in = fopen("irq-current.vmstate", "rb");
    FILE *out = fopen("irq-old.vmstate", "wb");
    CHECK(in && out);
    CHECK(!fseek(in, 0, SEEK_END));
    long length = ftell(in);
    rewind(in);
    long prefix = length - sizeof(Merced) - sizeof(Ia64I2000State) + old_size;
    uint8_t bytes[65536];
    for (long left = prefix; left;) {
        size_t n = left > (long)sizeof(bytes) ? sizeof(bytes) : (size_t)left;
        CHECK(fread(bytes, 1, n, in) == n && fwrite(bytes, 1, n, out) == n);
        left -= n;
    }
    CHECK(!fseek(in, length - sizeof(Merced), SEEK_SET));
    for (size_t left = sizeof(Merced); left;) {
        size_t n = left > sizeof(bytes) ? sizeof(bytes) : left;
        CHECK(fread(bytes, 1, n, in) == n && fwrite(bytes, 1, n, out) == n);
        left -= n;
    }
    uint32_t v = version;
    CHECK(!fseek(out, 8, SEEK_SET));
    CHECK(fwrite(&v, sizeof(v), 1, out) == 1);
    fclose(in); fclose(out);
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
    const size_t old_sizes[] = {
        offsetof(Ia64I2000State, acpi_pm),
        offsetof(Ia64I2000State, uhci_regs),
        offsetof(Ia64I2000State, atapi_device_control),
    };
    for (unsigned version = 24; version <= 26; version++) {
        old_snapshot(version, old_sizes[version - 24]);
        CHECK(i2000_load_snapshot(s, "irq-old.vmstate"));
        CHECK(s->cpu->gr_static[8] == UINT64_C(0x123456789abcdef0));
        CHECK(s->atapi_device_control == 0 && !s->atapi_irq_asserted);
        CHECK(!s->atapi_packet_command && s->atapi_irq_pending);
        i2000_poll_interrupts(s);
        CHECK(irq(s, 0x60));
    }
    ia64_i2000_destroy(s);
}
int main(void) {
    test_identify(); test_control(); test_packet_phases(); test_legacy(); test_os_mask();
    test_snapshots();
    printf("i2000 ATAPI interrupt regressions: %u checks passed\n", checks);
    return 0;
}
