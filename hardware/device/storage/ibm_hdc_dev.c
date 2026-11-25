/* The IBM Fixed Disk Adapter as a core device: see ibm_hdc_dev.h. */
#include "storage/ibm_hdc_dev.h"
#include "controller/i8237_dev.h"
#include "gemu/dev/line.h"
#include "gemu/dev/option.h"
#include "gemu/dev/state.h"
#include <stdio.h>

typedef struct {
    IbmHdc      chip;
    GemuLine   *intr;
    GemuDevice *dma;
    unsigned    dma_ch;
} Hdc;

IbmHdc *gemu_ibm_hdc(GemuDevice *d) { return &((Hdc *)d->priv)->chip; }
void gemu_ibm_hdc_tick(GemuDevice *d, uint32_t c) { ibm_hdc_tick(&((Hdc *)d->priv)->chip, c); }

void gemu_ibm_hdc_set_dma(GemuDevice *d, GemuDevice *dma, unsigned ch) {
    Hdc *p = d->priv;
    p->dma = dma;
    p->dma_ch = ch;
}

static void set_irq(void *ud, bool level) { gemu_line_set(((Hdc *)ud)->intr, level); }
static bool dma_to_mem(void *ud, uint8_t val, bool *tc) {
    Hdc *p = ud;
    return p->dma && gemu_i8237_to_mem(p->dma, p->dma_ch, val, tc);
}
static bool dma_from_mem(void *ud, uint8_t *val, bool *tc) {
    Hdc *p = ud;
    return p->dma && gemu_i8237_from_mem(p->dma, p->dma_ch, val, tc);
}

static IbmHdc *chip_of(void *ud) { return &((Hdc *)((GemuDevice *)ud)->priv)->chip; }
static uint8_t io_r8(void *ud, uint64_t off) { return ibm_hdc_read(chip_of(ud), (unsigned)off & 3); }
static void io_w8(void *ud, uint64_t off, uint8_t v) { ibm_hdc_write(chip_of(ud), (unsigned)off & 3, v); }
/* Status (321h) and the jumpers (322h) read without side effects. */
static bool io_peek(void *ud, uint64_t off, uint8_t *v) {
    if ((off & 3) != 1 && (off & 3) != 2)
        return false;
    *v = io_r8(ud, off);
    return true;
}
const GemuHandlers gemu_ibm_hdc_io = { .r8 = io_r8, .w8 = io_w8, .peek8 = io_peek };

static const char *const irqs[] = { "5", "2", NULL };
static const GemuOptionDef hdc_opts[] = {
    { "romfile", "the adapter's BIOS ROM (default: the one -rom found)", GEMU_OPT_FILE, NULL, 0, 0, NULL },
    { "type0", "drive 0's type jumper (-1: the one its image's size fits)", GEMU_OPT_INT, "-1", -1, 3, NULL },
    { "type1", "drive 1's type jumper", GEMU_OPT_INT, "-1", -1, 3, NULL },
    { "interleave", "how the drives' tracks are formatted", GEMU_OPT_INT, "6", 1, IBM_HDC_SPT - 1, NULL },
    { "irq", "interrupt line", GEMU_OPT_CHOICE, "5", 0, 0, irqs },
    { "rom", "the ROM jumper", GEMU_OPT_BOOL, "on", 0, 0, NULL },
    { NULL, NULL, 0, NULL, 0, 0, NULL },
};

static bool hdc_init(GemuDevice *d) {
    Hdc *p = d->priv;
    IbmHdc *c = &p->chip;
    if (!(p->intr = gemu_line_out(d, "int")))
        return false;
    c->set_irq = set_irq;
    c->dma_to_mem = dma_to_mem;
    c->dma_from_mem = dma_from_mem;
    c->ud = p;
    GEMU_STATE_VAR(d, "now", c->now);
    GEMU_STATE_VAR(d, "phase", c->phase);
    GEMU_STATE_VAR(d, "stage", c->stage);
    GEMU_STATE_VAR(d, "wait", c->wait);
    GEMU_STATE_VAR(d, "mask", c->mask);
    GEMU_STATE_VAR(d, "intr", c->intr);
    GEMU_STATE_ARRAY(d, "cmd", c->cmd);
    GEMU_STATE_VAR(d, "cmd_n", c->cmd_n);
    GEMU_STATE_ARRAY(d, "pio", c->pio);
    GEMU_STATE_VAR(d, "pio_n", c->pio_n);
    GEMU_STATE_VAR(d, "pio_pos", c->pio_pos);
    GEMU_STATE_ARRAY(d, "buf", c->buf);
    GEMU_STATE_VAR(d, "buf_len", c->buf_len);
    GEMU_STATE_VAR(d, "buf_pos", c->buf_pos);
    GEMU_STATE_VAR(d, "to_mem", c->to_mem);
    GEMU_STATE_VAR(d, "drv", c->drv);
    GEMU_STATE_VAR(d, "cyl", c->cyl);
    GEMU_STATE_VAR(d, "head", c->head);
    GEMU_STATE_VAR(d, "sector", c->sector);
    GEMU_STATE_VAR(d, "count", c->count);
    GEMU_STATE_ARRAY(d, "pos", c->pos);
    for (int u = 0; u < 2; u++) {
        char n[32];
        snprintf(n, sizeof n, "param%d.cyls", u);
        GEMU_STATE_VAR(d, n, c->param[u].cyls);
        snprintf(n, sizeof n, "param%d.heads", u);
        GEMU_STATE_VAR(d, n, c->param[u].heads);
    }
    GEMU_STATE_VAR(d, "status_byte", c->status_byte);
    GEMU_STATE_ARRAY(d, "sense", c->sense);
    return true;
}

static void hdc_reset(GemuDevice *d, GemuResetKind kind) {
    (void)kind;
    Hdc *p = d->priv;
    ibm_hdc_reset(&p->chip);
    gemu_line_set(p->intr, p->chip.intr && (p->chip.mask & 2));
}

static void hdc_describe(GemuDevice *d, char *buf, size_t len) {
    const IbmHdc *c = &((Hdc *)d->priv)->chip;
    int n = snprintf(buf, len,
                     "status %02X  mask %02X (DMA %s, IRQ %s)  jumpers %02X  phase %d%s\n"
                     "last command %02X: drive %d cylinder %u head %u sector %u count %u, "
                     "status byte %02X, sense %02X",
                     ibm_hdc_read((IbmHdc *)c, 1), c->mask, (c->mask & 1) ? "on" : "off",
                     (c->mask & 2) ? "on" : "off", c->jumpers, c->phase,
                     c->wait ? " (waiting)" : "", c->cmd[0], c->drv, c->cyl, c->head, c->sector,
                     c->count, c->status_byte, c->sense[0]);
    for (int u = 0; u < 2 && n >= 0 && (size_t)n < len; u++)
        if (c->drive[u])
            n += snprintf(buf + n, len - (size_t)n,
                          "\ndrive %d: %s, %u cylinders %u heads, interleave %u, heads at %u",
                          u, gemu_drive_name(c->drive[u]), c->geom[u].cyls, c->geom[u].heads,
                          c->interleave[u], c->pos[u]);
}

const GemuDeviceType gemu_ibm_hdc_type = {
    .name = "ibm-fixed-disk", .desc = "IBM Fixed Disk Adapter",
    .size = sizeof(Hdc), .options = hdc_opts,
    .init = hdc_init, .reset = hdc_reset, .describe = hdc_describe,
};
