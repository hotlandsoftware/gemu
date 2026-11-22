/* VGA core: monitor commands (see vga.h).  Separate from the device so that
 * it links without the monitor. */
#include "vga.h"
#include "gemu/monitor.h"
#include <stdio.h>

static GemuMonCmd cmd_vgareg(GemuMonitor *mon, const char *args, void *ud) {
    const Vga *v = ud;
    (void)args;
    gemu_monitor_printf(mon, "vga: misc=%02x text_mode=%d blanked=%d\n",
                        v->misc, vga_text_mode(v), vga_blanked(v));
    gemu_monitor_printf(mon, "seq[0-7]:");
    for (int i = 0; i < 8; i++) gemu_monitor_printf(mon, " %02x", v->seq[i]);
    gemu_monitor_printf(mon, "\ngc[0-15]:");
    for (int i = 0; i < 16; i++) gemu_monitor_printf(mon, " %02x", v->gc[i]);
    gemu_monitor_printf(mon, "\ncrtc[0-31]:");
    for (int i = 0; i < 32; i++) gemu_monitor_printf(mon, " %02x", v->crtc[i]);
    gemu_monitor_printf(mon, "\nattr[0-31]:");
    for (int i = 0; i < 32; i++) gemu_monitor_printf(mon, " %02x", v->attr[i]);
    gemu_monitor_printf(mon, "\n");
    gemu_monitor_printf(mon, "dac[0-15] (r,g,b):");
    for (int i = 0; i < 16; i++)
        gemu_monitor_printf(mon, " %02x%02x%02x", v->dac[i * 3],
                            v->dac[i * 3 + 1], v->dac[i * 3 + 2]);
    gemu_monitor_printf(mon, "\ndac write index=%u sub=%u mask=%02x\n",
                        v->dac_idx, v->dac_sub, v->pixel_mask);
    return GEMU_MON_NONE;
}

static GemuMonCmd cmd_vgapeek(GemuMonitor *mon, const char *args, void *ud) {
    const Vga *v = ud;
    unsigned plane, off, count;
    if (sscanf(args, "%u %x %u", &plane, &off, &count) == 3 && plane < 4 &&
        count <= 256 && off + count <= VGA_PLANE_SIZE) {
        gemu_monitor_printf(mon, "vgapeek plane=%u off=0x%x:", plane, off);
        for (unsigned i = 0; i < count; i++)
            gemu_monitor_printf(mon, " %02x", v->plane[plane][off + i]);
        gemu_monitor_printf(mon, "\n");
    } else {
        gemu_monitor_printf(mon, "usage: vgapeek <plane 0-3> <hex offset> <count>\n");
    }
    return GEMU_MON_NONE;
}

void vga_monitor_bind(GemuMonitor *mon, Vga *v) {
    gemu_monitor_bind(mon, "vgareg", cmd_vgareg, v);
    gemu_monitor_bind(mon, "vgapeek", cmd_vgapeek, v);
}
