/* 86Box's Creative Music System / Game Blaster (third_party/86box/v6.0,
 * unchanged) as GEMU device type "cms": two Philips SAA1099s at 2x0h-2x3h.
 * (The Sound Blaster 1.0 and 1.5 carry the same chips: cms_read/cms_write
 * here serve them too.)  Its state is its cms_t, which has no pointers. */
#include "../gemu_86box.h"
#include "gemu/dev/state.h"
#include "../../../third_party/86box/v6.0/src/sound/snd_cms.c"

static bool cms_state(GemuDevice *d, void *priv) {
    return gemu_state_add(d, "cms", priv, sizeof(cms_t), 1);
}

GEMU_86BOX_PORT(gemu_86box_cms_type, cms_device, "cms", cms_state, NULL, false);
