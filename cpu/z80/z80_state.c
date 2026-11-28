/* The Z80's save state: see z80_state.h. */
#include "z80_state.h"
#include "gemu/dev/state.h"

#define M(member) (ok &= GEMU_STATE_MEMBER(d, prefix, x, member))

/* Z80, in order.  Excluded: read, write, in, out, ud (the machine's
 * wiring).  Included: the internal ei, p, q, q_set (the last
 * instruction's effects, which the next one depends on) and int_data
 * (what the board puts on the bus at an interrupt acknowledge). */
bool z80_state_add(GemuDevice *d, const char *prefix, Z80 *x) {
    bool ok = true;
    M(a); M(f); M(b); M(c); M(d); M(e); M(h); M(l);
    M(af_); M(bc_); M(de_); M(hl_);
    M(ix); M(iy); M(sp); M(pc); M(wz);
    M(i); M(r); M(im);
    M(iff1); M(iff2); M(halted); M(ei); M(p); M(q); M(q_set);
    M(int_line); M(nmi_line); M(int_data);
    M(cycles);
    return ok;
}
