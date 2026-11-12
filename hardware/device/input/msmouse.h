#pragma once
/*
 * Microsoft serial mouse (two buttons), on an 8250's port.
 *
 * The mouse runs on the power it draws from DTR and RTS.  When both come
 * up it starts and answers with 'M', which is how drivers find it (they
 * drop RTS and raise it again).  After that it reports at 1200 baud, 7
 * data bits, in three-byte packets whenever it moves or a button changes:
 *
 *   1  0 1 L R Y7 Y6 X7 X6     (bit 6 marks the first byte)
 *   2  0 0 X5 X4 X3 X2 X1 X0
 *   3  0 0 Y5 Y4 Y3 Y2 Y1 Y0
 *
 * X and Y are signed counts since the last packet (Y grows downward).
 * Motion bigger than one packet carries is sent over the next ones.
 */
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t clock_hz;          /* what msmouse_tick() counts */

    bool     powered;
    uint32_t start_wait;        /* clocks until it has started and sends 'M' */
    uint8_t  q[4];              /* bytes waiting to go to the UART */
    int      qn, qpos;
    int32_t  dx, dy;            /* motion not yet reported */
    bool     left, right;       /* the buttons now */
    bool     sent_left, sent_right;
} MsMouse;

void msmouse_init(MsMouse *m, uint32_t clock_hz);
void msmouse_tick(MsMouse *m, uint32_t clocks);
/* Host input: motion since the last call, and the buttons. */
void msmouse_input(MsMouse *m, int dx, int dy, bool left, bool right);

/* For the 8250 (Ins8250's dev_lines / dev_rx, with dev = the MsMouse) */
void msmouse_lines(void *m, bool dtr, bool rts);
bool msmouse_rx(void *m, uint8_t *b);
