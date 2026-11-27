/* A ClockDiv's save state: see clock_div_state.h. */
#include "timer/clock_div_state.h"
#include "gemu/dev/state.h"

/* ClockDiv: every member, in order.  from_hz and to_hz are the machine's
 * rates, saved with the remainder they scale (a remainder is only right for
 * the rates it was taken with); mark is a count of the source clock, only
 * meaningful if that clock is restored with it (else clock_div_rebase()
 * after the load). */
bool clock_div_state_add(GemuDevice *d, const char *prefix, ClockDiv *x) {
    bool ok = true;
    ok &= GEMU_STATE_MEMBER(d, prefix, x, from_hz);
    ok &= GEMU_STATE_MEMBER(d, prefix, x, to_hz);
    ok &= GEMU_STATE_MEMBER(d, prefix, x, mark);
    ok &= GEMU_STATE_MEMBER(d, prefix, x, remainder);
    return ok;
}
