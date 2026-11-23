/* The 6821 PIA.  Run: ninja check-pia6821 */
#include "controller/pia6821.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

typedef struct {
    uint8_t pa_in, pb_in, pa_out, pb_out;
    unsigned pa_writes, pb_writes;
    bool ca2, cb2, irq_a, irq_b;
    unsigned ca2_changes, cb2_changes;
} Board;

static uint8_t read_pa(void *ud) { return ((Board *)ud)->pa_in; }
static uint8_t read_pb(void *ud) { return ((Board *)ud)->pb_in; }
static void write_pa(void *ud, uint8_t v) { Board *b = ud; b->pa_out = v; b->pa_writes++; }
static void write_pb(void *ud, uint8_t v) { Board *b = ud; b->pb_out = v; b->pb_writes++; }
static void write_ca2(void *ud, bool l) { Board *b = ud; b->ca2 = l; b->ca2_changes++; }
static void write_cb2(void *ud, bool l) { Board *b = ud; b->cb2 = l; b->cb2_changes++; }
static void irq_a(void *ud, bool l) { ((Board *)ud)->irq_a = l; }
static void irq_b(void *ud, bool l) { ((Board *)ud)->irq_b = l; }

static Pia6821 make(Board *b) {
    Pia6821 p = { .read_pa = read_pa, .read_pb = read_pb, .write_pa = write_pa,
                  .write_pb = write_pb, .write_ca2 = write_ca2, .write_cb2 = write_cb2,
                  .set_irq_a = irq_a, .set_irq_b = irq_b, .ud = b };
    pia6821_init(&p);
    return p;
}

static void test_ports(void) {
    Board b = {0};
    Pia6821 p = make(&b);
    /* Reset: DDRs selected; writes set them. */
    pia6821_write(&p, 0, 0x0F);
    CHECK(pia6821_read(&p, 0) == 0x0F && p.ddra == 0x0F);
    pia6821_write(&p, 1, 0x04);                       /* select ORA */
    pia6821_write(&p, 0, 0xA5);
    CHECK(b.pa_out == 0x05 && b.pa_writes == 1);      /* output pins only */
    b.pa_in = 0x70;
    CHECK(pia6821_read(&p, 0) == 0x75);               /* outputs | input pins */
    /* Port B the same way. */
    pia6821_write(&p, 2, 0xF0);
    pia6821_write(&p, 3, 0x04);
    pia6821_write(&p, 2, 0x3C);
    CHECK(b.pb_out == 0x30);
    b.pb_in = 0x0F;
    CHECK(pia6821_read(&p, 2) == 0x3F);
    /* Re-init keeps the wiring and clears the registers. */
    pia6821_init(&p);
    CHECK(p.ddra == 0 && p.cra == 0 && p.read_pa == read_pa && p.ud == &b);
}

static void test_c1(void) {
    Board b = {0};
    Pia6821 p = make(&b);
    pia6821_write(&p, 1, 0x07);                       /* ORA, rising, IRQ on */
    pia6821_set_ca1(&p, false);
    CHECK(!(pia6821_read(&p, 1) & 0x80));
    pia6821_set_ca1(&p, true);                        /* active edge */
    CHECK((pia6821_read(&p, 1) & 0x80) && b.irq_a);
    pia6821_set_ca1(&p, false);                       /* the other edge: nothing new */
    CHECK(pia6821_peek(&p, 1) & 0x80);
    /* The flags are read-only; peeking at ORA doesn't clear them. */
    pia6821_write(&p, 1, 0xC7);
    CHECK(pia6821_peek(&p, 1) == 0x87);
    pia6821_peek(&p, 0);
    CHECK(pia6821_peek(&p, 1) & 0x80);
    /* Reading ORA clears them and the IRQ. */
    pia6821_read(&p, 0);
    CHECK(!(pia6821_peek(&p, 1) & 0x80) && !b.irq_a);
    /* With the IRQ disabled the flag still sets. */
    pia6821_write(&p, 1, 0x06);
    pia6821_set_ca1(&p, true);
    CHECK((pia6821_peek(&p, 1) & 0x80) && !b.irq_a);
    pia6821_write(&p, 1, 0x07);                       /* enabling raises it */
    CHECK(b.irq_a);
    /* Reading the DDR doesn't clear the flags. */
    pia6821_write(&p, 1, 0x03);
    pia6821_read(&p, 0);
    CHECK(pia6821_peek(&p, 1) & 0x80);
    /* Falling edge, port B. */
    pia6821_write(&p, 3, 0x05);
    pia6821_set_cb1(&p, true);
    CHECK(!(pia6821_peek(&p, 3) & 0x80));
    pia6821_set_cb1(&p, false);
    CHECK((pia6821_peek(&p, 3) & 0x80) && b.irq_b);
    pia6821_read(&p, 2);
    CHECK(!b.irq_b);
}

