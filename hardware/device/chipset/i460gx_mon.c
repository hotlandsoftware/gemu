/* 460GX: monitor output (see i460gx.h).  Separate from the chipset so that
 * it links without the monitor. */
#include "chipset/i460gx.h"
#include "gemu/monitor.h"
#include <inttypes.h>

void i460gx_monitor_print_iosapic(GemuMonitor *mon, const I460GX *s) {
    for (unsigned i = 0; i < 64; i++) {
        uint64_t rte = s->iosapic_rte[i];
        if (rte == I460GX_IOSAPIC_RTE_MASK || rte == 0)
            continue;
        gemu_monitor_printf(mon, "gsi%-2u rte=%016" PRIX64 " vector=%02x mode=%u%s%s%s\n",
                            i, rte, (unsigned)(uint8_t)rte, (unsigned)((rte >> 8) & 7),
                            (rte & I460GX_IOSAPIC_RTE_MASK) ? " masked" : "",
                            (rte & (UINT64_C(1) << 15)) ? " level" : " edge",
                            (rte & (UINT64_C(1) << 13)) ? " low" : " high");
    }
}
