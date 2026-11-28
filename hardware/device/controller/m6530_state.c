/* The 6530 RRIOT's save state: see m6530_state.h. */
#include "controller/m6530_state.h"
#include "gemu/dev/state.h"

#define M(member) (ok &= GEMU_STATE_MEMBER(d, prefix, x, member))

/* M6530, in order.  Excluded: the callbacks, ud and irq_ud (the wiring).
 * The timer is its reload, prescaler and the cycle of its underflow, in
 * the CPU's cycles, which the CPU's state holds. */
bool m6530_state_add(GemuDevice *d, const char *prefix, M6530 *x) {
    bool ok = true;
    M(ram); M(pa); M(pb); M(ddra); M(ddrb);
    M(reload); M(prescale); M(irq_en); M(running); M(irq_flag); M(next_fire);
    return ok;
}
