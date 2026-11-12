#pragma once
/*
 * Intel 8255A programmable peripheral interface, generic: the machine
 * supplies what's on the pins.
 *
 * Mode 0 (basic I/O) for each group, with the control word's direction
 * bits, and port C bit set/reset.  Writing a mode word clears all
 * outputs, as the chip does; after RESET every port is an input (control
 * 9Bh).  Modes 1 and 2 (strobed/bidirectional handshakes) are accepted
 * in the control word but run as mode 0: no machine here uses them.
 *
 * Inputs read the pin callbacks (or 0xFF, the pull-ups, without one);
 * ports or nibbles set as outputs read back their output latch.
 */
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t control;
    uint8_t out[3];   /* output latches A, B, C */
    uint8_t (*read_pins)(void *ud, int port);              /* 0 A, 1 B, 2 C */
    void    (*write_pins)(void *ud, int port, uint8_t v);   /* outputs changed */
    void    *ud;
} Ppi8255;

void    ppi8255_reset(Ppi8255 *p);
uint8_t ppi8255_read(Ppi8255 *p, uint8_t reg);      /* A0-A1 */
void    ppi8255_write(Ppi8255 *p, uint8_t reg, uint8_t val);
/* Bits of a port currently configured as inputs. */
uint8_t ppi8255_input_mask(const Ppi8255 *p, int port);
