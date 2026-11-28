/* The CDP1869/CDP1870 as a core device: see cdp1869_dev.h. */
#include "cdp1869_dev.h"
#include "gemu/dev/state.h"
#include <stdio.h>

#define M(member) (ok &= GEMU_STATE_MEMBER(d, "", x, member))

Cdp1869 *gemu_cdp1869(GemuDevice *d) { return d->priv; }

/* Cdp1869, in order; not bitmap (rendered from the rest) or dirty. */
static bool vis_init(GemuDevice *d) {
    Cdp1869 *x = d->priv;
    cdp1869_init(x);
    bool ok = true;
    M(page_ram); M(char_ram); M(pcb_ram);
    M(prd); M(non_display); M(dispoff); M(fresvert); M(freshorz); M(cmem); M(dblpage);
    M(line16); M(line9); M(cfc); M(col); M(bkg); M(pma); M(hma); M(page_ram_mask);
    M(page_mask); M(max_page); M(char_stride); M(block_cpu_access); M(q); M(pcb);
    M(tone_amp); M(tone_freq_sel); M(tone_off); M(tone_div); M(lc); M(mc);
    return ok;
}

static void vis_post_load(GemuDevice *d) {
    ((Cdp1869 *)d->priv)->dirty = true;
}

static void vis_describe(GemuDevice *d, char *buf, size_t len) {
    const Cdp1869 *x = d->priv;
    snprintf(buf, len, "line %u, machine cycle %u, %s; PMA %04X HMA %04X, background %u, colour %u\n"
             "%s%s%s%s, tone %s (divider %u, range %u, amplitude %u)",
             x->lc, x->mc, x->non_display ? "not displaying" : "displaying", x->pma, x->hma,
             x->bkg, x->col, x->dispoff ? "display off" : "display on",
             x->fresvert ? ", full vertical" : "", x->freshorz ? ", full horizontal" : "",
             x->dblpage ? ", double page" : "", x->tone_off ? "off" : "on",
             x->tone_div, x->tone_freq_sel, x->tone_amp);
}

const GemuDeviceType gemu_cdp1869_type = {
    .name = "cdp1869", .desc = "RCA CDP1869/CDP1870 video interface system", .size = sizeof(Cdp1869),
    .init = vis_init, .describe = vis_describe, .post_load = vis_post_load,
};
