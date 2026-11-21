#pragma once
/*
 * Intel 8042 keyboard controller, PS/2 style: a keyboard (ps2_keyboard.h) on
 * the first port and, optionally, a mouse (ps2_mouse.h) on the auxiliary
 * port.  Data at 60h, status (read) and commands (write) at 64h.
 *
 * Controller commands: 20h/60h read/write the command byte, AAh self-test
 * (55h), ABh/A9h keyboard/aux interface tests (aux: 00h with a mouse
 * attached, 01h without), ADh/AEh and A7h/A8h disable/enable the ports
 * (recorded in the command byte), D0h read the output port, D1h write it
 * (ignored), D4h send the next data byte to the mouse.  Other data bytes go
 * to the keyboard.  Writing either port clears the controller's own
 * output buffer.
 *
 * The real chip has one output buffer; here the controller, keyboard and
 * mouse keep their own queues, read in this order: mouse, controller,
 * keyboard.  A mouse packet already started is never interleaved with
 * other bytes, which would break its framing.  Status: OBF (bit 0) when any
 * queue has data, AUXB (bit 5) when the mouse's is next, system flag
 * (bit 2) always.  Interrupts are left to the machine: i8042_kbd_pending()
 * and i8042_aux_pending() are the IRQ1/IRQ12 sources, the latter enabled by
 * command-byte bit 1.
 */
#include "input/ps2_keyboard.h"
#include "input/ps2_mouse.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    Ps2Keyboard *kbd;           /* wiring */
    Ps2Mouse    *aux;           /* wiring; NULL: nothing on the aux port */
    uint8_t command_byte;
    uint8_t pending_write;      /* 0 keyboard, 1 command byte, 2 output port, 3 aux */
    uint8_t out[8];
    uint8_t out_pos, out_len;
} I8042;

void    i8042_init(I8042 *k, Ps2Keyboard *kbd, Ps2Mouse *aux);
uint8_t i8042_read_data(I8042 *k);
uint8_t i8042_read_status(const I8042 *k);
void    i8042_write_data(I8042 *k, uint8_t val);
void    i8042_write_command(I8042 *k, uint8_t cmd);

/* A controller or keyboard byte waiting (the IRQ1 source). */
bool    i8042_kbd_pending(const I8042 *k);
/* A mouse byte waiting (the IRQ12 source). */
bool    i8042_aux_pending(const I8042 *k);
/* Command-byte bit 1: the controller drives IRQ12. */
bool    i8042_aux_irq_enabled(const I8042 *k);
/* Nothing waiting anywhere. */
bool    i8042_empty(const I8042 *k);
