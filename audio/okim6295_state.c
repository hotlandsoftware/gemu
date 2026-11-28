/* The OKI MSM6295's save state: see okim6295_state.h. */
#include "okim6295_state.h"
#include "gemu/dev/state.h"
#include <stddef.h>
#include <stdio.h>

#define M(member) (ok &= GEMU_STATE_MEMBER(d, prefix, x, member))

static const GemuStateField ch_fields[] = {
    GEMU_STATE_FIELD(Okim6295Channel, playing),
    GEMU_STATE_FIELD(Okim6295Channel, pos),
    GEMU_STATE_FIELD(Okim6295Channel, end),
    GEMU_STATE_FIELD(Okim6295Channel, low_nibble),
    GEMU_STATE_FIELD(Okim6295Channel, signal),
    GEMU_STATE_FIELD(Okim6295Channel, step_index),
    GEMU_STATE_FIELD(Okim6295Channel, attenuation),
};

/* Okim6295, in order.  Excluded:
 *   rom, rom_size            the sample ROM (the board's)
 *   step_period, clock_pps   from the clocks (configuration)
 *   frame_buf, frame_n       samples made but not yet handed on
 *   audio_dev                the host's output */
bool okim6295_state_add(GemuDevice *d, const char *prefix, Okim6295 *x) {
    char name[64];
    snprintf(name, sizeof name, "%sch", prefix);
    bool ok = GEMU_STATE_TABLE(d, name, x->ch, ch_fields);
    M(cmd_pending); M(cmd_phrase); M(step_acc); M(sample_acc); M(current_output);
    return ok;
}
