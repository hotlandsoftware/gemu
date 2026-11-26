/*
 * 86Box v6.0's mca.h for GEMU's 86Box layer (compat/86box).  No GEMU
 * machine has a MicroChannel bus: mca_add() stops GEMU with a message.  It
 * is here because drivers with an ISA and an MCA variant in one file (the
 * AdLib) call it from the MCA variant's init, which GEMU doesn't offer.
 *
 * From 86Box (src/include/86box/mca.h), GPLv2 or later.
 */
#ifndef EMU_MCA_H
#define EMU_MCA_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void mca_add(uint8_t (*read)(uint16_t port, void *priv),
                    void (*write)(uint16_t port, uint8_t val, void *priv),
                    uint8_t (*feedb)(void *priv),
                    void (*reset)(void *priv),
                    void *priv);

#ifdef __cplusplus
}
#endif

#endif /*EMU_MCA_H*/
