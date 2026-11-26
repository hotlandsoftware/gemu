/*
 * 86Box v6.0's dma.h for GEMU's 86Box layer (compat/86box): a driver's DMA
 * cycles, on the machine's 8237 (GEMU's i8237 device; channels 0-3).
 *
 * dma_channel_read(): one byte from memory (a channel programmed for a read
 * transfer), with DMA_OVER set when it was the last (terminal count);
 * dma_channel_write(): one byte to memory (a write transfer), DMA_OVER
 * likewise.  DMA_NODATA if the channel isn't ready: masked, the controller
 * disabled, programmed the other way, or no DMA controller.
 * dma_set_drq(): the channel's request bit in the status register.
 *
 * From 86Box (src/include/86box/dma.h), GPLv2 or later:
 *   Copyright 2017-2020 Fred N. van Kempen.  Copyright 2016-2020 Miran Grca.
 *   Copyright 2008-2020 Sarah Walker.
 */
#ifndef EMU_DMA_H
#define EMU_DMA_H
#include <stdint.h>

#define DMA_NODATA -1
#define DMA_OVER   0x10000
#define DMA_VERIFY 0x20000

#ifdef __cplusplus
extern "C" {
#endif

extern void dma_set_drq(int channel, int set);
extern int  dma_channel_read(int channel);
extern int  dma_channel_write(int channel, uint16_t val);

#ifdef __cplusplus
}
#endif

#endif /*EMU_DMA_H*/
