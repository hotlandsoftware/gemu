/* The system control port's save state: see pc_sysctl_state.h. */
#include "controller/pc_sysctl_state.h"
#include "gemu/dev/state.h"

/* PcSysctl, in order.  Excluded: pit - the timer whose channel 2 it gates
 * (wiring; the timer registers its own state). */
bool pc_sysctl_state_add(GemuDevice *d, const char *prefix, PcSysctl *x) {
    bool ok = true;
    ok &= GEMU_STATE_MEMBER(d, prefix, x, port_b);
    ok &= GEMU_STATE_MEMBER(d, prefix, x, post);
    return ok;
}
