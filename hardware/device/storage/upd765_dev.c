/* The uPD765 floppy controller as a core device: see upd765_dev.h. */
#include "storage/upd765_dev.h"
#include "controller/i8237_dev.h"
#include "gemu/dev/line.h"
#include "gemu/dev/state.h"
#include <stdio.h>

typedef struct {
    Upd765      chip;
    GemuLine   *intr;
    GemuDevice *dma;
    unsigned    dma_ch;
} Fdc;

Upd765 *gemu_upd765(GemuDevice *d) { return &((Fdc *)d->priv)->chip; }
void gemu_upd765_tick(GemuDevice *d, uint32_t c) { upd765_tick(&((Fdc *)d->priv)->chip, c); }

void gemu_upd765_set_dma(GemuDevice *d, GemuDevice *dma, unsigned ch) {
    Fdc *p = d->priv;
    p->dma = dma;
    p->dma_ch = ch;
}

static void set_irq(void *ud, bool level) { gemu_line_set(((Fdc *)ud)->intr, level); }
static bool dma_from_mem(void *ud, uint8_t *val, bool *tc) {
    Fdc *p = ud;
    return p->dma && gemu_i8237_from_mem(p->dma, p->dma_ch, val, tc);
}
static bool dma_to_mem(void *ud, uint8_t val, bool *tc) {
    Fdc *p = ud;
    return p->dma && gemu_i8237_to_mem(p->dma, p->dma_ch, val, tc);
}

static Upd765 *chip_of(void *ud) { return &((Fdc *)((GemuDevice *)ud)->priv)->chip; }
static uint8_t io_r8(void *ud, uint64_t off) { return upd765_read(chip_of(ud), (uint16_t)(0x3F0 + off)); }
static void io_w8(void *ud, uint64_t off, uint8_t v) { upd765_write(chip_of(ud), (uint16_t)(0x3F0 + off), v); }
/* Only the status register can be looked at: the data register moves
 * through the command and result bytes. */
static bool io_peek(void *ud, uint64_t off, uint8_t *v) {
    if ((off & 7) != 4)
        return false;
    *v = io_r8(ud, off);
    return true;
}
const GemuHandlers gemu_upd765_io = { .r8 = io_r8, .w8 = io_w8, .peek8 = io_peek };

static bool fdc_init(GemuDevice *d) {
    Fdc *p = d->priv;
    Upd765 *c = &p->chip;
    if (!(p->intr = gemu_line_out(d, "int")))
        return false;
    c->set_irq = set_irq;
    c->dma_from_mem = dma_from_mem;
    c->dma_to_mem = dma_to_mem;
    c->ud = p;
    GEMU_STATE_VAR(d, "dor", c->dor);
    GEMU_STATE_ARRAY(d, "cmd", c->cmd);
    GEMU_STATE_VAR(d, "cmd_len", c->cmd_len);
    GEMU_STATE_VAR(d, "cmd_need", c->cmd_need);
    GEMU_STATE_ARRAY(d, "res", c->res);
    GEMU_STATE_VAR(d, "res_len", c->res_len);
    GEMU_STATE_VAR(d, "res_pos", c->res_pos);
    GEMU_STATE_ARRAY(d, "pcn", c->pcn);
    GEMU_STATE_VAR(d, "busy", c->busy);
    GEMU_STATE_VAR(d, "irq_after", c->irq_after);
    GEMU_STATE_VAR(d, "irq", c->irq);
    GEMU_STATE_ARRAY(d, "int_st0", c->int_st0);
    GEMU_STATE_ARRAY(d, "int_pcn", c->int_pcn);
    GEMU_STATE_VAR(d, "n_int", c->n_int);
    for (int u = 0; u < 4; u++) {             /* the diskettes in the drives */
        char n[16];
        snprintf(n, sizeof n, "media%d", u);
        gemu_drive_add_state(d, n, &c->drive[u]);
    }
    return true;
}

static void fdc_reset(GemuDevice *d, GemuResetKind kind) {
    (void)kind;
    Upd765 *c = &((Fdc *)d->priv)->chip;
    c->dor = 0;                       /* as the machine does: the adapter's DOR clears */
    upd765_reset(c);
    gemu_line_set(((Fdc *)d->priv)->intr, c->irq);
}

static void fdc_describe(GemuDevice *d, char *buf, size_t len) {
    const Upd765 *c = &((Fdc *)d->priv)->chip;
    int n = snprintf(buf, len,
                     "DOR %02X: drive %c, %s, DMA/IRQ %s, motors %c%c%c%c\n"
                     "%s, IRQ %s%s\ncylinders %u %u %u %u, %d interrupt status%s pending",
                     c->dor, 'A' + (c->dor & 3), (c->dor & 4) ? "running" : "in reset",
                     (c->dor & 8) ? "on" : "off",
                     (c->dor & 0x10) ? 'A' : '-', (c->dor & 0x20) ? 'B' : '-',
                     (c->dor & 0x40) ? 'C' : '-', (c->dor & 0x80) ? 'D' : '-',
                     c->busy ? "executing" : c->res_pos < c->res_len ? "result phase"
                         : c->cmd_len ? "command phase" : "idle",
                     c->irq ? "high" : "low", c->busy ? " (busy)" : "",
                     c->pcn[0], c->pcn[1], c->pcn[2], c->pcn[3], c->n_int,
                     c->n_int == 1 ? "" : "es");
    for (int u = 0; u < 4 && n >= 0 && (size_t)n < len; u++)
        if (c->drive[u])
            n += snprintf(buf + n, len - (size_t)n, "\nunit %d: %s", u, gemu_drive_name(c->drive[u]));
}

const GemuDeviceType gemu_upd765_type = {
    .name = "upd765", .desc = "NEC uPD765 floppy disk controller (PC diskette adapter)",
    .size = sizeof(Fdc), .init = fdc_init, .reset = fdc_reset, .describe = fdc_describe,
};
