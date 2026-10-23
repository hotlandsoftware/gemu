#include "i8255.h"
#include <string.h>

void i8255_reset(I8255 *p) {
    uint8_t sw1 = p->sw1, sw2 = p->sw2;
    memset(p, 0, sizeof *p);
    p->sw1 = sw1;
    p->sw2 = sw2;
}

uint8_t i8255_io_read(I8255 *p, uint16_t port) {
    switch (port & 3) {
    case 0: return (p->port_b & 0x80) ? p->sw1 : p->kb_data;
    case 1: return p->port_b;
    case 2: {
        uint8_t c = (p->port_b & 0x04) ? (uint8_t)(p->sw2 & 0x0F)
                                       : (uint8_t)((p->sw2 >> 4) & 0x01);
        if (p->cassette_in) c |= 0x10;
        if (p->timer2_out)  c |= 0x20;
        return c;           /* no I/O channel or parity errors */
    }
    default: return 0xFF;   /* mode register is write-only */
    }
}

void i8255_io_write(I8255 *p, uint16_t port, uint8_t val) {
    if ((port & 3) == 1) p->port_b = val;
}
