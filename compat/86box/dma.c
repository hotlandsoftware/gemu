/*
 * GEMU's 86Box layer: DMA (<86box/dma.h>) on the machine's 8237 - GEMU's
 * i8237 device, found in the machine by its type (the first one).
 *
 * Like 86Box's dma.c: a cycle on a channel that's masked, on a disabled
 * controller or programmed the other way gives DMA_NODATA; a read marks
 * the last byte (terminal count) with DMA_OVER; a write returns DMA_OVER
 * only when the transfer ended and masked the channel, not on an
 * auto-initialise wrap.
 */
#include "compat_priv.h"
#include <86box/86box.h>
#include <86box/dma.h>
#include "controller/i8237_dev.h"
#include <string.h>

static bool find_dma(GemuDevice *d, void *ud) {
    if (d->type && !strcmp(d->type->name, "i8237")) {
        *(GemuDevice **)ud = d;
        return false;
    }
    return true;
}

static GemuDevice *controller(int channel) {
    Gemu86BoxDev *b = gemu_86box_need("DMA");
    if (channel < 0 || channel > 3)
        return NULL;                  /* the AT's second controller: not yet */
    if (!b->dma)
        gemu_device_walk(gemu_machine_root(b->dev->machine), find_dma, &b->dma);
    return b->dma;
}

int dma_channel_read(int channel) {
    GemuDevice *d = controller(channel);
    if (!d || (gemu_i8237(d)->ch[channel].mode & 0x0C) != 0x08)    /* a read transfer */
        return DMA_NODATA;
    uint8_t v;
    bool tc;
    if (!gemu_i8237_from_mem(d, (unsigned)channel, &v, &tc))
        return DMA_NODATA;
    return v | (tc ? DMA_OVER : 0);
}

int dma_channel_write(int channel, uint16_t val) {
    GemuDevice *d = controller(channel);
    if (!d || (gemu_i8237(d)->ch[channel].mode & 0x0C) != 0x04)    /* a write transfer */
        return DMA_NODATA;
    bool tc;
    if (!gemu_i8237_to_mem(d, (unsigned)channel, (uint8_t)val, &tc))
        return DMA_NODATA;
    return tc && !(gemu_i8237(d)->ch[channel].mode & 0x10) ? DMA_OVER : 0;
}

void dma_set_drq(int channel, int set) {
    GemuDevice *d = controller(channel);
    if (!d)
        return;
    I8237 *c = gemu_i8237(d);
    uint8_t bit = (uint8_t)(0x10 << channel);
    c->status = set ? (uint8_t)(c->status | bit) : (uint8_t)(c->status & ~bit);
}
