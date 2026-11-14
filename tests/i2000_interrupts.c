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
        /* Firmware enables the IFB's GPIO block at 400h. J29 drives three
         * input pins; the rest read their pull-ups. Both active-low board
         * straps must stay deasserted, including when firmware reads the high
         * bytes of the GPIO data register. */
        io_port_write(s, PCI_CFG_ADDR, UINT32_C(0x800018d0), 4);
        io_port_write(s, PCI_CFG_DATA, 0x401, 4);
        io_port_write(s, PCI_CFG_ADDR, UINT32_C(0x800018d4), 4);
        io_port_write(s, PCI_CFG_DATA, 1, 4);
        CHECK(bus_read(s, gpio, 4) == UINT32_C(0x010401ff));
        CHECK(bus_read(s, gpio, 2) == 0x01ff);
        CHECK(bus_read(s, gpio + 2, 2) == 0x0104);
        CHECK(bus_read(s, gpio + 2, 1) == 4);
        CHECK(bus_read(s, gpio + 3, 1) == 1);
        CHECK(bus_read(s, gpio | UINT64_C(0xc000000000000000), 4) ==
              UINT32_C(0x010401ff));
        /* Inputs do not change on software writes. */
        bus_write(s, gpio, 0, 4);
        CHECK(bus_read(s, gpio, 4) == UINT32_C(0x010401ff));
        bus_write(s, gpio + 2, 0xffff, 2);
        bus_write(s, gpio + 3, 0, 1);
        CHECK(bus_read(s, gpio, 4) == UINT32_C(0x010401ff));
        i2000_reset(s, false);
    }
    ia64_i2000_destroy(s);
}
/* The IFB's real-time clock: a real update cycle with UIP ahead of it, a
 * running time, periodic interrupts through IRQ8 and the RTC_STS event, and
 * state that survives a processor reset. */
static uint64_t itc_for_oscillator(uint64_t counts) {
    return counts * I2000_ITC_HZ / 32768;
}
static uint8_t rtc_read(Ia64I2000State *s, uint8_t index) {
    io_port_write(s, 0x70, index, 1);
    return (uint8_t)io_port_read(s, 0x71, 1);
}
static void rtc_write(Ia64I2000State *s, uint8_t index, uint8_t value) {
    io_port_write(s, 0x70, index, 1);
    io_port_write(s, 0x71, value, 1);
}
static uint64_t itc_for_pit(uint64_t clocks) {
    return clocks * UINT64_C(733084000) / UINT64_C(1193182);
}
static uint8_t pit_count(Ia64I2000State *s, unsigned channel) {
    io_port_write(s, 0x43, channel << 6, 1);                /* latch */
    uint8_t lo = (uint8_t)io_port_read(s, 0x40 + channel, 1);
    uint8_t hi = (uint8_t)io_port_read(s, 0x40 + channel, 1);
    return (uint8_t)(((unsigned)hi << 8 | lo) >> 8);
}
/* The interval timer counts the ITC down at 1.193182 MHz, drives IRQ0 and
 * channel 2 reports through port 61h. */
