/* 8042 keyboard controller: see i8042.h. */
#include "input/i8042.h"
#include <string.h>

void i8042_init(I8042 *k, Ps2Keyboard *kbd, Ps2Mouse *aux) {
    memset(k, 0, sizeof *k);
    k->kbd = kbd;
    k->aux = aux;
}

bool i8042_aux_pending(const I8042 *k) {
    return k->aux && ps2_mouse_has_data(k->aux);
}

bool i8042_kbd_pending(const I8042 *k) {
    return k->out_pos < k->out_len || ps2_keyboard_has_data(k->kbd);
}

bool i8042_aux_irq_enabled(const I8042 *k) {
    return (k->command_byte & 0x02) != 0;
}

bool i8042_empty(const I8042 *k) {
    return !i8042_aux_pending(k) && k->out_len == 0 &&
           !ps2_keyboard_has_data(k->kbd);
}

static void reply(I8042 *k, uint8_t v) {
    k->out[0] = v;
    k->out_pos = 0;
    k->out_len = 1;
}

uint8_t i8042_read_data(I8042 *k) {
    if (i8042_aux_pending(k))
        return ps2_mouse_read(k->aux);
    if (k->out_pos < k->out_len) {
        uint8_t v = k->out[k->out_pos++];
        if (k->out_pos == k->out_len)
            k->out_pos = k->out_len = 0;
        return v;
    }
    if (ps2_keyboard_has_data(k->kbd))
        return ps2_keyboard_read(k->kbd);
    return 0;
}

uint8_t i8042_read_status(const I8042 *k) {
    bool aux = i8042_aux_pending(k);
    bool full = aux || k->out_len || ps2_keyboard_has_data(k->kbd);
    return (uint8_t)((full ? 0x01 : 0) | (aux ? 0x20 : 0) | 0x04);
}

void i8042_write_command(I8042 *k, uint8_t cmd) {
    k->out_pos = k->out_len = 0;
    switch (cmd) {
    case 0x20: reply(k, k->command_byte); break;
    case 0x60: k->pending_write = 1; break;
    case 0xAA: reply(k, 0x55); break;                   /* self-test passed */
    case 0xAB: reply(k, 0x00); break;                   /* keyboard port OK */
    case 0xA9: reply(k, k->aux ? 0x00 : 0x01); break;   /* aux port */
    case 0xAD: k->command_byte |= 0x10; break;
    case 0xAE: k->command_byte &= (uint8_t)~0x10; break;
    case 0xA7: k->command_byte |= 0x20; break;
    case 0xA8: k->command_byte &= (uint8_t)~0x20; break;
    case 0xD0: reply(k, 0x03); break;      /* output port: reset off, A20 on */
    case 0xD1: k->pending_write = 2; break;
    case 0xD4: k->pending_write = 3; break;
    default: break;
    }
}

void i8042_write_data(I8042 *k, uint8_t val) {
    k->out_pos = k->out_len = 0;
    switch (k->pending_write) {
    case 1:
        k->command_byte = val;
        break;
    case 3:
        /* With nothing on the aux port the byte is consumed and no reply
         * comes, so a driver's mouse probe times out. */
        if (k->aux)
            ps2_mouse_command(k->aux, val);
        break;
    case 0:
        ps2_keyboard_command(k->kbd, val);
        break;
    default:                                /* output port: ignored */
        break;
    }
    k->pending_write = 0;
}
