#pragma once
/*
 * A standard PS/2 mouse (device ID 00: three buttons as two, no wheel) on
 * an 8042's auxiliary port (i8042.h).
 *
 * Commands: FF reset (FA AA 00), F6 set defaults, F4/F5 enable/disable data
 * reporting, E6/E7 scaling 1:1/2:1, E8 resolution and F3 sample rate (each
 * with a parameter byte), F2 get ID, E9 status request, EB read data (one
 * all-zero packet).  Anything else is acknowledged (FA), rather than
 * wedging a driver's probe.  Power-on state: reporting off, 4 counts/mm
 * (resolution 2), 100 samples/s.
 *
 * The host reports movement with ps2_mouse_report(); the 8042 reads replies
 * and packets from the output queue.
 */
#include <stdbool.h>
#include <stdint.h>

typedef struct Ps2Mouse {
    uint8_t  out[16];
    uint8_t  out_pos, out_len;
    bool     streaming;         /* data reporting enabled (F4) */
    uint8_t  param_cmd;         /* E8 or F3 waiting for its parameter, 0 = none */
    uint8_t  resolution, sample_rate;
    bool     scaling_2to1;
    bool     left, right;       /* the buttons as last reported */
    uint32_t resets;            /* counts FF commands: the host side drops its backlog */
} Ps2Mouse;

void    ps2_mouse_init(Ps2Mouse *m);
void    ps2_mouse_command(Ps2Mouse *m, uint8_t cmd);
bool    ps2_mouse_has_data(const Ps2Mouse *m);
uint8_t ps2_mouse_read(Ps2Mouse *m);

/* Queue one movement packet.  dx/dy are PS/2 counts (positive y is up),
 * clamped to +/-127 with the overflow bits set beyond that; *sent_dx/dy
 * (either may be NULL) get what the packet carried. */
void    ps2_mouse_report(Ps2Mouse *m, int dx, int dy, bool left, bool right,
                         int *sent_dx, int *sent_dy);
