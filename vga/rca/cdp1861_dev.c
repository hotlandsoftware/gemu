/* The CDP1861 as a core device: see cdp1861_dev.h. */
#include "cdp1861_dev.h"
#include "gemu/dev/state.h"
#include <stdio.h>

#define M(member) (ok &= GEMU_STATE_MEMBER(d, "", x, member))

Cdp1861 *gemu_cdp1861(GemuDevice *d) { return d->priv; }

void gemu_cdp1861_set_dma(GemuDevice *d, void (*cb)(uint8_t *byte, void *ud), void *ud) {
    Cdp1861 *x = d->priv;
    x->dma_out_cb = cb;
    x->dma_out_ud = ud;
}

/* Cdp1861, in order; not dma_out_cb and dma_out_ud (the wiring). */
static bool vdc_init(GemuDevice *d) {
    Cdp1861 *x = d->priv;
    cdp1861_init(x, NULL, NULL);
    bool ok = true;
    M(lines_total); M(line_counter); M(mcycle); M(display_on); M(int_fired); M(display_addr);
    return ok;
}

static void vdc_describe(GemuDevice *d, char *buf, size_t len) {
    const Cdp1861 *x = d->priv;
    snprintf(buf, len, "%s, line %u of %u, machine cycle %u; display %s, frame interrupt %s, "
             "DMA byte %u", x->lines_total == CDP1861_PAL_LINES_TOTAL ? "PAL" : "NTSC",
             x->line_counter, x->lines_total, x->mcycle, x->display_on ? "on" : "off",
             x->int_fired ? "fired" : "not yet", x->display_addr);
}

const GemuDeviceType gemu_cdp1861_type = {
    .name = "cdp1861", .desc = "RCA CDP1861 video display controller", .size = sizeof(Cdp1861),
    .init = vdc_init, .describe = vdc_describe,
};
