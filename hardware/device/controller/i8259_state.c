/* The 8259A's save state: see i8259_state.h. */
#include "controller/i8259_state.h"
#include "gemu/dev/state.h"
#include <stdio.h>

/* Every member of I8259, in order; nothing excluded.  The names are the
 * 5150's (i8259_dev.c): don't rename or reorder. */
bool i8259_state_add(GemuDevice *d, const char *prefix, I8259 *c) {
    bool ok = true;
    ok &= GEMU_STATE_MEMBER(d, prefix, c, imr);
    ok &= GEMU_STATE_MEMBER(d, prefix, c, irr);
    ok &= GEMU_STATE_MEMBER(d, prefix, c, isr);
    ok &= GEMU_STATE_MEMBER(d, prefix, c, lines);
    ok &= GEMU_STATE_MEMBER(d, prefix, c, vector_base);
    ok &= GEMU_STATE_MEMBER(d, prefix, c, auto_eoi);
    ok &= GEMU_STATE_MEMBER(d, prefix, c, level_mode);
    ok &= GEMU_STATE_MEMBER(d, prefix, c, read_isr);
    ok &= GEMU_STATE_MEMBER(d, prefix, c, poll);
    ok &= GEMU_STATE_MEMBER(d, prefix, c, icw_step);
    ok &= GEMU_STATE_MEMBER(d, prefix, c, single);
    ok &= GEMU_STATE_MEMBER(d, prefix, c, expect_icw4);
    return ok;
}

/* I8259Pair: master, slave - both registered. */
bool i8259_pair_state_add(GemuDevice *d, const char *prefix, I8259Pair *p) {
    char sub[64];
    bool ok = true;
    snprintf(sub, sizeof sub, "%smaster.", prefix ? prefix : "");
    ok &= i8259_state_add(d, sub, &p->master);
    snprintf(sub, sizeof sub, "%sslave.", prefix ? prefix : "");
    ok &= i8259_state_add(d, sub, &p->slave);
    return ok;
}
