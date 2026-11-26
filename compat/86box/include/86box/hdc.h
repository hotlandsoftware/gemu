/*
 * 86Box v6.0's hdc.h, the part sound cards use, for GEMU's 86Box layer
 * (compat/86box): the IDE interfaces some cards carry.  GEMU doesn't offer
 * those models; adding one stops GEMU with a message.
 *
 * From 86Box (src/include/86box/hdc.h), GPLv2 or later.
 */
#ifndef EMU_HDC_H
#define EMU_HDC_H

#ifdef EMU_DEVICE_H
extern const device_t ide_qua_pnp_device;
#endif

#endif /*EMU_HDC_H*/
