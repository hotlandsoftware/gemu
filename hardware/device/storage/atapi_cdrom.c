/* ATAPI CD-ROM: see atapi_cdrom.h. */
#include "storage/atapi_cdrom.h"
#include "gemu/drive.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static AtapiCdrom *cd(IdeDevice *d) { return (AtapiCdrom *)d; }
static const AtapiCdrom *ccd(const IdeDevice *d) { return (const AtapiCdrom *)d; }

static uint64_t now(const AtapiCdrom *d) { return d->cfg.now(d->cfg.ctx); }

static const char *packet_name(uint8_t opcode) {
    switch (opcode) {
    case 0x00: return "TEST UNIT READY";
    case 0x03: return "REQUEST SENSE";
    case 0x12: return "INQUIRY";
    case 0x1B: return "START STOP UNIT";
    case 0x1E: return "PREVENT/ALLOW MEDIUM REMOVAL";
    case 0x25: return "READ CAPACITY";
    case 0x28: return "READ(10)";
    case 0x43: return "READ TOC";
    case 0xA8: return "READ(12)";
    default:   return "unknown";
    }
}

/* INTRQ stays latched until a normal Status read. */
void atapi_cdrom_clear_irq(AtapiCdrom *d) {
    d->irq_pending = false;
}

uint32_t atapi_cdrom_irq_epoch(const AtapiCdrom *d) {
    return d->irq_epoch;
}

/* Latch an interrupt, visible `latency` from now. */
static void raise_irq(AtapiCdrom *d) {
    d->irq_pending = true;
    d->irq_epoch++;
    d->irq_ready_at = now(d) + d->cfg.latency;
}

static void defer_ready(AtapiCdrom *d, bool irq) {
    d->ready_status = d->status;
    d->status = 0x80;                           /* BSY */
    d->status_pending = true;
    d->ready_irq = irq;
    d->status_ready_at = now(d) + d->cfg.latency;
    /* The completion interrupt belongs to the ready transition, not to the
     * command-register write while the drive is still busy. */
    atapi_cdrom_clear_irq(d);
    if (!d->cfg.latency)
        atapi_cdrom_poll(d);                    /* no busy interval at all */
}

void atapi_cdrom_poll(AtapiCdrom *d) {
    if (!d->status_pending || now(d) < d->status_ready_at)
        return;
    d->status_pending = false;
    d->status = d->ready_status;
    if (d->ready_irq) {
        d->irq_pending = true;
        d->irq_epoch++;
        d->irq_ready_at = now(d);
    }
    if (d->cfg.debug)
        fprintf(stderr, "atapi: BSY cleared status=%#04x reason=%#04x now=%"
                PRIu64 "\n", d->status, d->count, now(d));
}

static void free_data(AtapiCdrom *d) {
    free(d->data);
    d->data = NULL;
    d->data_len = d->data_pos = 0;
    d->phase_end = 0;
}

void atapi_cdrom_free(AtapiCdrom *d) {
    free_data(d);
}

static void begin_data_phase(AtapiCdrom *d) {
    size_t remaining = d->data_len - d->data_pos;
    size_t phase = remaining;
    uint32_t limit = d->byte_limit ? d->byte_limit : 65536u;
    if (phase > limit)
        phase = limit;
    /* ATAPI PIO byte counts are even except for a final odd byte. */
    if (phase > 1 && (phase & 1) && phase < remaining)
        phase--;
    d->phase_end = d->data_pos + phase;
    d->count = 0x02;                /* data phase: CoD=0, I/O=device-to-host */
    d->lba_mid = (uint8_t)phase;
    d->lba_high = (uint8_t)(phase >> 8);
    d->status = 0x48;               /* DRDY|DRQ */
    raise_irq(d);
}

/* Take ownership of a malloc'd response (NULL, 0 for none) and start its
 * data phase, or complete the command. */
static void set_data(AtapiCdrom *d, uint8_t *data, size_t len) {
    free_data(d);
    if (len) {
        d->data = data;
        d->data_len = len;
        begin_data_phase(d);
    } else {
        free(data);
        d->count = 0x03;            /* command complete: CoD=1, I/O=1 */
        d->lba_mid = d->lba_high = 0;
        d->status = 0x40;
        raise_irq(d);
    }
    if (d->cfg.debug)
        fprintf(stderr, "atapi: response bytes=%zu status=%#04x reason=%#04x\n",
                len, d->status, d->count);
}

/* A copy of a reply on the stack, for set_data(). */
static void set_reply(AtapiCdrom *d, const void *reply, size_t len) {
    uint8_t *buf = len ? malloc(len) : NULL;
    if (len && !buf) {
        d->error = 0x04;
        d->status = 0x41;
        return;
    }
    if (len) memcpy(buf, reply, len);
    set_data(d, buf, len);
}

