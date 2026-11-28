/* The CDP1802's save state: see cdp1802_state.h. */
#include "cdp1802_state.h"
#include "gemu/dev/state.h"
#include <stdio.h>

#define M(member) (ok &= GEMU_STATE_MEMBER(d, prefix, x, member))

/* Cdp1802, in order.  Excluded:
 *   next_dma_out.cb, .ud, dma_queue   callbacks (refilled after a load)
 *   mem, mem_mask                     the machine's memory (configuration)
 *   io_in ... panic_ud                the wiring */
bool cdp1802_state_add(GemuDevice *d, const char *prefix, Cdp1802 *x) {
    bool ok = true;
    M(D); M(DF); M(B); M(R); M(P); M(X); M(N); M(I); M(T); M(IE); M(Q); M(EF);
    M(state); M(exec_left); M(idle); M(init_pending);
    M(dma_out_pending); M(irq_pending);
    char name[64];
    snprintf(name, sizeof name, "%snext_dma_out.count", prefix);
    ok &= gemu_state_add(d, name, &x->next_dma_out.count, sizeof x->next_dma_out.count, 1);
    M(dma_head); M(dma_count); M(memory_addr); M(cycle_count); M(insn_count);
    return ok;
}
