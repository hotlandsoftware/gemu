/*
 * GEMU's 86Box layer: fm_driver_get() (<86box/snd_opl.h>), in place of
 * 86Box's src/sound/snd_opl.c, which offers every FM core 86Box has (the C++
 * ymfm ones among them).  Here: the cores that have GEMU ports - the Nuked
 * OPL2 so far - added as a sub-device of the card that asks.  A chip
 * without one stops GEMU with a message (86Box would return 0, which its
 * drivers don't check).
 */
#include "compat_priv.h"
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/snd_opl.h>

uint8_t fm_driver_get_ex(int chip_id, fm_drv_t *drv, int is_48k) {
    void *flag_48k = is_48k ? (void *)(uintptr_t)FM_FORCE_48K : NULL;
    switch (chip_id) {
    case FM_YM3812:                                    /* OPL2 */
        *drv = is_48k ? nuked_opl2_drv_48k : nuked_opl2_drv;
        drv->priv = device_add_params(&ym3812_nuked_opl2_device, flag_48k);
        break;
    default:
        fatal("FM chip %d: GEMU's 86Box layer has the Nuked OPL2 (YM3812) only so far\n",
              chip_id);
    }
    return drv->priv != NULL;
}

uint8_t fm_driver_get(int chip_id, fm_drv_t *drv) {
    return fm_driver_get_ex(chip_id, drv, 0);
}
