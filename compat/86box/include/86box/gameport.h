/*
 * 86Box v6.0's gameport.h, the part sound cards use, for GEMU's 86Box layer
 * (compat/86box).  GEMU has no joystick port yet: gameport_add() stops GEMU
 * with a message (a card's game port option is off by default).
 *
 * From 86Box (src/include/86box/gameport.h), GPLv2 or later.
 */
#ifndef EMU_GAMEPORT_H
#define EMU_GAMEPORT_H
#include <stdint.h>

#ifdef EMU_DEVICE_H
extern const device_t gameport_device;
extern const device_t gameport_200_device;
extern const device_t gameport_pnp_device;
extern const device_t gameport_pnp_1io_device;
#endif

extern void  gameport_remap(void *priv, uint16_t address);
extern void *gameport_add(const device_t *gameport_type);

#endif /*EMU_GAMEPORT_H*/
