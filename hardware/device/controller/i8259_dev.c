/* The 8259A PIC as a core device: see i8259_dev.h. */
#include "controller/i8259_dev.h"
#include "gemu/dev/line.h"
#include "gemu/dev/state.h"
#include <stdio.h>

typedef struct {
    I8259       chip;
    GemuLineIn *ir[8];
    GemuLine   *intr;
} Pic;

I8259 *gemu_i8259(GemuDevice *d) { return &((Pic *)d->priv)->chip; }

static void update_int(Pic *p) { gemu_line_set(p->intr, i8259_intr(&p->chip)); }

static void ir_changed(void *ud, int idx, bool level) {
    Pic *p = ud;
    i8259_set_irq(&p->chip, (unsigned)idx, level);
    update_int(p);
}

uint8_t gemu_i8259_ack(GemuDevice *d) {
    Pic *p = d->priv;
    uint8_t v = i8259_ack(&p->chip);
    update_int(p);
    return v;
}

static uint8_t io_r8(void *ud, uint64_t off) {
    Pic *p = ((GemuDevice *)ud)->priv;
    uint8_t v = i8259_io_read(&p->chip, (uint16_t)off);   /* a poll acknowledges */
    update_int(p);
    return v;
}
static void io_w8(void *ud, uint64_t off, uint8_t v) {
    Pic *p = ((GemuDevice *)ud)->priv;
    i8259_io_write(&p->chip, (uint16_t)off, v);
    update_int(p);
}
static bool io_peek(void *ud, uint64_t off, uint8_t *v) {
    const I8259 *c = &((Pic *)((GemuDevice *)ud)->priv)->chip;
    if (off & 1)
        *v = c->imr;
    else if (c->poll)
        return false;                                     /* that read acknowledges */
    else
        *v = c->read_isr ? c->isr : c->irr;
    return true;
}
const GemuHandlers gemu_i8259_io = { .r8 = io_r8, .w8 = io_w8, .peek8 = io_peek };

static bool pic_init(GemuDevice *d) {
    Pic *p = d->priv;
    I8259 *c = &p->chip;
    for (int i = 0; i < 8; i++) {
        char name[16];
        snprintf(name, sizeof name, "ir%d", i);
        if (!(p->ir[i] = gemu_line_in(d, name, ir_changed, p, i)))
            return false;
    }
    if (!(p->intr = gemu_line_out(d, "int")))
        return false;
    GEMU_STATE_VAR(d, "imr", c->imr);
    GEMU_STATE_VAR(d, "irr", c->irr);
    GEMU_STATE_VAR(d, "isr", c->isr);
    GEMU_STATE_VAR(d, "lines", c->lines);
    GEMU_STATE_VAR(d, "vector_base", c->vector_base);
    GEMU_STATE_VAR(d, "auto_eoi", c->auto_eoi);
    GEMU_STATE_VAR(d, "level_mode", c->level_mode);
    GEMU_STATE_VAR(d, "read_isr", c->read_isr);
    GEMU_STATE_VAR(d, "poll", c->poll);
    GEMU_STATE_VAR(d, "icw_step", c->icw_step);
    GEMU_STATE_VAR(d, "single", c->single);
    GEMU_STATE_VAR(d, "expect_icw4", c->expect_icw4);
    return true;
}

static void pic_reset(GemuDevice *d, GemuResetKind kind) {
    (void)kind;
    Pic *p = d->priv;
    i8259_reset(&p->chip);
    /* the pins keep their levels across a reset */
    for (int i = 0; i < 8; i++)
        if (gemu_line_in_level(p->ir[i]))
            i8259_set_irq(&p->chip, (unsigned)i, true);
    update_int(p);
}

static void pic_describe(GemuDevice *d, char *buf, size_t len) {
    const I8259 *c = &((Pic *)d->priv)->chip;
    snprintf(buf, len,
             "IRR %02X  ISR %02X  IMR %02X  pins %02X  INT %s\n"
             "vector base %02X, %s-triggered%s%s%s",
             c->irr, c->isr, c->imr, c->lines, i8259_intr(c) ? "high" : "low",
             c->vector_base, c->level_mode ? "level" : "edge",
             c->auto_eoi ? ", automatic EOI" : "",
             c->icw_step ? ", waiting for ICW" : "",
             c->icw_step == 2 ? "2" : c->icw_step == 3 ? "3" : c->icw_step == 4 ? "4" : "");
}

const GemuDeviceType gemu_i8259_type = {
    .name = "i8259", .desc = "Intel 8259A programmable interrupt controller",
    .size = sizeof(Pic), .init = pic_init, .reset = pic_reset, .describe = pic_describe,
};
