/* The RTC's save state: see mc146818_state.h. */
#include "timer/mc146818_state.h"
#include "gemu/dev/state.h"

#define M(member) (ok &= GEMU_STATE_MEMBER(d, prefix, x, member))

/* Mc146818, in order.  Excluded:
 *   dirty      host bookkeeping: RAM changed since the host last collected
 *              it for its NVRAM file (mc146818_take_dirty); a machine that
 *              wants a loaded RAM written out sets it in post_load
 *   reported   how many debug reports were printed (host log limit)
 *   config     Mc146818Config: the clock and irq callbacks, their opaque,
 *              clock_hz, debug - host wiring
 * osc_mark is a count of the board clock (config.clock): it is only
 * meaningful if that clock is restored to the same value with it. */
bool mc146818_state_add(GemuDevice *d, const char *prefix, Mc146818 *x) {
    bool ok = true;
    M(nv); M(battery); M(nmi_disabled); M(std_index); M(ext_index); M(running);
    M(divider); M(osc_mark); M(irq);
    return ok;
}
