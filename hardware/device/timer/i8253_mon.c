/* 8253/8254 timer: monitor output (see i8253.h).  Separate from the device
 * so that it links without the monitor. */
#include "timer/i8253.h"
#include "gemu/monitor.h"

void i8253_monitor_print(GemuMonitor *mon, const I8253 *p) {
    for (unsigned c = 0; c < 3; c++)
        gemu_monitor_printf(mon, "PIT%u: mode=%u reload=%u count=%u out=%u "
               "gate=%u\n", c, p->ch[c].mode,
               p->ch[c].reload ? (unsigned)p->ch[c].reload : 65536u,
               (unsigned)p->ch[c].count, p->ch[c].out, p->ch[c].gate);
}
