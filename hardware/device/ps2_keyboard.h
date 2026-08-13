#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PS2_KEYBOARD_QUEUE_SIZE 64

typedef struct Ps2Keyboard {
    uint8_t queue[PS2_KEYBOARD_QUEUE_SIZE];
    uint8_t queue_pos, queue_len;
    uint8_t pending_command;
    uint8_t leds;
    uint8_t typematic;
    uint8_t scan_set;
    uint8_t last_sent;
    bool scanning;
} Ps2Keyboard;

void ps2_keyboard_init(Ps2Keyboard *kbd);
void ps2_keyboard_command(Ps2Keyboard *kbd, uint8_t command);
bool ps2_keyboard_has_data(const Ps2Keyboard *kbd);
uint8_t ps2_keyboard_read(Ps2Keyboard *kbd);

/* Host key symbols use the X11/RFB values for non-ASCII keys.  ASCII
 * printable characters may be passed directly. */
void ps2_keyboard_key(Ps2Keyboard *kbd, uint32_t keysym, bool down);
void ps2_keyboard_tap(Ps2Keyboard *kbd, uint32_t keysym);