/* The command failed: sense key in the error register's high nibble. */
static void fail(AtapiCdrom *d, uint8_t error) {
    free_data(d);
    d->count = 0x03;                /* command completion, I/O to host */
    d->error = error;
    d->status = 0x41;               /* DRDY|ERR */
    raise_irq(d);
    if (d->cfg.debug)
        fprintf(stderr, "atapi: failed status=%#04x error=%#04x\n",
                d->status, d->error);
}

static void reply(AtapiCdrom *d) {
    const uint8_t *p = d->packet;
    uint8_t r[64] = {0};
    uint32_t blocks = (uint32_t)(gemu_drive_size(d->cfg.drive) / 2048);
    bool medium = gemu_drive_inserted(d->cfg.drive);
    if (d->cfg.debug) {
        unsigned limit = (unsigned)d->lba_mid | ((unsigned)d->lba_high << 8);
        fprintf(stderr, "atapi: packet %-28s opcode=%#04x host-byte-limit=%u bytes=",
                packet_name(p[0]), p[0], limit ? limit : 65536u);
        for (unsigned i = 0; i < sizeof(d->packet); i++)
            fprintf(stderr, "%s%02x", i ? " " : "", p[i]);
        fputc('\n', stderr);
    }
    switch (p[0]) {
    case 0x00:                                  /* TEST UNIT READY */
        if (!medium) { fail(d, 0x20); break; }  /* NOT READY */
        set_data(d, NULL, 0);
        break;
    case 0x1B:                                  /* START STOP UNIT */
    case 0x1E:                                  /* PREVENT/ALLOW MEDIUM REMOVAL */
        set_data(d, NULL, 0);
        break;
    case 0x03:                                  /* REQUEST SENSE */
        r[0] = 0x70;
        if (!medium) {
            r[2] = 0x02;                        /* NOT READY */
            r[12] = 0x3a;                       /* medium not present */
        }
        r[7] = 10;
        set_reply(d, r, p[4] < 18 ? p[4] : 18);
        break;
    case 0x12: {                                /* INQUIRY */
        r[0] = 0x05; r[1] = 0x80; r[2] = 0x00;
        r[3] = 0x21; r[4] = 31;
        memcpy(r + 8, "GEMU    ", 8);
        memcpy(r + 16, "ATAPI CD-ROM    ", 16);
        memcpy(r + 32, "1.0 ", 4);
        set_reply(d, r, p[4] < 36 ? p[4] : 36);
        break;
    }
    case 0x25:                                  /* READ CAPACITY */
        if (!medium) { fail(d, 0x20); break; }
        if (blocks) blocks--;
        r[0] = blocks >> 24; r[1] = blocks >> 16;
        r[2] = blocks >> 8;  r[3] = blocks;
        r[6] = 0x08;
        set_reply(d, r, 8);
        break;
    case 0x43: {                                /* READ TOC: one data track, lead-out */
        if (!medium) { fail(d, 0x20); break; }
        r[1] = 0x12; r[2] = 1; r[3] = 1;
        r[5] = 0x14; r[6] = 1;
        r[13] = 0x14; r[14] = 0xAA;
        r[16] = blocks >> 24; r[17] = blocks >> 16;
        r[18] = blocks >> 8; r[19] = blocks;
        size_t alloc = ((size_t)p[7] << 8) | p[8];
        set_reply(d, r, alloc < 20 ? alloc : 20);
        break;
    }
    case 0x28:                                  /* READ(10) */
    case 0xA8: {                                /* READ(12) */
        uint32_t lba = ((uint32_t)p[2] << 24) | ((uint32_t)p[3] << 16) |
                       ((uint32_t)p[4] << 8) | p[5];
        uint32_t count = p[0] == 0x28 ? ((uint32_t)p[7] << 8) | p[8] :
                         ((uint32_t)p[6] << 24) | ((uint32_t)p[7] << 16) |
                         ((uint32_t)p[8] << 8) | p[9];
        size_t len = (size_t)count * 2048;
        if (d->cfg.debug)
            fprintf(stderr, "atapi: %s lba=%" PRIu32 " sectors=%" PRIu32
                    " bytes=%zu image-blocks=%" PRIu32 "\n",
                    packet_name(p[0]), lba, count, len, blocks);
        uint8_t *buf = len ? malloc(len) : NULL;
        if ((len && !buf) || lba >= blocks || count > blocks - lba ||
            (len && !gemu_drive_read(d->cfg.drive, (uint64_t)lba * 2048, buf, len))) {
            free(buf);
            d->error = 0x50;                    /* ILLEGAL REQUEST */
            d->status = 0x41;
            d->count = 0x03;
            raise_irq(d);
            if (d->cfg.debug)
                fprintf(stderr, "atapi: read failed lba=%" PRIu32 " sectors=%"
                        PRIu32 "\n", lba, count);
        } else {
            set_data(d, buf, len);
        }
        break;
    }
    default:
        fprintf(stderr, "atapi: unsupported packet command %02X\n", p[0]);
        d->error = 0x50;
        d->status = 0x41;
        d->count = 0x03;
        raise_irq(d);
        break;
    }
}

