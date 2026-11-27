/* The INS8250's save state: see ins8250_state.h. */
#include "serial/ins8250_state.h"
#include "gemu/dev/state.h"

/* Ins8250, in order (the 5150's names and order, ins8250_dev.c: don't
 * rename or reorder).  Excluded:
 *   model, clock_hz   wiring, set from the device's options before reset
 *   set_irq, ud       the host's interrupt callback and its argument
 *   dev_tx, dev_lines, dev_rx, dev   the attached device's callbacks and
 *                     its argument (host wiring)
 * Everything from modem_in on is registered. */
bool ins8250_state_add(GemuDevice *d, const char *prefix, Ins8250 *u) {
    bool ok = true;
    ok &= GEMU_STATE_MEMBER(d, prefix, u, modem_in);
    ok &= GEMU_STATE_MEMBER(d, prefix, u, rbr);
    ok &= GEMU_STATE_MEMBER(d, prefix, u, thr);
    ok &= GEMU_STATE_MEMBER(d, prefix, u, ier);
    ok &= GEMU_STATE_MEMBER(d, prefix, u, lcr);
    ok &= GEMU_STATE_MEMBER(d, prefix, u, mcr);
    ok &= GEMU_STATE_MEMBER(d, prefix, u, lsr);
    ok &= GEMU_STATE_MEMBER(d, prefix, u, msr);
    ok &= GEMU_STATE_MEMBER(d, prefix, u, scr);
    ok &= GEMU_STATE_MEMBER(d, prefix, u, divisor);
    ok &= GEMU_STATE_MEMBER(d, prefix, u, thre_int);
    ok &= GEMU_STATE_MEMBER(d, prefix, u, tx_busy);
    ok &= GEMU_STATE_MEMBER(d, prefix, u, tsr);
    ok &= GEMU_STATE_MEMBER(d, prefix, u, tx_wait);
    ok &= GEMU_STATE_MEMBER(d, prefix, u, rx_busy);
    ok &= GEMU_STATE_MEMBER(d, prefix, u, rsr);
    ok &= GEMU_STATE_MEMBER(d, prefix, u, rx_wait);
    ok &= GEMU_STATE_MEMBER(d, prefix, u, loop_pending);
    ok &= GEMU_STATE_MEMBER(d, prefix, u, loop_byte);
    return ok;
}
