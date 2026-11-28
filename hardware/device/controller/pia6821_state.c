/* The 6821 PIA's save state: see pia6821_state.h. */
#include "controller/pia6821_state.h"
#include "gemu/dev/state.h"

#define M(member) (ok &= GEMU_STATE_MEMBER(d, prefix, x, member))

/* Pia6821, in order.  Excluded: the callbacks and ud (the wiring).  The
 * IRQ outputs as last reported are saved; the machine's lines hold the
 * levels the rest of the machine saw. */
bool pia6821_state_add(GemuDevice *d, const char *prefix, Pia6821 *x) {
    bool ok = true;
    M(ora); M(ddra); M(cra); M(orb); M(ddrb); M(crb);
    M(ca1); M(ca2); M(cb1); M(cb2);
    M(irq_a); M(irq_b);
    return ok;
}
