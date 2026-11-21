/* The 8259A: one chip as the 5150 uses it, and the cascaded pair the i2000
 * uses.  Run: ninja check-i8259 */
#include "controller/i8259.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

/* ICW1 (edge or level, ICW4 follows), ICW2, ICW4; single chip. */
static void init_single(I8259 *p, uint8_t base, bool level, bool aeoi) {
    i8259_reset(p);
    i8259_io_write(p, 0x20, (uint8_t)(0x13 | (level ? 0x08 : 0)));
    i8259_io_write(p, 0x21, base);
    i8259_io_write(p, 0x21, aeoi ? 0x03 : 0x01);
}

static uint8_t irr(I8259 *p) { i8259_io_write(p, 0x20, 0x0A); return i8259_io_read(p, 0x20); }
static uint8_t isr(I8259 *p) { i8259_io_write(p, 0x20, 0x0B); return i8259_io_read(p, 0x20); }

static void test_single(void) {
    I8259 p;
    i8259_reset(&p);
    CHECK(i8259_io_read(&p, 0x21) == 0xFF);           /* reset masks all */
    init_single(&p, 0x08, false, false);
    CHECK(i8259_io_read(&p, 0x21) == 0x00);           /* ICW1 clears IMR */

    /* Edge: a rising line requests; acknowledging puts it in service. */
    i8259_set_irq(&p, 3, true);
    CHECK(irr(&p) == 0x08 && i8259_intr(&p));
    CHECK(i8259_ack(&p) == 0x0B);
    CHECK(irr(&p) == 0 && isr(&p) == 0x08 && !i8259_intr(&p));
    /* Still high: no new edge, so no new request after EOI. */
    i8259_io_write(&p, 0x20, 0x20);
    CHECK(isr(&p) == 0 && !i8259_intr(&p));
    i8259_set_irq(&p, 3, false);
    i8259_set_irq(&p, 3, true);
    CHECK(i8259_intr(&p));

    /* A request goes away if its line drops before the acknowledge, which
     * then finds nothing: a spurious IRQ7, not put in service. */
    i8259_set_irq(&p, 3, false);
    CHECK(irr(&p) == 0 && !i8259_intr(&p));
    CHECK(i8259_ack(&p) == 0x0F && isr(&p) == 0);

    /* Fixed priority: IRQ1 in service holds off IRQ4 but not IRQ0. */
    i8259_set_irq(&p, 1, true);
    CHECK(i8259_ack(&p) == 0x09);
    i8259_set_irq(&p, 4, true);
    CHECK(!i8259_intr(&p) && irr(&p) == 0x10);
    i8259_set_irq(&p, 0, true);
    CHECK(i8259_intr(&p) && i8259_ack(&p) == 0x08);
    CHECK(isr(&p) == 0x03);
    i8259_io_write(&p, 0x20, 0x20);                    /* non-specific: IRQ0 */
    CHECK(isr(&p) == 0x02 && !i8259_intr(&p));
    i8259_io_write(&p, 0x20, 0x61);                    /* specific EOI 1 */
    CHECK(isr(&p) == 0 && i8259_ack(&p) == 0x0C);
    i8259_io_write(&p, 0x20, 0x20);

    /* Masking hides a request from INT but not from IRR. */
    i8259_set_irq(&p, 5, true);
    i8259_io_write(&p, 0x21, 0x20);
    CHECK(irr(&p) & 0x20);
    CHECK(!i8259_intr(&p));
    i8259_io_write(&p, 0x21, 0x00);
    CHECK(i8259_intr(&p));

    /* Poll mode: the read acknowledges and reports the level. */
    i8259_io_write(&p, 0x20, 0x0C);
    CHECK(i8259_io_read(&p, 0x20) == 0x85 && isr(&p) == 0x20);

    /* Level mode: a line still high asks again after the EOI. */
    init_single(&p, 0x08, true, false);
    i8259_set_irq(&p, 6, true);
    CHECK(i8259_ack(&p) == 0x0E && !i8259_intr(&p));
    i8259_io_write(&p, 0x20, 0x20);
    CHECK(i8259_intr(&p));
    i8259_set_irq(&p, 6, false);
    CHECK(!i8259_intr(&p));

    /* Automatic EOI: nothing stays in service. */
    init_single(&p, 0x08, false, true);
    i8259_set_irq(&p, 2, true);
    CHECK(i8259_ack(&p) == 0x0A && isr(&p) == 0);
}

/* As the PC/AT BIOS programs the pair: edge, cascade on IRQ2, 8086 mode. */
static void init_pair(I8259Pair *p) {
    i8259_pair_reset(p);
    const uint8_t seq[] = {0x11, 0x08, 0x04, 0x01};
    const uint8_t sseq[] = {0x11, 0x70, 0x02, 0x01};
    for (unsigned i = 0; i < 4; i++) {
        CHECK(i8259_pair_io_write(p, i ? 0x21 : 0x20, seq[i]));
        CHECK(i8259_pair_io_write(p, i ? 0xA1 : 0xA0, sseq[i]));
    }
}
static uint8_t pair_read(I8259Pair *p, uint16_t port) {
    uint8_t v = 0xEE;
    CHECK(i8259_pair_io_read(p, port, &v));
    return v;
}
static uint8_t pair_irr(I8259Pair *p, uint16_t port) {
    i8259_pair_io_write(p, port, 0x0A);
    return pair_read(p, port);
}
static uint8_t pair_isr(I8259Pair *p, uint16_t port) {
    i8259_pair_io_write(p, port, 0x0B);
    return pair_read(p, port);
}

