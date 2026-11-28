/* The 6821 PIA as a core device: see pia6821_dev.h. */
#include "controller/pia6821_dev.h"
#include "controller/pia6821_state.h"
#include "gemu/dev/line.h"
#include <stdio.h>

typedef struct {
    Pia6821     chip;
    GemuLine   *irqa, *irqb;
    GemuLineIn *c[4];
} Pia;

Pia6821 *gemu_pia6821(GemuDevice *d) { return &((Pia *)d->priv)->chip; }

static void irq_a(void *ud, bool level) { gemu_line_set(((Pia *)ud)->irqa, level); }
static void irq_b(void *ud, bool level) { gemu_line_set(((Pia *)ud)->irqb, level); }

static void c_in(void *ud, int idx, bool level) {
    Pia6821 *p = &((Pia *)ud)->chip;
    switch (idx) {
    case 0: pia6821_set_ca1(p, level); break;
    case 1: pia6821_set_cb1(p, level); break;
    case 2: pia6821_set_ca2(p, level); break;
    default: pia6821_set_cb2(p, level); break;
    }
}

static uint8_t bus_r8(void *ud, uint64_t off) {
    return pia6821_read(gemu_pia6821(ud), (unsigned)off & 3u);
}
static void bus_w8(void *ud, uint64_t off, uint8_t v) {
    pia6821_write(gemu_pia6821(ud), (unsigned)off & 3u, v);
}
static bool bus_peek(void *ud, uint64_t off, uint8_t *v) {
    *v = pia6821_peek(gemu_pia6821(ud), (unsigned)off & 3u);
    return true;
}
const GemuHandlers gemu_pia6821_bus = { .r8 = bus_r8, .w8 = bus_w8, .peek8 = bus_peek };

static bool pia_init(GemuDevice *d) {
    Pia *p = d->priv;
    p->chip.set_irq_a = irq_a;
    p->chip.set_irq_b = irq_b;
    p->chip.irq_ud = p;
    pia6821_init(&p->chip);
    static const char *const names[4] = { "ca1", "cb1", "ca2", "cb2" };
    for (int i = 0; i < 4; i++)
        if (!(p->c[i] = gemu_line_in(d, names[i], c_in, p, i)))
            return false;
    return (p->irqa = gemu_line_out(d, "irqa")) && (p->irqb = gemu_line_out(d, "irqb")) &&
           pia6821_state_add(d, "", &p->chip);
}

static void pia_reset(GemuDevice *d, GemuResetKind kind) {
    (void)kind;
    Pia *p = d->priv;
    pia6821_init(&p->chip);
    gemu_line_set(p->irqa, false);
    gemu_line_set(p->irqb, false);
}

static void pia_describe(GemuDevice *d, char *buf, size_t len) {
    const Pia6821 *p = &((Pia *)d->priv)->chip;
    snprintf(buf, len, "A: OR %02X DDR %02X CR %02X%s   B: OR %02X DDR %02X CR %02X%s\n"
             "CA1 %d CA2 %d CB1 %d CB2 %d",
             p->ora, p->ddra, p->cra, p->irq_a ? " IRQ" : "",
             p->orb, p->ddrb, p->crb, p->irq_b ? " IRQ" : "",
             p->ca1, p->ca2, p->cb1, p->cb2);
}

const GemuDeviceType gemu_pia6821_type = {
    .name = "pia6821", .desc = "Motorola 6820/6821 PIA", .size = sizeof(Pia),
    .init = pia_init, .reset = pia_reset, .describe = pia_describe,
};