static void test_c2(void) {
    Board b = {0};
    Pia6821 p = make(&b);
    /* Input, rising edge, IRQ enabled: IRQ2 in bit 6. */
    pia6821_write(&p, 1, 0x1C);
    pia6821_set_ca2(&p, true);
    CHECK((pia6821_peek(&p, 1) & 0x40) && b.irq_a);
    pia6821_read(&p, 0);
    CHECK(!(pia6821_peek(&p, 1) & 0x40) && !b.irq_a);

    /* Manual output: CA2 follows bit 3; inputs are ignored. */
    pia6821_write(&p, 1, 0x3C);
    CHECK(b.ca2);
    pia6821_write(&p, 1, 0x34);
    CHECK(!b.ca2);
    pia6821_set_ca2(&p, true);
    CHECK(!(pia6821_peek(&p, 1) & 0x40) && !b.ca2);

    /* Handshake: an ORA read pulls CA2 low; the active CA1 edge restores it. */
    pia6821_write(&p, 1, 0x26);                       /* rising CA1, ORA */
    CHECK(b.ca2);
    pia6821_read(&p, 0);
    CHECK(!b.ca2);
    pia6821_set_ca1(&p, false);
    pia6821_set_ca1(&p, true);
    CHECK(b.ca2);

    /* Pulse: low for one access, straight back. */
    pia6821_write(&p, 1, 0x2C);
    unsigned before = b.ca2_changes;
    pia6821_read(&p, 0);
    CHECK(b.ca2 && b.ca2_changes == before + 2);

    /* Port B strobes on an ORB write, not a read. */
    pia6821_write(&p, 3, 0x24);
    CHECK(b.cb2);
    pia6821_read(&p, 2);
    CHECK(b.cb2);
    pia6821_write(&p, 2, 0x55);
    CHECK(!b.cb2);
    pia6821_set_cb1(&p, false);                       /* the active edge (falling) */
    CHECK(!b.cb2);                                    /* was already low */
    pia6821_set_cb1(&p, true);
    pia6821_set_cb1(&p, false);
    CHECK(b.cb2);
}

/* The Apple I's use: a key strobe on CA1, the display on port B. */
static void test_apple1(void) {
    Board b = {0};
    Pia6821 p = make(&b);
    pia6821_write(&p, 2, 0x7F);                       /* DDRB: PB7 an input */
    pia6821_write(&p, 1, 0xA7);
    pia6821_write(&p, 3, 0xA7);
    CHECK(pia6821_read(&p, 1) == 0x27);               /* no key yet */
    b.pa_in = 0xC1;                                   /* 'A' with bit 7 */
    pia6821_set_ca1(&p, true);
    pia6821_set_ca1(&p, false);
    CHECK(pia6821_read(&p, 1) & 0x80);
    CHECK(pia6821_read(&p, 0) == 0xC1);
    CHECK(!(pia6821_read(&p, 1) & 0x80));
    CHECK(!(pia6821_read(&p, 2) & 0x80));             /* display not busy */
    pia6821_write(&p, 2, 0xC1);
    CHECK(b.pb_out == 0x41);
}

int main(void) {
    test_ports();
    test_c1();
    test_c2();
    test_apple1();
    printf("pia6821: %u checks passed\n", checks);
    return 0;
}
