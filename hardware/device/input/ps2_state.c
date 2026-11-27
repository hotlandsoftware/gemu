/* Save state for the PS/2 models: see ps2_state.h. */
#include "input/ps2_state.h"
#include "gemu/dev/state.h"

#define M(member) (ok &= GEMU_STATE_MEMBER(d, prefix, x, member))

/* I8042, in order.  Excluded: kbd, aux - pointers to the keyboard and the
 * mouse (wiring; each registers its own state). */
bool i8042_state_add(GemuDevice *d, const char *prefix, I8042 *x) {
    bool ok = true;
    M(command_byte); M(pending_write); M(out); M(out_pos); M(out_len);
    return ok;
}

/* Ps2Keyboard: every member, in order. */
bool ps2_keyboard_state_add(GemuDevice *d, const char *prefix, Ps2Keyboard *x) {
    bool ok = true;
    M(queue); M(queue_pos); M(queue_len); M(pending_command); M(leds); M(typematic);
    M(scan_set); M(last_sent); M(scanning);
    return ok;
}

/* Ps2Mouse: every member, in order. */
bool ps2_mouse_state_add(GemuDevice *d, const char *prefix, Ps2Mouse *x) {
    bool ok = true;
    M(out); M(out_pos); M(out_len); M(streaming); M(param_cmd); M(resolution);
    M(sample_rate); M(scaling_2to1); M(left); M(right); M(resets);
    return ok;
}