static void test_pit(void) {
    Ia64I2000State *s = machine();
    s->cpu->ar[44] = 0;
    io_port_write(s, 0x43, 0x34, 1);          /* channel 0, mode 2, lo/hi */
    io_port_write(s, 0x40, 0x00, 1);
    io_port_write(s, 0x40, 0x10, 1);          /* count 4096 */
    s->cpu->ar[44] = itc_for_pit(1000);
    uint8_t high = pit_count(s, 0);
    CHECK(high == 0x0c);                      /* 4096 - 1000 - 1 */
    s->cpu->ar[44] = itc_for_pit(3000);
    CHECK(pit_count(s, 0) < high);

    /* A rising edge at the end of the period latches IRQ0 behind the mask. */
    io_port_write(s, 0x21, 0x01, 1);
    CHECK(!s->pit_irq_pending);
    s->cpu->ar[44] = itc_for_pit(5000);
    i2000_poll_interrupts(s);
    CHECK(s->pit_irq_pending && !irq(s, 0));
    io_port_write(s, 0x21, 0x00, 1);          /* unmask */
    CHECK(irq(s, 0));

    /* Channel 2: gated through port 61h, output visible in bit 5. */
    io_port_write(s, 0x43, 0xb0, 1);          /* channel 2, mode 0, lo/hi */
    io_port_write(s, 0x42, 0x40, 1);
    io_port_write(s, 0x42, 0x00, 1);          /* count 64 */
    io_port_write(s, 0x61, 0x01, 1);          /* gate on */
    CHECK(!(io_port_read(s, 0x61, 1) & 0x20));
    s->cpu->ar[44] += itc_for_pit(100);
    CHECK(io_port_read(s, 0x61, 1) & 0x20);
    /* The refresh toggle alternates about every 15 us. */
    uint8_t first = io_port_read(s, 0x61, 1) & 0x10;
    s->cpu->ar[44] += itc_for_pit(18);
    CHECK((io_port_read(s, 0x61, 1) & 0x10) != first);
    ia64_i2000_destroy(s);
}
static void test_rtc(void) {
    Ia64I2000State *s = machine();
    /* The divider starts half a second from an update: UIP rises 16 counts
     * before it and the time advances when the 65-count update ends. */
    s->cpu->ar[44] = 0;
    CHECK(!(rtc_read(s, 0x0a) & 0x80));
    uint8_t seconds = rtc_read(s, 0x00);
    s->cpu->ar[44] = itc_for_oscillator(16384 - 8);
    CHECK(rtc_read(s, 0x0a) & 0x80);
    s->cpu->ar[44] = itc_for_oscillator(16384 + 30);
    CHECK(rtc_read(s, 0x0a) & 0x80);                 /* mid-update */
    s->cpu->ar[44] = itc_for_oscillator(16384 + 200);
    CHECK(!(rtc_read(s, 0x0a) & 0x80));
    CHECK(rtc_read(s, 0x00) != seconds);
    /* Register D reports valid RAM; C reads zero with nothing enabled. */
    CHECK(rtc_read(s, 0x0d) & 0x80);

    /* Periodic interrupt: RS = 6 is 1 ms. IRQ8 stays up, and RTC_STS is set,
     * until register C is read. */
    rtc_write(s, 0x0b, 0x42);                        /* PIE, 24-hour BCD */
    s->cpu->ar[44] += itc_for_oscillator(200);
    rtc_write(s, 0x0b, 0x42);                        /* a poll point */
    CHECK(s->rtc_irq);
    CHECK(s->chipset.pm.pm1_sts & 0x0400);
    CHECK(rtc_read(s, 0x0c) & 0xc0);
    CHECK(!s->rtc_irq && rtc_read(s, 0x0c) == 0);

    /* Alarm: seconds, minutes and hours matching raise AF at the update. */
    rtc_write(s, 0x0b, 0x22);                        /* AIE, 24-hour */
    rtc_write(s, 0x01, 0xc0);                        /* don't care */
    rtc_write(s, 0x03, 0xc0);
    rtc_write(s, 0x05, 0xc0);
    s->cpu->ar[44] += itc_for_oscillator(32768);
    CHECK(rtc_read(s, 0x0c) & 0xa0);                 /* IRQF and AF */

    /* The extended bank needs the upper-128 enable and is separate RAM. */
    io_port_write(s, 0x72, 0x10, 1);
    io_port_write(s, 0x73, 0x5a, 1);                 /* aliases 70h/71h */
    io_port_write(s, PCI_CFG_ADDR, UINT32_C(0x800018c8), 4);
    io_port_write(s, PCI_CFG_DATA, 0x04, 1);         /* U128E */
    io_port_write(s, 0x72, 0x10, 1);
    io_port_write(s, 0x73, 0xa5, 1);
    io_port_write(s, 0x72, 0x10, 1);
    CHECK(io_port_read(s, 0x73, 1) == 0xa5);
    CHECK(rtc_read(s, 0x10) == 0x5a);                /* standard bank intact */

    /* The cell keeps its RAM across a processor reset, and the clock runs. */
    rtc_write(s, 0x20, 0x77);
    uint8_t before = rtc_read(s, 0x00);
    s->cpu->ar[44] = itc_for_oscillator(3 * 32768);
    i2000_reset(s, false);
    CHECK(rtc_read(s, 0x20) == 0x77);
    s->cpu->ar[44] = itc_for_oscillator(2 * 32768);
    CHECK(rtc_read(s, 0x00) != before);
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
    /* Modifiers are real held keys: Setup's Shift+F10 opens a console. */
    ps2_keyboard_key(&s->ps2_keyboard, 0xffe1, true);   /* Left Shift */
    ps2_keyboard_tap(&s->ps2_keyboard, 0xffc7);          /* F10 */
    ps2_keyboard_key(&s->ps2_keyboard, 0xffe1, false);
    CHECK(io_port_read(s, 0x60, 1) == 0x2A && io_port_read(s, 0x60, 1) == 0x44 &&
          io_port_read(s, 0x60, 1) == 0xC4 && io_port_read(s, 0x60, 1) == 0xAA);
    ps2_keyboard_tap(&s->ps2_keyboard, 0xffe4);          /* Right Ctrl */
    CHECK(io_port_read(s, 0x60, 1) == 0xE0 && io_port_read(s, 0x60, 1) == 0x1D &&
          io_port_read(s, 0x60, 1) == 0xE0 && io_port_read(s, 0x60, 1) == 0x9D);
    i2000_type_key(s, 'b');
    i2000_poll_interrupts(s);
    CHECK(i2000_save_snapshot(s, "kbd.vmstate"));
    s->keyboard_gsi_asserted = false;
    CHECK(i2000_load_snapshot(s, "kbd.vmstate"));
    CHECK(s->keyboard_gsi_asserted);
    ia64_i2000_destroy(s);
}
/* Windows routes the 8042's aux output (IRQ12) through GSI12 too.  Without
 * it, an unread mouse byte sits first in the single 8042 output buffer and
 * starves the keyboard as well.  Like IRQ1: one edge per byte, re-armed by a
 * data-port read; and the 8042 only drives IRQ12 with command-byte INT2. */
