/* The plain 8255 as a core device: see ppi8255_dev.h. */
#include "controller/ppi8255_dev.h"
#include "gemu/dev/state.h"
#include <stdio.h>

Ppi8255 *gemu_ppi8255(GemuDevice *d) { return d->priv; }

static uint8_t bus_r8(void *ud, uint64_t off) { return ppi8255_read(gemu_ppi8255(ud), (uint8_t)(off & 3)); }
static void bus_w8(void *ud, uint64_t off, uint8_t v) { ppi8255_write(gemu_ppi8255(ud), (uint8_t)(off & 3), v); }
const GemuHandlers gemu_ppi8255_bus = { .r8 = bus_r8, .w8 = bus_w8 };

static bool ppi_init(GemuDevice *d) {
    Ppi8255 *p = d->priv;
    ppi8255_reset(p);
    return GEMU_STATE_MEMBER(d, "", p, control) && GEMU_STATE_MEMBER(d, "", p, out);
}

static void ppi_reset(GemuDevice *d, GemuResetKind kind) {
    (void)kind;
    ppi8255_reset(d->priv);
}

static void ppi_describe(GemuDevice *d, char *buf, size_t len) {
    const Ppi8255 *p = d->priv;
    snprintf(buf, len, "control %02X; outputs A %02X B %02X C %02X", p->control,
             p->out[0], p->out[1], p->out[2]);
}

const GemuDeviceType gemu_ppi8255_type = {
    .name = "ppi8255", .desc = "Intel 8255 programmable peripheral interface",
    .size = sizeof(Ppi8255), .init = ppi_init, .reset = ppi_reset, .describe = ppi_describe,
};