static uint64_t data_read(IdeDevice *dev, unsigned size) {
    AtapiCdrom *d = cd(dev);
    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++)
        if (d->data_pos < d->data_len && d->data_pos < d->phase_end)
            v |= (uint64_t)d->data[d->data_pos++] << (i * 8);
    if (d->data_pos >= d->phase_end && (d->status & 0x08)) {
        if (d->data_pos < d->data_len) {
            begin_data_phase(d);
            if (d->cfg.debug)
                fprintf(stderr, "atapi: next PIO phase pos=%zu end=%zu total=%zu\n",
                        d->data_pos, d->phase_end, d->data_len);
        } else {
            d->status = 0x40;
            /* PACKET has a separate command-complete interrupt.  IDENTIFY
             * PACKET is an ATA PIO data-in command: its only interrupt
             * announced the 512-byte response. */
            if (d->packet_command) {
                d->count = 0x03;            /* CoD=1, I/O=1 */
                d->lba_mid = d->lba_high = 0;
                raise_irq(d);
            }
            if (d->cfg.debug)
                fprintf(stderr, "atapi: PIO complete bytes=%zu status=%#04x "
                        "reason=%#04x\n", d->data_len, d->status, d->count);
        }
    }
    return v;
}

static void data_write(IdeDevice *dev, uint64_t val, unsigned size) {
    AtapiCdrom *d = cd(dev);
    unsigned old = d->packet_pos;
    for (unsigned i = 0; i < size && d->packet_pos < 12; i++)
        d->packet[d->packet_pos++] = (uint8_t)(val >> (i * 8));
    /* The SDV's legacy EFI thunk checks the data phase synchronously after
     * writing the last PACKET word: PACKET itself asserts BSY (AtaPim waits
     * for that edge), but another BSY interval here makes the thunk abandon
     * PIO before its data read.  reply() exposes DRQ and schedules the
     * data-ready interrupt. */
    if (old < 12 && d->packet_pos == 12)
        reply(d);
}

static uint8_t reg_read(IdeDevice *dev, unsigned reg, bool ack) {
    AtapiCdrom *d = cd(dev);
    switch (reg) {
    case 1: return d->error;
    case 2: return d->count;
    case 3: return d->lba_low;
    case 4: return d->lba_mid;
    case 5: return d->lba_high;
    case 6: return d->device;
    case 7:
        if (ack)
            atapi_cdrom_clear_irq(d);   /* Alternate Status doesn't acknowledge */
        return d->status;
    default:
        return 0;
    }
}

static void signature(AtapiCdrom *d) {
    d->status = 0x40;
    d->count = d->lba_low = 1;
    d->lba_mid = 0x14;
    d->lba_high = 0xEB;
}

static void reg_write(IdeDevice *dev, unsigned reg, uint8_t val) {
    AtapiCdrom *d = cd(dev);
    switch (reg) {
    case 1: d->features = val; break;
    case 2: d->count = val; break;
    case 3: d->lba_low = val; break;
    case 4: d->lba_mid = val; break;
    case 5: d->lba_high = val; break;
    case 6: {
        bool was_slave = (d->device & 0x10) != 0;
        d->device = val;
        bool is_slave = (val & 0x10) != 0;
        if (is_slave != was_slave) {
            atapi_cdrom_clear_irq(d);
            d->status_pending = false;
            if (!is_slave) {
                d->error = 0;
                signature(d);
            }
        }
        break;
    }
    }
}

