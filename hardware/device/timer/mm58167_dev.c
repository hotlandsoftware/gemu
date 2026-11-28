/* The MM58167 as a core device: see mm58167_dev.h. */
#include "timer/mm58167_dev.h"
#include "gemu/dev/state.h"
#include <stdio.h>

Mm58167 *gemu_mm58167(GemuDevice *d) { return d->priv; }

#define M(member) (ok &= GEMU_STATE_MEMBER(d, "", r, member))
static bool rtc_init(GemuDevice *d) {
    Mm58167 *r = d->priv;
    mm58167_init(r);
    bool ok = true;
    M(cnt); M(ram); M(int_status); M(int_control); M(standby_int); M(int_out); M(sub); M(last_cycle);
    return ok;
}
#undef M

/* The counters as read: 02 seconds, 03 minutes, 04 hours, 06 day of the
 * month, 07 month (BCD). */
static void rtc_describe(GemuDevice *d, char *buf, size_t len) {
    const Mm58167 *r = d->priv;
    snprintf(buf, len, "%02X/%02X %02X:%02X:%02X (month/day h:m:s, BCD)",
             mm58167_peek(r, 7), mm58167_peek(r, 6), mm58167_peek(r, 4),
             mm58167_peek(r, 3), mm58167_peek(r, 2));
}

const GemuDeviceType gemu_mm58167_type = {
    .name = "mm58167", .desc = "National MM58167 real-time clock",
    .size = sizeof(Mm58167), .init = rtc_init, .describe = rtc_describe,
};
