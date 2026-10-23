#include "i8253.h"
#include <string.h>

static void set_out(I8253 *p, unsigned idx, bool level) {
    I8253Channel *c = &p->ch[idx];
    if (c->out == level) return;
    c->out = level;
    if (p->out_changed) p->out_changed(p->ud, idx, level);
}

void i8253_reset(I8253 *p) {
    void (*cb)(void *, unsigned, bool) = p->out_changed;
    void *ud = p->ud;
    memset(p, 0, sizeof *p);
    p->out_changed = cb;
    p->ud = ud;
    for (int i = 0; i < 3; i++) {
        p->ch[i].rw_mode = 3;
        p->ch[i].gate = true;
        p->ch[i].out = true;
    }
}

/* Value the counting element holds for a reload, honouring mode 3's
 * halved-period counting (it decrements by two). */
static uint32_t reload_value(const I8253Channel *c) {
    return c->reload ? c->reload : 0x10000u;
}

static void count_written(I8253 *p, unsigned idx) {
    I8253Channel *c = &p->ch[idx];
    switch (c->mode) {
    case 0:                       /* restart the count, output low until TC */
        set_out(p, idx, false);
        c->load_pending = true;
        break;
    case 4:                       /* software strobe: restart */
        c->load_pending = true;
        break;
    case 2: case 3:               /* periodic: new count applies at the next
                                     reload, or now if not yet running */
        if (!c->counting) c->load_pending = true;
        break;
    default:                      /* 1, 5: wait for a gate trigger */
        break;
    }
}

void i8253_io_write(I8253 *p, uint16_t port, uint8_t val) {
    if ((port & 3) == 3) {
        unsigned sel = (val >> 6) & 3;
        if (sel == 3) return;     /* read-back is an 8254 command */
        I8253Channel *c = &p->ch[sel];
        unsigned rw = (val >> 4) & 3;
        if (rw == 0) {            /* counter latch */
            if (!c->latched) {
                c->latched_value = c->count;
                c->latched = true;
            }
            return;
        }
        c->mode = (uint8_t)((val >> 1) & 7);
        if (c->mode > 5) c->mode = (uint8_t)(c->mode - 4);   /* 6,7 = 2,3 */
        c->rw_mode = (uint8_t)rw;
        c->write_msb_next = c->read_msb_next = false;
        c->latched = false;
        c->counting = c->load_pending = c->trigger = c->strobe = false;
        /* Programming a mode sets the initial output level. */
        set_out(p, sel, c->mode != 0);
        return;
    }
    unsigned idx = port & 3;
    I8253Channel *c = &p->ch[idx];
    switch (c->rw_mode) {
    case 1: c->reload = (uint16_t)((c->reload & 0xFF00) | val); break;
    case 2: c->reload = (uint16_t)((c->reload & 0x00FF) | (val << 8)); break;
    default:
        if (!c->write_msb_next) {
            c->reload = (uint16_t)((c->reload & 0xFF00) | val);
            c->write_msb_next = true;
            if (c->mode == 0) c->counting = false;   /* LSB write halts mode 0 */
            return;
        }
        c->reload = (uint16_t)((c->reload & 0x00FF) | (val << 8));
        c->write_msb_next = false;
        break;
    }
    count_written(p, idx);
}

uint8_t i8253_io_read(I8253 *p, uint16_t port) {
    if ((port & 3) == 3) return 0xFF;
    I8253Channel *c = &p->ch[port & 3];
    uint16_t v = c->latched ? c->latched_value : c->count;
    uint8_t r;
    switch (c->rw_mode) {
    case 1: r = (uint8_t)v; c->latched = false; break;
    case 2: r = (uint8_t)(v >> 8); c->latched = false; break;
    default:
        if (!c->read_msb_next) {
            r = (uint8_t)v;
            c->read_msb_next = true;
        } else {
            r = (uint8_t)(v >> 8);
            c->read_msb_next = false;
            c->latched = false;
        }
        break;
    }
    return r;
}

void i8253_set_gate(I8253 *p, unsigned idx, bool level) {
    I8253Channel *c = &p->ch[idx];
    if (level && !c->gate) c->trigger = true;
    c->gate = level;
    /* Modes 2 and 3 force the output high while the gate is low. */
    if (!level && (c->mode == 2 || c->mode == 3)) set_out(p, idx, true);
}

bool i8253_out(const I8253 *p, unsigned idx) { return p->ch[idx].out; }

/* Load the counting element from the count register; mode 3 loads an
 * even value so that it can count down by two. */
static void load(I8253Channel *c) {
    uint32_t v = reload_value(c);
    if (c->mode == 3) v &= ~1u;
    c->count = (uint16_t)v;
    c->counting = true;
}

static void clock_channel(I8253 *p, unsigned idx) {
    I8253Channel *c = &p->ch[idx];
    bool trigger = c->trigger;
    c->trigger = false;

    if (c->strobe) {              /* end of a mode 4/5 one-clock pulse */
        c->strobe = false;
        set_out(p, idx, true);
    }

    switch (c->mode) {
    case 0:
    case 4:
        if (c->load_pending) {
            c->load_pending = false;
            load(c);
            return;
        }
        if (!c->counting || !c->gate) return;
        c->count--;
        if (c->count == 0) {
            if (c->mode == 0) set_out(p, idx, true);
            else { set_out(p, idx, false); c->strobe = true; }
        }
        return;

    case 1:
    case 5:
        if (trigger) {            /* gate edge (re)starts the one-shot */
            load(c);
            if (c->mode == 1) set_out(p, idx, false);
            return;
        }
        if (!c->counting) return;
        c->count--;
        if (c->count == 0) {
            if (c->mode == 1) set_out(p, idx, true);
            else { set_out(p, idx, false); c->strobe = true; }
            c->counting = false;
        }
        return;

    case 2:
        if (c->load_pending || trigger) {
            c->load_pending = false;
            load(c);
            set_out(p, idx, true);
            return;
        }
        if (!c->counting || !c->gate) return;
        c->count--;
        if (c->count == 1) {
            set_out(p, idx, false);       /* low for one clock... */
        } else if (c->count == 0) {
            load(c);                      /* ...then reload, output high */
            set_out(p, idx, true);
        }
        return;

    case 3:
        if (c->load_pending || trigger) {
            c->load_pending = false;
            load(c);
            set_out(p, idx, true);
            return;
        }
        if (!c->counting || !c->gate) return;
        c->count = (uint16_t)(c->count - 2);
        if (c->count == 0) {
            set_out(p, idx, !c->out);
            load(c);
        }
        return;
    }
}

void i8253_tick(I8253 *p, uint32_t ticks) {
    while (ticks--) {
        clock_channel(p, 0);
        clock_channel(p, 1);
        clock_channel(p, 2);
    }
}