static void test_mouse(void) {
    Ia64I2000State *s = machine();
    route_gsi(s, 1, 0x80);
    route_gsi(s, 12, 0x90);
    s->kbc_command_byte &= (uint8_t)~0x02;
    aux_queue_byte(s, 0xfa);           /* reset reply: ACK, AA, 00 */
    aux_queue_byte(s, 0xaa);
    aux_queue_byte(s, 0x00);
    i2000_poll_interrupts(s);
    CHECK(!irq(s, 0x90) && !irq(s, 0)); /* INT2 off: polled, no IRQ12 */
    s->kbc_command_byte |= 0x02;
    i2000_poll_interrupts(s);
    CHECK(irq(s, 0x90) && s->mouse_gsi_asserted);
    merced_ack_external(s->cpu, 0x90);
    i2000_poll_interrupts(s);
    CHECK(!irq(s, 0x90));              /* acceptance is not a new edge */
    i2000_type_key(s, 'a');            /* queued behind the aux bytes */
    CHECK(io_port_read(s, 0x60, 1) == 0xfa && !s->mouse_gsi_asserted);
    i2000_poll_interrupts(s);
    CHECK(irq(s, 0x90));               /* next aux byte: fresh edge */
    merced_ack_external(s->cpu, 0x90);
    CHECK(io_port_read(s, 0x60, 1) == 0xaa);
    CHECK(io_port_read(s, 0x60, 1) == 0x00);
    if (irq(s, 0x80)) merced_ack_external(s->cpu, 0x80);
    i2000_poll_interrupts(s);
    CHECK(!irq(s, 0x90));              /* aux drained */
    CHECK(irq(s, 0x80));               /* the keyboard byte is reachable */
    CHECK(io_port_read(s, 0x60, 1) == 0x1e);
    route_gsi(s, 12, 0x90 | I460GX_IOSAPIC_RTE_MASK);
    aux_queue_byte(s, 0x08);
    s->cpu->ninsts += 1000000;
    i2000_poll_interrupts(s);
    CHECK(!irq(s, 0x90) && !irq(s, 0)); /* masked OS vector: no fallback */
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
    /* Each board's Scratch EEPROM (SMBus slave 0x50) holds its own byte. */
    source->cart_scratch.mem[2] = 4;
    bus_write(source, I460GX_SAC_CBNR, 0x81, 4);
    bus_write(source, I460GX_SAC_CCSR, 0x80, 4);
    pci_write(source, 0, 0x10, 0, 0x40, 0x32); /* CBN */
    pci_write(source, 0x32, 0, 0, 0x44, 0x680);
    pci_write(source, 0x32, 5, 4, 0x48, 1);   /* memory-card row select */
    pci_write(source, 0, 3, 3, 0x20, 0x1a01); /* SMBus */
    pci_write(source, 0, 3, 3, 4, 1);
    pci_write(source, 0, 3, 3, 0x40, 1);      /* host enable */
    io_port_write(source, 0x1a03, 2, 1);
    io_port_write(source, 0x1a04, 0xa1, 1);
    io_port_write(source, 0x1a02, 0x48, 1);
    pci_write(source, 0, 3, 0, 0x40, 0xa01);  /* PM */
    pci_write(source, 0, 3, 0, 0x44, 1);
    io_port_write(source, 0xa04, 0x401, 2);   /* SLP_TYP and SCI_EN */
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
    s->cart_scratch.mem[2] = 0x5a;
    io_port_write(s, 0x1a02, 0x48, 1);
    CHECK(io_port_read(s, 0x1a05, 1) == 0x5a);
    CHECK(io_port_read(source, 0x1a05, 1) == 4);
    io_port_write(s, 0xa04, 0x400, 2);        /* SCI_EN off on this board */
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
 * pending flash overlay, and RAM contents survive a monitor reset. */
static void test_reset_snapshot(void) {
    Ia64I2000State *s = machine();
    const uint64_t high = 0x100000;
    const uint64_t tail = 0x1000000;
    const uint64_t first_value = UINT64_C(0x0123456789abcdef);
    const uint64_t last_value = UINT64_C(0xfedcba9876543210);
    bus_write(s, high, first_value, 8);
    bus_write(s, tail, last_value, 8);
    snprintf(s->nvram_path, sizeof(s->nvram_path), "reset-nvram.bin");
    s->mouse_enabled = true;

    for (unsigned reset = 0; reset < 2; reset++) {
        s->mouse_streaming = true;
        i2000_reset(s, false);             /* monitor/display reset */
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
        CHECK(bus_read(s, high, 8) == first_value);
        CHECK(bus_read(s, tail, 8) == last_value);
    }
    /* Complete a real flash byte-program command, then reset before
     * shutdown. No firmware file or user's NVRAM is involved. Do this
     * after the snapshot cycles, since loadvm deliberately clears dirty. */
    /* The IFB gates flash writes on BIOS_WEN, and each hub block starts
     * write-locked; the firmware's flash driver unlocks a block with 91h and
     * a write of the lock register at block+2. */
    bus_write(s, I2000_FLASH_BASE + 0x20000, 0x40, 1);
    bus_write(s, I2000_FLASH_BASE + 0x20000, 0x5a, 1);
    CHECK(s->flash[0x20000] == 0xff && !s->nvram_dirty);
    io_port_write(s, PCI_CFG_ADDR, UINT32_C(0x8000184c), 4);
    io_port_write(s, PCI_CFG_DATA + 2, 0xc5, 1);        /* BIOSEN.BIOS_WEN */
    bus_write(s, I2000_FLASH_BASE + 0x20000, 0xff, 1);
    bus_write(s, I2000_FLASH_BASE + 0x20000, 0x40, 1);
    bus_write(s, I2000_FLASH_BASE + 0x20000, 0x5a, 1);
    CHECK(s->flash[0x20000] == 0xff);                   /* write-locked block */
    bus_write(s, I2000_FLASH_BASE + 0x20000, 0xff, 1);
    bus_write(s, I2000_FLASH_BASE + 0x20000, 0x91, 1);
    bus_write(s, I2000_FLASH_BASE + 0x20002, 0x00, 1);
    bus_write(s, I2000_FLASH_BASE + 0x20000, 0xff, 1);
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
    test_mouse();
    test_gpio_straps();
    test_pit();
    test_rtc();
    test_cf9_reset();
    test_reset_snapshot();
    test_ata();   /* last: registers a global -hda drive */
    printf("i2000 ATAPI interrupt regressions: %u checks passed\n", checks);
    return 0;
}