static void test_pair(void) {
    I8259Pair p;
    uint8_t v;
    i8259_pair_reset(&p);
    CHECK(!i8259_pair_io_read(&p, 0x22, &v) && !i8259_pair_io_write(&p, 0x40, 0));
    CHECK(pair_read(&p, 0x21) == 0xFF && pair_read(&p, 0xA1) == 0xFF);
    init_pair(&p);
    int irq = 99;

    /* A slave request shows on the master's IRQ2 and is acknowledged with
     * the slave's vector, both chips marking it in service. */
    i8259_pair_set_irq(&p, 14, true);
    CHECK(pair_irr(&p, 0xA0) == 0x40 && pair_irr(&p, 0x20) == 0x04);
    CHECK(i8259_pair_intr(&p));
    CHECK(i8259_pair_ack(&p, &irq) == 0x76 && irq == 14);
    CHECK(pair_isr(&p, 0x20) == 0x04 && pair_isr(&p, 0xA0) == 0x40);
    CHECK(!i8259_pair_intr(&p));

    /* IRQ15 waits for IRQ14 to finish; IRQ0 does not. */
    i8259_pair_set_irq(&p, 15, true);
    CHECK(!i8259_pair_intr(&p));
    i8259_pair_set_irq(&p, 0, true);
    CHECK(i8259_pair_ack(&p, &irq) == 0x08 && irq == 0);
    i8259_pair_io_write(&p, 0x20, 0x20);                /* EOI IRQ0 */
    CHECK(!i8259_pair_intr(&p));                         /* master IRQ2 busy */
    i8259_pair_io_write(&p, 0xA0, 0x20);                /* slave EOI */
    CHECK(!i8259_pair_intr(&p));                         /* ... and the master's */
    i8259_pair_io_write(&p, 0x20, 0x20);
    CHECK(i8259_pair_intr(&p));
    CHECK(i8259_pair_ack(&p, &irq) == 0x77 && irq == 15);
    i8259_pair_io_write(&p, 0xA0, 0x20);
    i8259_pair_io_write(&p, 0x20, 0x20);
    CHECK(pair_isr(&p, 0x20) == 0 && pair_isr(&p, 0xA0) == 0);

    /* IRQ2 can't be driven directly. */
    i8259_pair_set_irq(&p, 0, false);
    i8259_pair_set_irq(&p, 14, false);
    i8259_pair_set_irq(&p, 15, false);
    i8259_pair_set_irq(&p, 2, true);
    CHECK(pair_irr(&p, 0x20) == 0 && !i8259_pair_intr(&p));

    /* A masked slave line shows in the slave's IRR only. */
    i8259_pair_io_write(&p, 0xA1, 0x80);
    i8259_pair_set_irq(&p, 15, true);
    CHECK(pair_irr(&p, 0xA0) == 0x80 && pair_irr(&p, 0x20) == 0);
    CHECK(!i8259_pair_intr(&p));
    i8259_pair_io_write(&p, 0xA1, 0x00);                /* unmasking raises it */
    CHECK(pair_irr(&p, 0x20) == 0x04 && i8259_pair_intr(&p));
    /* ... and so does the cascade's own mask. */
    i8259_pair_io_write(&p, 0x21, 0x04);
    CHECK(!i8259_pair_intr(&p));
    i8259_pair_io_write(&p, 0x21, 0x00);
    CHECK(i8259_pair_intr(&p));

    /* The line drops before the acknowledge: spurious, from the master. */
    i8259_pair_set_irq(&p, 15, false);
    CHECK(!i8259_pair_intr(&p));
    CHECK(i8259_pair_ack(&p, &irq) == 0x0F && irq == -1);
    CHECK(pair_isr(&p, 0x20) == 0 && pair_isr(&p, 0xA0) == 0);

    /* A slave request in service, then another slave request after both
     * EOIs: the cascade raises a fresh edge on the master. */
    i8259_pair_set_irq(&p, 12, true);
    CHECK(i8259_pair_ack(&p, &irq) == 0x74 && irq == 12);
    i8259_pair_set_irq(&p, 12, false);
    i8259_pair_set_irq(&p, 9, true);
    /* IRQ9 outranks IRQ12 on the slave, but the master's IRQ2 is in
     * service, so nothing from the slave gets through until its EOI. */
    CHECK(!i8259_pair_intr(&p));
    i8259_pair_io_write(&p, 0x20, 0x20);
    CHECK(i8259_pair_intr(&p));
    CHECK(i8259_pair_ack(&p, &irq) == 0x71 && irq == 9);
    CHECK(pair_isr(&p, 0xA0) == 0x12);
}

int main(void) {
    test_single();
    test_pair();
    printf("i8259: %u checks passed\n", checks);
    return 0;
}
