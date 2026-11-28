/* The 6530 RRIOT as a core device: see m6530_dev.h. */
#include "controller/m6530_dev.h"
#include "controller/m6530_state.h"
#include "gemu/dev/line.h"
#include <inttypes.h>
#include <stdio.h>

typedef struct {
    M6530      chip;
    GemuLine  *irq;
    uint64_t (*cycles)(void *ud);
    void      *cycles_ud;
} Riot;

M6530 *gemu_m6530(GemuDevice *d) { return &((Riot *)d->priv)->chip; }

void gemu_m6530_set_cycles(GemuDevice *d, uint64_t (*cycles)(void *ud), void *ud) {
    Riot *r = d->priv;
    r->cycles = cycles;
    r->cycles_ud = ud;
}

static uint64_t now(const Riot *r) { return r->cycles ? r->cycles(r->cycles_ud) : 0; }

static void irq_changed(void *ud) {
    Riot *r = ud;
    gemu_line_set(r->irq, m6530_irq(&r->chip));
}

void gemu_m6530_tick(GemuDevice *d) {
    Riot *r = d->priv;
    m6530_tick(&r->chip, now(r));
}

static uint8_t io_r8(void *ud, uint64_t off) {
    Riot *r = ((GemuDevice *)ud)->priv;
    return m6530_read(&r->chip, (unsigned)off & 0x0Fu, now(r));
}
static void io_w8(void *ud, uint64_t off, uint8_t v) {
    Riot *r = ((GemuDevice *)ud)->priv;
    m6530_write(&r->chip, (unsigned)off & 0x0Fu, v, now(r));
}
static bool io_peek(void *ud, uint64_t off, uint8_t *v) {
    Riot *r = ((GemuDevice *)ud)->priv;
    *v = m6530_peek(&r->chip, (unsigned)off & 0x0Fu, now(r));
    return true;
}
const GemuHandlers gemu_m6530_io = { .r8 = io_r8, .w8 = io_w8, .peek8 = io_peek };

static bool riot_init(GemuDevice *d) {
    Riot *r = d->priv;
    if (!(r->irq = gemu_line_out(d, "irq")))
        return false;
    r->chip.irq_changed = irq_changed;
    r->chip.irq_ud = r;
    m6530_reset(&r->chip);
    return m6530_state_add(d, "", &r->chip);
}

static void riot_reset(GemuDevice *d, GemuResetKind kind) {
    (void)kind;
    m6530_reset(gemu_m6530(d));
}

static void riot_describe(GemuDevice *d, char *buf, size_t len) {
    Riot *r = d->priv;
    const M6530 *c = &r->chip;
    snprintf(buf, len, "PA %02X (DDR %02X)  PB %02X (DDR %02X)\n"
             "timer %s, /%u, count %02X%s%s",
             c->pa, c->ddra, c->pb, c->ddrb, c->running ? "running" : "stopped",
             (unsigned)c->prescale, m6530_peek(c, 4, now(r)),
             c->irq_flag ? ", flag set" : "", c->irq_en ? ", IRQ enabled" : "");
}

const GemuDeviceType gemu_m6530_type = {
    .name = "m6530", .desc = "MOS 6530 RRIOT (RAM, I/O, timer)", .size = sizeof(Riot),
    .init = riot_init, .reset = riot_reset, .describe = riot_describe,
};
