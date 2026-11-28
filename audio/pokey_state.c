/* POKEY's save state: see pokey_state.h. */
#include "pokey_state.h"
#include "gemu/dev/state.h"

#define M(member) (ok &= GEMU_STATE_MEMBER(d, prefix, x, member))

/* Pokey, in order, all of it. */
bool pokey_state_add(GemuDevice *d, const char *prefix, Pokey *x) {
    bool ok = true;
    M(audf); M(audc); M(audctl);
    M(irqen); M(irq_pending);
    M(skctl); M(skstat); M(kbcode);
    M(serout); M(serout_delay);
    M(timer_count); M(poly17);
    return ok;
}
