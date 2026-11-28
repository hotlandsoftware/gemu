/* The MSM5205 as a core device: see msm5205_dev.h. */
#include "msm5205_dev.h"
#include "gemu/dev/state.h"
#include <stdio.h>

Msm5205 *gemu_msm5205(GemuDevice *d) { return d->priv; }

static bool msm_init(GemuDevice *d) {
    Msm5205 *m = d->priv;
    msm5205_init(m);
    return GEMU_STATE_MEMBER(d, "", m, signal) && GEMU_STATE_MEMBER(d, "", m, step) &&
           GEMU_STATE_MEMBER(d, "", m, reset) && GEMU_STATE_MEMBER(d, "", m, bits3);
}

static void msm_describe(GemuDevice *d, char *buf, size_t len) {
    const Msm5205 *m = d->priv;
    snprintf(buf, len, "signal %d, step %d%s%s", m->signal, m->step,
             m->reset ? ", reset" : "", m->bits3 ? ", 3-bit" : "");
}

const GemuDeviceType gemu_msm5205_type = {
    .name = "msm5205", .desc = "OKI MSM5205 ADPCM speech synthesizer",
    .size = sizeof(Msm5205), .init = msm_init, .describe = msm_describe,
};
