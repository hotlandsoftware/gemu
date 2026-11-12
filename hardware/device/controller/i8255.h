#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Intel 8255 PPI as wired on the IBM 5150 motherboard - ports 0x60-0x63.
 *
 * Port A (in):  keyboard scan code, or DIP switch block SW1 when PB7 is set.
 * Port B (out): PB0 timer 2 gate, PB1 speaker data, PB2 selects what port C
 *               bits 0-3 show, PB3 cassette motor off, PB4/PB5 disable
 *               parity / I/O channel check, PB6 low holds the keyboard clock
 *               low (keyboard reset), PB7 clears the keyboard and selects SW1.
 * Port C (in):  PC0-3 SW2 switches 1-4 (PB2 set) or switch 5 on PC0 (PB2
 *               clear); PC4 cassette data in; PC5 timer 2 output; PC6 I/O
 *               channel check; PC7 RAM parity check.  With the cassette
 *               motor off, PC4 reads back timer 2 (the cassette wrap).
 *
 * The 5150 BIOS always programs the fixed A-in/B-out/C-in configuration
 * (0x99), so the mode register at 0x63 is accepted and ignored.  The
 * keyboard handshake driven by PB6/PB7 is the machine's job; it reads
 * port_b after writes. */

typedef struct {
    uint8_t port_b;
    uint8_t sw1;           /* SW1: floppy/8087/planar RAM/video/drives */
    uint8_t sw2;           /* SW2: expansion RAM size in 32K blocks (bits 0-4) */
    uint8_t kb_data;       /* keyboard shift register */
    bool    timer2_out;    /* PIT channel 2 output -> PC5 */
    bool    cassette_in;   /* cassette data -> PC4 */
} I8255;

void    i8255_reset(I8255 *p);
uint8_t i8255_io_read(I8255 *p, uint16_t port);
void    i8255_io_write(I8255 *p, uint16_t port, uint8_t val);
