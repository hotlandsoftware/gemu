/* Microsoft serial mouse: see msmouse.h. */
#include "input/msmouse.h"
#include <string.h>

#define START_US 10000          /* from power to 'M' */

void msmouse_init(MsMouse *m, uint32_t clock_hz) {
    memset(m, 0, sizeof(*m));
    m->clock_hz = clock_hz;
}

void msmouse_lines(void *p, bool dtr, bool rts) {
    MsMouse *m = p;
    bool on = dtr && rts;
    if (on == m->powered) return;
    m->powered = on;
    m->qn = m->qpos = 0;
    m->dx = m->dy = 0;
    m->sent_left = m->sent_right = false;
    m->start_wait = on ? (uint32_t)((uint64_t)START_US * m->clock_hz / 1000000u) : 0;
    if (on && !m->start_wait) m->start_wait = 1;
}

void msmouse_tick(MsMouse *m, uint32_t clocks) {
    if (!m->start_wait) return;
    if (clocks < m->start_wait) { m->start_wait -= clocks; return; }
    m->start_wait = 0;
    m->q[0] = 'M';
    m->qn = 1;
    m->qpos = 0;
}

void msmouse_input(MsMouse *m, int dx, int dy, bool left, bool right) {
    m->left = left;
    m->right = right;
    if (!m->powered || m->start_wait) return;   /* a mouse that's off sees nothing */
    m->dx += dx;
    m->dy += dy;
    /* don't let a long stall build up minutes of motion */
    if (m->dx >  4096) m->dx =  4096;
    if (m->dx < -4096) m->dx = -4096;
    if (m->dy >  4096) m->dy =  4096;
    if (m->dy < -4096) m->dy = -4096;
}

static int clamp8(int32_t v) { return v > 127 ? 127 : v < -128 ? -128 : (int)v; }

bool msmouse_rx(void *p, uint8_t *b) {
    MsMouse *m = p;
    if (!m->powered || m->start_wait) return false;
    if (m->qpos == m->qn) {
        m->qn = m->qpos = 0;
        if (!m->dx && !m->dy && m->left == m->sent_left && m->right == m->sent_right)
            return false;
        int x = clamp8(m->dx), y = clamp8(m->dy);
        m->dx -= x;
        m->dy -= y;
        m->sent_left = m->left;
        m->sent_right = m->right;
        m->q[0] = (uint8_t)(0x40 | (m->left ? 0x20 : 0) | (m->right ? 0x10 : 0) |
                            ((y >> 4) & 0x0C) | ((x >> 6) & 0x03));
        m->q[1] = (uint8_t)(x & 0x3F);
        m->q[2] = (uint8_t)(y & 0x3F);
        m->qn = 3;
    }
    *b = m->q[m->qpos++];
    return true;
}
