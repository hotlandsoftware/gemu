/* 8042 keyboard controller: monitor output (see i8042.h).  Separate from the
 * device so that it links without the monitor. */
#include "input/i8042.h"
#include "gemu/monitor.h"

void i8042_monitor_print(GemuMonitor *mon, const I8042 *k) {
    const Ps2Keyboard *kbd = k->kbd;
    const Ps2Mouse *m = k->aux;
    gemu_monitor_printf(mon, "8042: command=%02X pending_write=%u out=%u/%u "
           "kbd=%u/%u aux=%u/%u\n", k->command_byte, k->pending_write,
           k->out_pos, k->out_len, kbd->queue_pos, kbd->queue_len,
           m ? m->out_pos : 0, m ? m->out_len : 0);
    if (ps2_keyboard_has_data(kbd)) {
        gemu_monitor_printf(mon, "keyboard bytes:");
        for (unsigned i = kbd->queue_pos; i < kbd->queue_len; i++)
            gemu_monitor_printf(mon, " %02X", kbd->queue[i]);
        gemu_monitor_printf(mon, "\n");
    }
    if (m)
        gemu_monitor_printf(mon, "mouse: streaming=%u resolution=%u rate=%u "
               "scaling=%s param_cmd=%02x buttons=%s%s resets=%u\n",
               m->streaming, m->resolution, m->sample_rate,
               m->scaling_2to1 ? "2:1" : "1:1", m->param_cmd,
               m->left ? "L" : "-", m->right ? "R" : "-", m->resets);
    else
        gemu_monitor_printf(mon, "mouse: none on the aux port\n");
}
