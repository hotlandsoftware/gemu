/* The 8253's save state: see i8253_state.h. */
#include "timer/i8253_state.h"
#include "gemu/dev/state.h"
#include <stdio.h>

/* I8253: ch[3] - each channel's every member, as "ch<n>.<member>" (the
 * 5150's names and order, i8253_dev.c: don't rename or reorder).
 * Excluded: out_changed, ud - the host's callback for the output pins and
 * its argument (wiring, set by whoever owns the chip). */
bool i8253_state_add(GemuDevice *d, const char *prefix, I8253 *p) {
    bool ok = true;
    for (int i = 0; i < 3; i++) {
        I8253Channel *c = &p->ch[i];
        char n[64];
        snprintf(n, sizeof n, "%sch%d.", prefix ? prefix : "", i);
#define CH(field) (ok &= GEMU_STATE_MEMBER(d, n, c, field))
        CH(count); CH(reload); CH(mode); CH(rw_mode); CH(out); CH(gate); CH(counting);
        CH(load_pending); CH(trigger); CH(strobe); CH(write_msb_next); CH(read_msb_next);
        CH(latched); CH(latched_value);
#undef CH
    }
    return ok;
}
