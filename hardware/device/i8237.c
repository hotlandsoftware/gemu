#include "i8237.h"
#include <string.h>

void i8237_reset(I8237 *d) {
    uint8_t pages[4];
    for (int i = 0; i < 4; i++) pages[i] = d->ch[i].page;
    memset(d, 0, sizeof *d);
    for (int i = 0; i < 4; i++) d->ch[i].page = pages[i];
    d->mask = 0x0F;   /* master clear masks every channel */
}

uint8_t i8237_io_read(I8237 *d, uint16_t port) {
    port &= 0x0F;
    if (port < 8) {
        I8237Channel *c = &d->ch[port >> 1];
        uint16_t v = (port & 1) ? c->cur_count : c->cur_addr;
        uint8_t r = d->flipflop ? (uint8_t)(v >> 8) : (uint8_t)v;
        d->flipflop = !d->flipflop;
        return r;
    }
    switch (port) {
    case 0x08: {                               /* status; reading clears TC bits */
        uint8_t s = d->status;
        d->status = (uint8_t)(d->status & 0xF0);
        return s;
    }
    case 0x0D: return d->temp;                 /* temporary register */
    default:   return 0xFF;
    }
}

void i8237_io_write(I8237 *d, uint16_t port, uint8_t val) {
    port &= 0x0F;
    if (port < 8) {
        I8237Channel *c = &d->ch[port >> 1];
        uint16_t *base = (port & 1) ? &c->base_count : &c->base_addr;
        uint16_t *cur  = (port & 1) ? &c->cur_count  : &c->cur_addr;
        if (d->flipflop) *base = (uint16_t)((*base & 0x00FF) | (val << 8));
        else             *base = (uint16_t)((*base & 0xFF00) | val);
        *cur = *base;
        d->flipflop = !d->flipflop;
        return;
    }
    switch (port) {
    case 0x08: d->command = val; break;
    case 0x09:                                 /* request register */
        if (val & 4) d->request |= (uint8_t)(1u << (val & 3));
        else         d->request = (uint8_t)(d->request & ~(1u << (val & 3)));
        break;
    case 0x0A:                                 /* single mask bit */
        if (val & 4) d->mask |= (uint8_t)(1u << (val & 3));
        else         d->mask = (uint8_t)(d->mask & ~(1u << (val & 3)));
        break;
    case 0x0B: d->ch[val & 3].mode = val; break;
    case 0x0C: d->flipflop = false; break;     /* clear byte pointer */
    case 0x0D: i8237_reset(d); break;          /* master clear */
    case 0x0E: d->mask = 0; break;             /* clear all mask bits */
    case 0x0F: d->mask = (uint8_t)(val & 0x0F); break;
    default: break;
    }
}

/* 74LS670 page registers: 0x81 = channel 2, 0x82 = channel 3, 0x83 =
 * channel 1.  0x80 has no register on the 5150. */
static int page_channel(uint16_t port) {
    switch (port & 0x0F) {
    case 1: return 2;
    case 2: return 3;
    case 3: return 1;
    default: return -1;
    }
}

uint8_t i8237_page_read(I8237 *d, uint16_t port) {
    int ch = page_channel(port);
    return ch < 0 ? 0xFF : d->ch[ch].page;
}

void i8237_page_write(I8237 *d, uint16_t port, uint8_t val) {
    int ch = page_channel(port);
    if (ch >= 0) d->ch[ch].page = (uint8_t)(val & 0x0F);
}

/* Step channel ch's address and count by one transfer.  Returns true at
 * terminal count (the count wrapped past zero). */
static bool step(I8237 *d, unsigned ch) {
    I8237Channel *c = &d->ch[ch];
    if (c->mode & 0x20) c->cur_addr--; else c->cur_addr++;
    bool tc = c->cur_count-- == 0;
    if (tc) {
        d->status |= (uint8_t)(1u << ch);
        d->request = (uint8_t)(d->request & ~(1u << ch));
        if (c->mode & 0x10) {                  /* autoinitialize */
            c->cur_addr  = c->base_addr;
            c->cur_count = c->base_count;
        } else {
            d->mask |= (uint8_t)(1u << ch);
        }
    }
    return tc;
}

static bool channel_ready(const I8237 *d, unsigned ch) {
    return !(d->command & 0x04) && !(d->mask & (1u << ch));
}

void i8237_refresh(I8237 *d) {
    if (channel_ready(d, 0)) step(d, 0);
}

bool i8237_transfer(I8237 *d, unsigned ch, uint32_t *phys, bool *tc) {
    if (ch > 3 || !channel_ready(d, ch)) return false;
    I8237Channel *c = &d->ch[ch];
    *phys = ((uint32_t)c->page << 16) | c->cur_addr;
    *tc = step(d, ch);
    return true;
}
