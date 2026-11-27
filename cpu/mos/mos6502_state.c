/* The 6502's save state: see mos6502_state.h. */
#include "mos6502_state.h"
#include "gemu/dev/state.h"

#define M(member) (ok &= GEMU_STATE_MEMBER(d, prefix, x, member))

/* Mos6502, in order.  Excluded:
 *   mem_read, mem_write, mem_ud   the bus callbacks and their context
 *                                 (the machine's wiring)
 * Everything else is registered, including the variant settings
 * decimal_disable, addr_mask, no_int_pins, dummy_cycles and cycle_polling
 * (set by the machine at init: the same values on the same machine) and the
 * internal dummy_addr, poll_nmi/poll_irq and rdy_halted. */
bool mos6502_state_add(GemuDevice *d, const char *prefix, Mos6502 *x) {
    bool ok = true;
    M(A); M(X); M(Y); M(SP); M(PC); M(P);
    M(cycle_count); M(insn_count);
    M(irq); M(nmi);
    M(decimal_disable); M(addr_mask); M(no_int_pins); M(dummy_cycles);
    M(dummy_addr); M(cycle_polling);
    M(poll_nmi); M(poll_irq);
    M(rdy_halted);
    return ok;
}
