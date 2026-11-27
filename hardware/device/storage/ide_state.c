/* Save state for the IDE models: see ide_state.h. */
#include "storage/ide_state.h"
#include "gemu/dev/state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define M(member) (ok &= GEMU_STATE_MEMBER(d, prefix, x, member))

/* AtaDisk, in order.  Excluded:
 *   ide   the IdeDevice ops (function pointers, set by ata_disk_init/rewire)
 *   cfg   AtaDiskConfig: the drive, the now() callback and its context,
 *         latency and debug - the machine's wiring and settings
 * Everything else is registered (data[] is inline: 8 KiB, saved whole). */
bool ata_disk_state_add(GemuDevice *d, const char *prefix, AtaDisk *x) {
    bool ok = true;
    M(error); M(features); M(count); M(lba_low); M(lba_mid); M(lba_high); M(device);
    M(status); M(control);
    M(multiple); M(xfer_cmd); M(block_sectors); M(xfer_lba); M(xfer_remaining);
    M(data_is_write); M(data_len); M(data_pos); M(data);
    M(irq_pending); M(irq_ready_at); M(irq_epoch);
    return ok;
}

/* ── the CD-ROM's response buffer ── */

static void cd_data_save(void *ud, GemuStateWriter *w) {
    AtapiCdrom *c = ud;
    uint64_t len = c->data ? (uint64_t)c->data_len : 0;
    gemu_state_write(w, &len, sizeof len);
    if (len)
        gemu_state_write(w, c->data, (size_t)len);
}
static bool cd_data_len(GemuStateReader *r, uint64_t *len) {
    if (!gemu_state_read(r, len, sizeof *len)) {
        gemu_state_refuse(r, "atapi data: truncated");
        return false;
    }
    if (*len > ATAPI_CDROM_STATE_MAX_DATA || *len > SIZE_MAX) {
        gemu_state_refuse(r, "atapi data: %llu bytes is too many", (unsigned long long)*len);
        return false;
    }
    if (gemu_state_left(r) != *len) {
        gemu_state_refuse(r, "atapi data: %llu bytes promised, %llu there",
                          (unsigned long long)*len, (unsigned long long)gemu_state_left(r));
        return false;
    }
    return true;
}
static bool cd_data_check(void *ud, GemuStateReader *r) {
    (void)ud;
    uint64_t len;
    return cd_data_len(r, &len);
}
static bool cd_data_load(void *ud, GemuStateReader *r) {
    AtapiCdrom *c = ud;
    uint64_t len;
    if (!cd_data_len(r, &len))
        return false;
    uint8_t *buf = NULL;
    if (len) {
        if (!(buf = malloc((size_t)len)) || !gemu_state_read(r, buf, (size_t)len)) {
            free(buf);
            return false;
        }
    }
    free(c->data);
    c->data = buf;
    c->data_len = (size_t)len;
    return true;
}

bool atapi_cdrom_state_add_data(GemuDevice *d, const char *name, AtapiCdrom *c) {
    return gemu_state_add_custom(d, name, cd_data_save, cd_data_check, cd_data_load, c);
}

/* AtapiCdrom, in order.  Excluded:
 *   ide       the IdeDevice ops (function pointers, set by init/rewire)
 *   cfg       AtapiCdromConfig: drive, now() and its context, latency,
 *             debug - the machine's wiring and settings
 *   data, data_len   a heap buffer and its length: one custom item, "data"
 *             (atapi_cdrom_state_add_data), which reallocates it on a load
 * Everything else is registered (data_pos and phase_end are size_t: 8
 * bytes on the 64-bit hosts, a different size refuses the load). */
bool atapi_cdrom_state_add(GemuDevice *d, const char *prefix, AtapiCdrom *x) {
    bool ok = true;
    M(error); M(features); M(count); M(lba_low); M(lba_mid); M(lba_high); M(device);
    M(status); M(control);
    M(packet); M(packet_pos); M(packet_command); M(byte_limit);
    char name[64];
    snprintf(name, sizeof name, "%sdata", prefix ? prefix : "");
    ok &= atapi_cdrom_state_add_data(d, name, x);
    M(data_pos); M(phase_end);
    M(status_pending); M(status_ready_at); M(ready_status); M(ready_irq);
    M(irq_pending); M(irq_ready_at); M(irq_epoch);
    return ok;
}

/* IdeChannel.  Excluded: dev[2] - pointers to the devices on the cable
 * (wiring; each device registers its own state). */
bool ide_channel_state_add(GemuDevice *d, const char *prefix, IdeChannel *x) {
    bool ok = true;
    M(device); M(control);
    return ok;
}

/* Cmd649.  Excluded: channel[2] - pointers to the channels it decodes
 * (wiring; each channel registers its own state). */
bool cmd649_state_add(GemuDevice *d, const char *prefix, Cmd649 *x) {
    bool ok = true;
    M(cfg);
    return ok;
}
