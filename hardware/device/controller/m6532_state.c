/* The 6532 RIOT's save state: see m6532_state.h. */
#include "controller/m6532_state.h"
#include "gemu/dev/state.h"

#define M(member) (ok &= GEMU_STATE_MEMBER(d, prefix, x, member))

/* M6532, in order.  Excluded: read_pa, read_pb, ud (the wiring).  The
 * timer is kept as its start cycle, value and interval: it is worked out
 * from the CPU's cycle count, which the CPU's state holds. */
bool m6532_state_add(GemuDevice *d, const char *prefix, M6532 *x) {
    bool ok = true;
    M(ram); M(ora); M(ddra); M(orb); M(ddrb);
    M(timer_start); M(timer_value); M(timer_shift); M(flag_cleared);
    M(pa7_flag); M(edge_ctrl);
    return ok;
}
