/*
 * 8255A PPI - see ppi8255.h.
 */
#include "ppi8255.h"

void ppi8255_reset(Ppi8255 *p) {
    p->control = 0x9B;   /* mode 0, all inputs */
    p->out[0] = p->out[1] = p->out[2] = 0;
}

uint8_t ppi8255_input_mask(const Ppi8255 *p, int port) {
    switch (port) {
    case 0: return (p->control & 0x10u) ? 0xFF : 0x00;
    case 1: return (p->control & 0x02u) ? 0xFF : 0x00;
    default:
        return (uint8_t)(((p->control & 0x08u) ? 0xF0u : 0) | ((p->control & 0x01u) ? 0x0Fu : 0));
    }
}

static void outputs_changed(Ppi8255 *p, int port) {
    if (p->write_pins) p->write_pins(p->ud, port, p->out[port]);
}

uint8_t ppi8255_read(Ppi8255 *p, uint8_t reg) {
    reg &= 3u;
    if (reg == 3) return 0xFF;   /* the control register can't be read */
    uint8_t in_mask = ppi8255_input_mask(p, reg);
    uint8_t pins = p->read_pins ? p->read_pins(p->ud, reg) : 0xFF;
    return (uint8_t)((pins & in_mask) | (p->out[reg] & ~in_mask));
}

void ppi8255_write(Ppi8255 *p, uint8_t reg, uint8_t val) {
    reg &= 3u;
    if (reg < 3) {
        p->out[reg] = val;
        outputs_changed(p, reg);
        return;
    }
    if (val & 0x80u) {   /* mode set: outputs cleared */
        p->control = val;
        for (int i = 0; i < 3; i++) { p->out[i] = 0; outputs_changed(p, i); }
    } else {             /* port C bit set/reset */
        uint8_t bit = (uint8_t)(1u << ((val >> 1) & 7u));
        if (val & 1u) p->out[2] |= bit;
        else          p->out[2] &= (uint8_t)~bit;
        outputs_changed(p, 2);
    }
}
