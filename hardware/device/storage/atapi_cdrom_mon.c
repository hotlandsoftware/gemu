/* ATAPI CD-ROM: monitor commands (see atapi_cdrom.h).  Separate from the
 * device so that it links without the monitor. */
#include "storage/atapi_cdrom.h"
#include "gemu/monitor.h"
#include <inttypes.h>

void atapi_cdrom_monitor_print(GemuMonitor *mon, const AtapiCdrom *d) {
    gemu_monitor_printf(mon, "atapi: status=%#04x error=%#04x count=%#04x "
           "device=%#04x control=%#04x irq_pending=%u irq_epoch=%u "
           "irq_ready=%" PRIu64 " status_pending=%u "
           "status_ready=%" PRIu64 " ready_status=%#04x ready_irq=%u "
           "data=%" PRIu64 "/%" PRIu64 " phase_end=%" PRIu64 " now=%" PRIu64 "\n",
           d->status, d->error, d->count, d->device, d->control,
           d->irq_pending, d->irq_epoch, d->irq_ready_at, d->status_pending,
           d->status_ready_at, d->ready_status, d->ready_irq,
           (uint64_t)d->data_pos, (uint64_t)d->data_len, (uint64_t)d->phase_end,
           d->cfg.now ? d->cfg.now(d->cfg.ctx) : 0);
}

static GemuMonCmd cmd_state(GemuMonitor *mon, const char *args, void *ud) {
    (void)args;
    atapi_cdrom_monitor_print(mon, ud);
    return GEMU_MON_NONE;
}

void atapi_cdrom_monitor_bind(GemuMonitor *mon, const char *name, AtapiCdrom *d) {
    gemu_monitor_bind(mon, name, cmd_state, d);
}
