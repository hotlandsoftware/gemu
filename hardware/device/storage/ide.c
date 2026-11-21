/* IDE channel: see ide.h. */
#include "storage/ide.h"
#include <stddef.h>

void ide_reset(IdeChannel *c) {
    c->device = 0;
    c->control = 0;
}

IdeDevice *ide_selected(const IdeChannel *c) {
    return c->dev[(c->device >> 4) & 1];
}

uint64_t ide_read(IdeChannel *c, unsigned reg, unsigned size) {
    IdeDevice *d = ide_selected(c);
    if (!d)
        return reg == 6 ? c->device : 0;
    switch (reg) {
    case 0: return d->data_read(d, size);
    case 8: return d->reg_read(d, 7, false);
    default: return d->reg_read(d, reg & 7, reg == 7);
    }
}

void ide_write(IdeChannel *c, unsigned reg, uint64_t val, unsigned size) {
    IdeDevice *d = ide_selected(c);
    switch (reg) {
    case 0:
        if (d) d->data_write(d, val, size);
        return;
    case 7:
        if (d) d->command(d, (uint8_t)val);
        return;
    case 8:
        for (unsigned i = 0; i < 2; i++)
            if (c->dev[i]) c->dev[i]->control(c->dev[i], (uint8_t)val);
        /* The end of a software reset leaves device 0 selected. */
        if ((c->control & 4) && !(val & 4)) c->device = 0;
        c->control = (uint8_t)val;
        return;
    default:
        if (reg == 6) c->device = (uint8_t)val;
        for (unsigned i = 0; i < 2; i++)
            if (c->dev[i]) c->dev[i]->reg_write(c->dev[i], reg, (uint8_t)val);
        return;
    }
}

bool ide_intrq(const IdeChannel *c) {
    const IdeDevice *d = ide_selected(c);
    return d && d->intrq(d);
}
