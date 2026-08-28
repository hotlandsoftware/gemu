#include "i8255.h"
#include <string.h>

void i8255_reset(I8255 *p) {
    memset(p, 0, sizeof *p);
}

uint8_t i8255_io_read(I8255 *p, uint16_t port) {
    switch (port & 3) {
    case 0: return p->kb_scancode;
    case 1: return p->port_b;
    case 2: {
        uint8_t nibble = (p->port_b & 0x08) ? (uint8_t)(p->sw1 >> 4) : (uint8_t)(p->sw1 & 0x0F);
        uint8_t c = (uint8_t)(nibble & 0x0F);
        if (p->timer2_out) c |= 0x80;
        return c; /* bits 4-6 (cassette-in / I/O-channel-check / parity-check): report no error, always 0 */
    }
    default: return 0xFF; /* port 0x63: mode-control register is write-only on real hardware */
    }
}

void i8255_io_write(I8255 *p, uint16_t port, uint8_t val) {
    if ((port & 3) != 1) return; /* only Port B is writable in the 5150's fixed wiring */
    bool clear = (val & 0x80) != 0 && !(p->port_b & 0x80);
    p->port_b = val;
    if (clear) p->kb_data_ready = false; /* rising edge on PB7 acks/clears the keyboard latch */
}

void i8255_kb_scancode(I8255 *p, uint8_t code) {
    p->kb_scancode = code;
    p->kb_data_ready = true;
}
void i8255_set_timer2_out(I8255 *p, bool v) { p->timer2_out = v; }
bool i8255_speaker_gate(const I8255 *p) { return (p->port_b & 0x01) != 0; }
bool i8255_speaker_data(const I8255 *p) { return (p->port_b & 0x02) != 0; }
