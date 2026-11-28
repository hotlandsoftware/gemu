/* The TIA's save state: see tia_state.h. */
#include "tia_state.h"
#include "gemu/dev/state.h"
#include <stdio.h>
#include <stddef.h>

#define M(member) (ok &= GEMU_STATE_MEMBER(d, prefix, x, member))
#define S(member) (ok &= gemu_state_add_named(d, sp, #member, &x->sound.member, \
                                              sizeof x->sound.member, 1))

static const GemuStateField dq_fields[] = {
    GEMU_STATE_FIELD(TiaDelayedWrite, when),
    GEMU_STATE_FIELD(TiaDelayedWrite, reg),
    GEMU_STATE_FIELD(TiaDelayedWrite, val),
};

/* Tia, in order.  Excluded:
 *   fire, pot_charge          the controllers' levels, set from the host's
 *                             input every frame (update_tia_inputs)
 *   debug_colors, hide,       the debugger's settings and its full-frame
 *   debug_view, dbg,          view, not machine state
 *   dbg_vsync
 *   sound.tia_rate            configuration (the colour clock)
 *   sound.buf, buf_n, dev     host output: samples made but not yet handed on
 * The objects (TiaObject, all single bytes) and the sound channels
 * (TiaChannel, likewise) are saved whole; the delayed writes as a table. */
bool tia_state_add(GemuDevice *d, const char *prefix, Tia *x) {
    bool ok = true;
    M(clock); M(hctr); M(scanline); M(hblank); M(ext_hblank);
    M(vsync); M(vblank);
    M(nusiz); M(colup); M(colupf); M(colubk); M(ctrlpf); M(pf);
    M(refp); M(grp_new); M(grp_old); M(enam); M(enabl_new); M(enabl_old);
    M(vdelp); M(vdelbl); M(resmp);
    M(p); M(m); M(bl);
    M(pf_bit); M(pf_reflect);
    M(movement); M(movement_clock);
    M(cx);
    M(latch_inputs); M(latch); M(pot_start);
    char name[64];
    snprintf(name, sizeof name, "%sdq", prefix);
    ok &= GEMU_STATE_TABLE(d, name, x->dq, dq_fields);
    M(dq_n);
    M(fb); M(draw); M(frame_ready); M(frame_count); M(frame_lines);
    M(vsync_end_line); M(first_lit); M(last_lit);
    M(done_first_lit); M(done_last_lit); M(done_vsync_end); M(line_lit);
    M(sound_clock);
    char sp[64];
    snprintf(sp, sizeof sp, "%ssound.", prefix);
    S(ch); S(counter); S(sum); S(sum_n); S(phase); S(acc); S(dc_x); S(dc_y);
    return ok;
}
