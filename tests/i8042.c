/* The 8042 with a PS/2 keyboard and mouse.  Run: ninja check-i8042 */
#include "input/i8042.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

static void drain(I8042 *k) {
    for (unsigned i = 0; i < 64 && (i8042_read_status(k) & 1); i++)
        i8042_read_data(k);
}
static void mouse_cmd(I8042 *k, uint8_t cmd) {
    i8042_write_command(k, 0xD4);
    i8042_write_data(k, cmd);
}

static void test_controller(void) {
    Ps2Keyboard kbd;
    ps2_keyboard_init(&kbd);
    I8042 k;
    i8042_init(&k, &kbd, NULL);
    drain(&k);
    CHECK(i8042_read_status(&k) == 0x04 && i8042_empty(&k));

    i8042_write_command(&k, 0xAA);
    CHECK(i8042_read_status(&k) == 0x05 && i8042_read_data(&k) == 0x55);
    i8042_write_command(&k, 0xAB);
    CHECK(i8042_read_data(&k) == 0x00);
    i8042_write_command(&k, 0xA9);                  /* nothing on aux */
    CHECK(i8042_read_data(&k) == 0x01);
    i8042_write_command(&k, 0xD0);
    CHECK(i8042_read_data(&k) == 0x03);

    i8042_write_command(&k, 0x60);
    i8042_write_data(&k, 0x47);
    i8042_write_command(&k, 0x20);
    CHECK(i8042_read_data(&k) == 0x47 && i8042_aux_irq_enabled(&k));
    i8042_write_command(&k, 0xAD);
    i8042_write_command(&k, 0xA7);
    i8042_write_command(&k, 0x20);
    CHECK(i8042_read_data(&k) == 0x77);
    i8042_write_command(&k, 0xAE);
    i8042_write_command(&k, 0xA8);
    i8042_write_command(&k, 0x20);
    CHECK(i8042_read_data(&k) == 0x47);

    /* A command clears an unread controller reply. */
    i8042_write_command(&k, 0xAA);
    i8042_write_command(&k, 0xAE);
    CHECK(!(i8042_read_status(&k) & 1));

    /* D1 takes a data byte that goes nowhere; D4 with no mouse eats one. */
    i8042_write_command(&k, 0xD1);
    i8042_write_data(&k, 0xDF);
    i8042_write_command(&k, 0xD4);
    i8042_write_data(&k, 0xFF);
    CHECK(i8042_empty(&k));

    /* Plain data goes to the keyboard: ECHO comes back. */
    i8042_write_data(&k, 0xEE);
    CHECK(i8042_kbd_pending(&k) && i8042_read_data(&k) == 0xEE);
    ps2_keyboard_tap(&kbd, 'a');
    CHECK(i8042_read_status(&k) == 0x05);
    CHECK(i8042_read_data(&k) == 0x1E && i8042_read_data(&k) == 0x9E);
}

static void test_mouse(void) {
    Ps2Keyboard kbd;
    Ps2Mouse m;
    ps2_keyboard_init(&kbd);
    ps2_mouse_init(&m);
    I8042 k;
    i8042_init(&k, &kbd, &m);
    drain(&k);
    i8042_write_command(&k, 0xA9);
    CHECK(i8042_read_data(&k) == 0x00);

    mouse_cmd(&k, 0xFF);                            /* reset */
    CHECK(m.resets == 1);
    CHECK(i8042_read_status(&k) == 0x25 && i8042_aux_pending(&k));
    CHECK(i8042_read_data(&k) == 0xFA && i8042_read_data(&k) == 0xAA &&
          i8042_read_data(&k) == 0x00 && !i8042_aux_pending(&k));
    mouse_cmd(&k, 0xF2);
    CHECK(i8042_read_data(&k) == 0xFA && i8042_read_data(&k) == 0x00);
    mouse_cmd(&k, 0xF3);                            /* rate 40 */
    mouse_cmd(&k, 40);
    mouse_cmd(&k, 0xE8);                            /* resolution 3 */
    mouse_cmd(&k, 3);
    mouse_cmd(&k, 0xE7);
    mouse_cmd(&k, 0xF4);
    for (unsigned i = 0; i < 6; i++) CHECK(i8042_read_data(&k) == 0xFA);
    mouse_cmd(&k, 0xE9);                            /* status */
    CHECK(i8042_read_data(&k) == 0xFA && i8042_read_data(&k) == 0x30 &&
          i8042_read_data(&k) == 3 && i8042_read_data(&k) == 40);
    mouse_cmd(&k, 0xF6);                            /* defaults */
    mouse_cmd(&k, 0xE9);
    CHECK(i8042_read_data(&k) == 0xFA && i8042_read_data(&k) == 0xFA &&
          i8042_read_data(&k) == 0x00 && i8042_read_data(&k) == 2 &&
          i8042_read_data(&k) == 100);
    mouse_cmd(&k, 0x5A);                            /* unknown: acknowledged */
    CHECK(i8042_read_data(&k) == 0xFA);

    /* Reports: sign bits, clamping with the overflow bits, buttons. */
    int sx, sy;
    ps2_mouse_report(&m, 5, -3, true, false, &sx, &sy);
    CHECK(sx == 5 && sy == -3 && m.left && !m.right);
    CHECK(i8042_read_data(&k) == 0x29 && i8042_read_data(&k) == 5 &&
          i8042_read_data(&k) == 0xFD);
    ps2_mouse_report(&m, 300, 0, false, true, &sx, &sy);
    CHECK(sx == 127 && sy == 0);
    CHECK(i8042_read_data(&k) == 0x4A && i8042_read_data(&k) == 127 &&
          i8042_read_data(&k) == 0);

    /* The mouse is read first, so a packet isn't split by other bytes. */
    ps2_mouse_report(&m, 1, 1, false, false, NULL, NULL);
    ps2_keyboard_tap(&kbd, 'a');
    CHECK(i8042_read_data(&k) == 0x08);
    i8042_write_command(&k, 0xAA);                  /* controller reply waits */
    CHECK(i8042_read_data(&k) == 1 && i8042_read_data(&k) == 1);
    CHECK(i8042_read_data(&k) == 0x55);
    CHECK(i8042_read_data(&k) == 0x1E && i8042_read_data(&k) == 0x9E);
    CHECK(i8042_empty(&k));

    /* The mouse's own reset state. */
    ps2_mouse_init(&m);
    CHECK(!m.streaming && m.resolution == 2 && m.sample_rate == 100 && m.resets == 0);
}

int main(void) {
    test_controller();
    test_mouse();
    printf("i8042: %u checks passed\n", checks);
    return 0;
}
