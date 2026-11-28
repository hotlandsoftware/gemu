/* The TMS9918 as a core device: see tms9918_dev.h. */
#include "tms9918_dev.h"
#include "gemu/dev/state.h"
#include <stdio.h>

Tms9918 *gemu_tms9918(GemuDevice *d) { return d->priv; }

static uint8_t io_r8(void *ud, uint64_t off) { return tms9918_read(gemu_tms9918(ud), (int)(off & 1)); }
static void io_w8(void *ud, uint64_t off, uint8_t v) { tms9918_write(gemu_tms9918(ud), (int)(off & 1), v); }
static bool io_peek(void *ud, uint64_t off, uint8_t *v) {
    const Tms9918 *t = gemu_tms9918(ud);
    *v = (off & 1) ? tms9918_peek_status(t) : t->read_ahead;
    return true;
}
const GemuHandlers gemu_tms9918_io = { .r8 = io_r8, .w8 = io_w8, .peek8 = io_peek };

#define M(member) (ok &= GEMU_STATE_MEMBER(d, "", v, member))
static bool vdp_init(GemuDevice *d) {
    Tms9918 *v = d->priv;
    tms9918_reset(v);
    bool ok = true;
    M(vram); M(reg); M(status); M(addr); M(latch); M(latched); M(read_ahead);
    M(line); M(frame); M(int_out);
    return ok;
}
#undef M

static void vdp_reset(GemuDevice *d, GemuResetKind kind) {
    (void)kind;
    tms9918_reset(d->priv);
}

static void vdp_describe(GemuDevice *d, char *buf, size_t len) {
    tms9918_describe(d->priv, buf, len);
}

const GemuDeviceType gemu_tms9918_type = {
    .name = "tms9918", .desc = "TI TMS9918/9928/9128 video display processor",
    .size = sizeof(Tms9918), .init = vdp_init, .reset = vdp_reset, .describe = vdp_describe,
};