static void identify_packet(AtapiCdrom *d) {
    uint8_t id[512] = {0};
    uint8_t count = d->count, low = d->lba_low, mid = d->lba_mid, high = d->lba_high;
    id[0] = 0xC0; id[1] = 0x85;                 /* removable ATAPI CD-ROM */
    id[98] = 0x00; id[99] = 0x02;               /* LBA supported */
    char model[40];
    memset(model, ' ', sizeof(model));
    memcpy(model, "GEMU ATAPI CD-ROM", 17);
    for (unsigned i = 0; i < 40; i += 2) {
        id[54 + i] = model[i + 1]; id[55 + i] = model[i];
    }
    d->byte_limit = sizeof(id);
    set_reply(d, id, sizeof(id));
    /* IDENTIFY PACKET is an ATA PIO data-in command, not a PACKET data
     * phase: the sector count isn't an interrupt reason and the cylinder
     * registers aren't a byte count.  Keep what the host had in them. */
    d->count = count;
    d->lba_low = low;
    d->lba_mid = mid;
    d->lba_high = high;
    defer_ready(d, true);
}

static void command(IdeDevice *dev, uint8_t cmd) {
    AtapiCdrom *d = cd(dev);
    /* A new command supersedes any unacknowledged completion of the last. */
    atapi_cdrom_clear_irq(d);
    d->status_pending = false;
    d->error = 0;
    d->packet_command = cmd == 0xA0;
    if (d->cfg.debug)
        fprintf(stderr, "atapi: ATA command=%#04x device=%#04x features=%#04x "
                "byte-limit=%u now=%" PRIu64 "\n", cmd, d->device, d->features,
                (unsigned)d->lba_mid | ((unsigned)d->lba_high << 8), now(d));
    switch (cmd) {
    case 0xA0: {                                /* PACKET */
        unsigned limit = (unsigned)d->lba_mid | ((unsigned)d->lba_high << 8);
        d->byte_limit = limit ? limit : 65536u;
        d->packet_pos = 0;
        memset(d->packet, 0, sizeof(d->packet));
        d->count = 0x01;
        d->status = 0x48;
        defer_ready(d, false);
        break;
    }
    case 0xA1:                                  /* IDENTIFY PACKET DEVICE */
        identify_packet(d);
        break;
    case 0x08:                                  /* DEVICE RESET */
        d->status = 0x40;
        d->count = 1;
        d->lba_mid = 0x14;
        d->lba_high = 0xEB;
        break;
    default:
        d->error = 0x04;                        /* ABRT */
        d->status = 0x41;
        raise_irq(d);
        if (d->cfg.debug)
            fprintf(stderr, "atapi: unsupported ATA command=%#04x\n", cmd);
        break;
    }
}

/* Device control: nIEN (2) holds INTRQ back; SRST (4) resets on its
 * transitions, leaving device 0 selected. */
static void control(IdeDevice *dev, uint8_t val) {
    AtapiCdrom *d = cd(dev);
    if (d->cfg.debug)
        fprintf(stderr, "atapi: device-control=%#04x (%s reset)\n", val,
                (val & 4) ? "assert" : "deassert");
    uint8_t old = d->control;
    d->control = val;
    if (val & 2)
        d->irq_epoch++;                         /* nIEN lowers the output */
    if ((old ^ val) & 4) {
        atapi_cdrom_clear_irq(d);
        d->status_pending = false;
        d->packet_pos = 0;
        d->packet_command = false;
        d->data_pos = d->data_len;
        if (val & 4) {
            d->status = 0x80;
        } else {
            signature(d);
            d->device &= (uint8_t)~0x10;
        }
    }
}

static bool intrq(const IdeDevice *dev) {
    const AtapiCdrom *d = ccd(dev);
    return d->irq_pending && !(d->control & 6) && now(d) >= d->irq_ready_at;
}

void atapi_cdrom_rewire(AtapiCdrom *d, const AtapiCdromConfig *cfg) {
    d->cfg = *cfg;
    d->ide = (IdeDevice){
        .data_read = data_read, .data_write = data_write,
        .reg_read = reg_read, .reg_write = reg_write,
        .command = command, .control = control, .intrq = intrq,
    };
}

void atapi_cdrom_reset(AtapiCdrom *d) {
    d->error = d->features = 0;
    d->count = 1;
    d->lba_low = 1;
    d->lba_mid = 0x14;
    d->lba_high = 0xEB;
    d->device = 0xA0;
    /* The drive is there even with its tray empty: the medium affects
     * packet results, not whether the device answers. */
    d->status = 0x40;
    d->control = 0;
    d->packet_pos = 0;
    d->packet_command = false;
    d->byte_limit = 65536u;
    free_data(d);
    d->status_pending = false;
    d->ready_irq = false;
    d->irq_pending = false;
    d->irq_epoch++;
}

void atapi_cdrom_init(AtapiCdrom *d, const AtapiCdromConfig *cfg) {
    memset(d, 0, sizeof *d);
    atapi_cdrom_rewire(d, cfg);
    atapi_cdrom_reset(d);
}
