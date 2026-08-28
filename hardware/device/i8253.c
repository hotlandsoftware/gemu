#include "i8253.h"
#include <string.h>

void i8253_reset(I8253 *p) {
    memset(p, 0, sizeof *p);
    for (int i = 0; i < 3; i++) p->ch[i].rw_mode = 3;
}

void i8253_io_write(I8253 *p, uint16_t port, uint8_t val) {
    if (port == 0x43) {
        unsigned sel = (val >> 6) & 3;
        if (sel == 3) return; /* read-back command - not needed by PC BIOS */
        I8253Channel *c = &p->ch[sel];
        unsigned rw = (val >> 4) & 3;
        if (rw == 0) { /* counter latch command, not a mode change */
            c->latched_value = c->counter ? c->counter : (c->reload ? c->reload : 0);
            c->latched = true;
            return;
        }
        c->mode = (uint8_t)((val >> 1) & 7);
        c->rw_mode = (uint8_t)rw;
        c->write_msb_next = false;
        c->latched = false;
        return;
    }
    unsigned idx = (unsigned)(port - 0x40);
    if (idx > 2) return;
    I8253Channel *c = &p->ch[idx];
    bool reload_now;
    if (c->rw_mode == 1) { c->reload = (uint16_t)((c->reload & 0xFF00) | val); reload_now = true; }
    else if (c->rw_mode == 2) { c->reload = (uint16_t)((c->reload & 0x00FF) | ((uint16_t)val << 8)); reload_now = true; }
    else if (!c->write_msb_next) { /* LSB then MSB: only the second byte commits */
        c->reload = (uint16_t)((c->reload & 0xFF00) | val);
        c->write_msb_next = true;
        reload_now = false;
    } else {
        c->reload = (uint16_t)((c->reload & 0x00FF) | ((uint16_t)val << 8));
        c->write_msb_next = false;
        reload_now = true;
    }
    if (reload_now) c->counter = c->reload; /* 0 means "65536", same convention i8253_tick() uses */
}

uint8_t i8253_io_read(I8253 *p, uint16_t port) {
    if (port == 0x43) return 0; /* status readback not implemented */
    unsigned idx = (unsigned)(port - 0x40);
    if (idx > 2) return 0xFF;
    I8253Channel *c = &p->ch[idx];
    uint16_t v = c->latched ? c->latched_value : c->counter;
    if (c->rw_mode == 1) return (uint8_t)v;
    if (c->rw_mode == 2) return (uint8_t)(v >> 8);
    if (!c->read_msb_next) { c->read_msb_next = true; return (uint8_t)v; }
    c->read_msb_next = false;
    c->latched = false;
    return (uint8_t)(v >> 8);
}

bool i8253_tick(I8253 *p, uint32_t ticks) {
    bool fired0 = false;
    for (int i = 0; i < 3; i++) {
        I8253Channel *c = &p->ch[i];
        uint32_t reload = c->reload ? c->reload : 0x10000u;
        uint32_t counter = c->counter ? c->counter : reload;
        uint32_t remaining = ticks;
        while (remaining >= counter) {
            remaining -= counter;
            counter = reload;
            c->out_fired = true;
            if (i == 0) fired0 = true;
        }
        counter -= remaining;
        c->counter = (uint16_t)counter; /* wraps 0x10000->0, consistent with the "0 == reload" convention */
    }
    return fired0;
}

bool i8253_ch2_out(const I8253 *p) { return p->ch[2].out_fired; }
