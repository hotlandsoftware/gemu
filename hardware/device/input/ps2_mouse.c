/* PS/2 mouse: see ps2_mouse.h. */
#include "input/ps2_mouse.h"
#include <string.h>

static void queue_byte(Ps2Mouse *m, uint8_t byte) {
    if (m->out_pos) {
        memmove(m->out, m->out + m->out_pos, m->out_len - m->out_pos);
        m->out_len -= m->out_pos;
        m->out_pos = 0;
    }
    if (m->out_len < sizeof(m->out))
        m->out[m->out_len++] = byte;
}

static void defaults(Ps2Mouse *m) {
    m->streaming = false;
    m->resolution = 2;
    m->sample_rate = 100;
    m->scaling_2to1 = false;
}

void ps2_mouse_init(Ps2Mouse *m) {
    memset(m, 0, sizeof *m);
    defaults(m);
}

bool ps2_mouse_has_data(const Ps2Mouse *m) {
    return m->out_pos < m->out_len;
}

uint8_t ps2_mouse_read(Ps2Mouse *m) {
    if (m->out_pos >= m->out_len)
        return 0;
    uint8_t v = m->out[m->out_pos++];
    if (m->out_pos == m->out_len)
        m->out_pos = m->out_len = 0;
    return v;
}

void ps2_mouse_command(Ps2Mouse *m, uint8_t cmd) {
    if (m->param_cmd) {                         /* a command's parameter */
        if (m->param_cmd == 0xE8)
            m->resolution = cmd;
        else if (m->param_cmd == 0xF3)
            m->sample_rate = cmd;
        m->param_cmd = 0;
        queue_byte(m, 0xFA);
        return;
    }
    switch (cmd) {
    case 0xFF:                                  /* reset */
        queue_byte(m, 0xFA);
        queue_byte(m, 0xAA);
        queue_byte(m, 0x00);
        defaults(m);
        m->left = m->right = false;
        m->resets++;
        break;
    case 0xF6:                                  /* set defaults */
        queue_byte(m, 0xFA);
        defaults(m);
        break;
    case 0xF4:                                  /* enable data reporting */
        queue_byte(m, 0xFA);
        m->streaming = true;
        break;
    case 0xF5:                                  /* disable data reporting */
        queue_byte(m, 0xFA);
        m->streaming = false;
        break;
    case 0xE6:                                  /* scaling 1:1 */
        queue_byte(m, 0xFA);
        m->scaling_2to1 = false;
        break;
    case 0xE7:                                  /* scaling 2:1 */
        queue_byte(m, 0xFA);
        m->scaling_2to1 = true;
        break;
    case 0xE8:                                  /* set resolution */
    case 0xF3:                                  /* set sample rate */
        queue_byte(m, 0xFA);
        m->param_cmd = cmd;
        break;
    case 0xF2:                                  /* get device ID */
        queue_byte(m, 0xFA);
        queue_byte(m, 0x00);
        break;
    case 0xE9:                                  /* status request */
        queue_byte(m, 0xFA);
        queue_byte(m, (uint8_t)((m->scaling_2to1 ? 0x10 : 0) |
                                (m->streaming ? 0x20 : 0) |
                                (m->left ? 0x01 : 0) | (m->right ? 0x04 : 0)));
        queue_byte(m, m->resolution);
        queue_byte(m, m->sample_rate);
        break;
    case 0xEB:                                  /* read data: no motion kept */
        queue_byte(m, 0xFA);
        queue_byte(m, (uint8_t)(0x08 | (m->left ? 0x01 : 0) | (m->right ? 0x02 : 0)));
        queue_byte(m, 0x00);
        queue_byte(m, 0x00);
        break;
    default:
        queue_byte(m, 0xFA);
        break;
    }
}

void ps2_mouse_report(Ps2Mouse *m, int dx, int dy, bool left, bool right,
                      int *sent_dx, int *sent_dy) {
    int cdx = dx < -127 ? -127 : dx > 127 ? 127 : dx;
    int cdy = dy < -127 ? -127 : dy > 127 ? 127 : dy;
    uint8_t flags = (uint8_t)((left ? 0x01 : 0) | (right ? 0x02 : 0) | 0x08 |
                              (cdx < 0 ? 0x10 : 0) | (cdy < 0 ? 0x20 : 0) |
                              (dx != cdx ? 0x40 : 0) | (dy != cdy ? 0x80 : 0));
    queue_byte(m, flags);
    queue_byte(m, (uint8_t)(int8_t)cdx);
    queue_byte(m, (uint8_t)(int8_t)cdy);
    m->left = left;
    m->right = right;
    if (sent_dx) *sent_dx = cdx;
    if (sent_dy) *sent_dy = cdy;
}
