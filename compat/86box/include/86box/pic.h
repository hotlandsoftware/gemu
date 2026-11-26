/*
 * 86Box v6.0's pic.h for GEMU's 86Box layer (compat/86box): the interrupt
 * requests a driver makes.  The PIC itself is the machine's (GEMU's i8259);
 * the driver's device has output lines irq0..irq15, which the machine
 * connects to it (gemu_86box.h).
 *
 * picint() (edge): the line goes high - low then high again if it already
 * was, so each call is a request, as each sets the IRR bit in 86Box.
 * picintc(): low.  picintlevel()/picintclevel(): the line is the level;
 * several devices on one IRQ are the core's wired-OR.  num is a mask of
 * IRQs (1 << n).
 *
 * From 86Box (src/include/86box/pic.h), GPLv2 or later:
 *   Copyright 2015-2020 Andrew Jenner.  Copyright 2016-2020 Miran Grca.
 */
#ifndef EMU_PIC_H
#define EMU_PIC_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void picint_common(uint16_t num, int level, int set, uint8_t *irq_state);

#ifdef __cplusplus
}
#endif

#define PIC_IRQ_EDGE                    0
#define PIC_IRQ_LEVEL                   1

#define picint(num)                     picint_common(num, PIC_IRQ_EDGE,  1, NULL)
#define picintlevel(num, irq_state)     picint_common(num, PIC_IRQ_LEVEL, 1, irq_state)
#define picintc(num)                    picint_common(num, PIC_IRQ_EDGE,  0, NULL)
#define picintclevel(num, irq_state)    picint_common(num, PIC_IRQ_LEVEL, 0, irq_state)

#endif /*EMU_PIC_H*/
