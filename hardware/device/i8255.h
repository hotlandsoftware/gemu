#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Intel 8255 PPI as wired on the IBM 5150/5160 motherboard - ports
 * 0x60-0x63. Port A: keyboard scan code in. Port B: speaker gate/data out,
 * plus a nibble-select bit for Port C's DIP-switch readback. Port C: SW1
 * nibble (equipment config - floppy count, video mode, RAM size) selected
 * by Port B bit 3, plus timer channel 2's output on bit 7.
 *
 * Simplified relative to real silicon: the 8255's general-purpose mode
 * configuration (port 0x63) is accepted but not used to reprogram
 * direction/mode - the 5150's BIOS always uses the fixed A-in/B-out/C-in
 * wiring described above, so nothing on this machine needs more. */

typedef struct {
    uint8_t port_b;
    uint8_t sw1;           /* DIP switch bank 1 - caller sets equipment bits */
    uint8_t kb_scancode;
    bool    kb_data_ready;
    bool    timer2_out;    /* PIT channel 2 output, mirrored onto Port C bit 7 */
} I8255;

void    i8255_reset(I8255 *p);
uint8_t i8255_io_read(I8255 *p, uint16_t port);
void    i8255_io_write(I8255 *p, uint16_t port, uint8_t val);

/* Machine-facing helpers */
void i8255_kb_scancode(I8255 *p, uint8_t code); /* latch a new scan code + request IRQ1 */
void i8255_set_timer2_out(I8255 *p, bool v);
bool i8255_speaker_gate(const I8255 *p); /* Port B bit 0 */
bool i8255_speaker_data(const I8255 *p); /* Port B bit 1 */
