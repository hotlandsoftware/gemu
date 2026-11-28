/* The ADC0809 as a core device: see adc0809_dev.h. */
#include "controller/adc0809_dev.h"
#include "gemu/dev/state.h"
#include <inttypes.h>
#include <stdio.h>

Adc0809 *gemu_adc0809(GemuDevice *d) { return d->priv; }

#define M(member) (ok &= GEMU_STATE_MEMBER(d, "", a, member))
static bool adc_init(GemuDevice *d) {
    Adc0809 *a = d->priv;
    adc0809_init(a);
    bool ok = true;
    M(in); M(result); M(channel); M(busy); M(done_at); M(pending);
    return ok;
}
#undef M

static void adc_describe(GemuDevice *d, char *buf, size_t len) {
    const Adc0809 *a = d->priv;
    int n = snprintf(buf, len, "channel %u, latch %02X%s; inputs", a->channel, a->result,
                     a->busy ? ", converting" : "");
    for (int i = 0; i < 8 && n >= 0 && (size_t)n < len; i++)
        n += snprintf(buf + n, len - (size_t)n, " %.0f", a->in[i]);
}

const GemuDeviceType gemu_adc0809_type = {
    .name = "adc0809", .desc = "National ADC0809 8-channel A/D converter",
    .size = sizeof(Adc0809), .init = adc_init, .describe = adc_describe,
};
