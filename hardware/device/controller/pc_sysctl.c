/* System control port B and POST port: see pc_sysctl.h. */
#include "controller/pc_sysctl.h"

void pc_sysctl_init(PcSysctl *s, I8253 *pit) {
    s->pit = pit;
    pc_sysctl_reset(s);
}

void pc_sysctl_reset(PcSysctl *s) {
    s->port_b = 0;
    s->post = 0;
    i8253_set_gate(s->pit, 2, false);
}

bool pc_sysctl_io_read(const PcSysctl *s, uint16_t port, uint64_t pit_clocks,
                       uint8_t *val) {
    if (port != 0x61)
        return false;
    *val = (uint8_t)((s->port_b & 0x0F) | (((pit_clocks / 18) & 1) << 4) |
                     (i8253_out(s->pit, 2) ? 0x20 : 0));
    return true;
}

bool pc_sysctl_io_write(PcSysctl *s, uint16_t port, uint8_t val) {
    if (port == 0x80) {
        s->post = val;
        return true;
    }
    if (port != 0x61)
        return false;
    s->port_b = val & 0x0F;
    i8253_set_gate(s->pit, 2, val & 1);
    return true;
}
