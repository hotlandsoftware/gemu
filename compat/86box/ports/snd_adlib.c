/* 86Box's AdLib (third_party/86box/v6.0, unchanged) as GEMU device type
 * "adlib": the OPL2 at 388h-389h (a ym3812_nuked_opl2 under it) and a music
 * stream.  Its own state is the MCA POS registers, which the ISA card only
 * keeps; the chip is the OPL2's. */
#include "../gemu_86box.h"
#include "gemu/dev/state.h"
#include "../../../third_party/86box/v6.0/src/sound/snd_adlib.c"

static bool adlib_state(GemuDevice *d, void *priv) {
    adlib_t *adlib = priv;
    return GEMU_STATE_ARRAY(d, "pos_regs", adlib->pos_regs);
}

GEMU_86BOX_PORT(gemu_86box_adlib_type, adlib_device, "adlib", adlib_state, NULL, false);
