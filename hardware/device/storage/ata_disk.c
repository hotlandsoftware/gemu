/* ATA hard disk: see ata_disk.h. */
#include "storage/ata_disk.h"
#include "gemu/drive.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define ATA_HEADS 16u
#define ATA_SPT   63u

static AtaDisk *disk(IdeDevice *d) { return (AtaDisk *)d; }
static const AtaDisk *cdisk(const IdeDevice *d) { return (const AtaDisk *)d; }

static uint64_t now(const AtaDisk *d) { return d->cfg.now(d->cfg.ctx); }

static void set_data(AtaDisk *d, const void *data, size_t len) {
    if (len > sizeof(d->data)) len = sizeof(d->data);
    d->data_len = (uint32_t)len;
    d->data_pos = 0;
    d->data_is_write = false;
    if (len) {
        if (!data) memset(d->data, 0, len);
        else if (data != d->data) memcpy(d->data, data, len);
    }
    d->status = len ? 0x58 : 0x50;      /* DRDY|DSC|DRQ or DRDY|DSC */
}

static uint32_t total_sectors(const AtaDisk *d) {
    uint64_t n = gemu_drive_size(d->cfg.drive) / 512;
    return n > 0x0FFFFFFFu ? 0x0FFFFFFFu : (uint32_t)n;   /* LBA28 */
}

/* The address: 28-bit LBA when the device register's LBA bit (0x40) is set,
 * otherwise CHS in the fixed IDENTIFY geometry. */
static bool address(const AtaDisk *d, uint32_t *lba) {
    if (d->device & 0x40) {
        *lba = (uint32_t)d->lba_low | ((uint32_t)d->lba_mid << 8) |
               ((uint32_t)d->lba_high << 16) | ((uint32_t)(d->device & 0x0F) << 24);
        return true;
    }
    uint32_t cyl = (uint32_t)d->lba_mid | ((uint32_t)d->lba_high << 8);
    uint32_t head = d->device & 0x0F, sector = d->lba_low;
    if (!sector || sector > ATA_SPT || head >= ATA_HEADS)
        return false;
    *lba = (cyl * ATA_HEADS + head) * ATA_SPT + sector - 1;
    return true;
}

/* Raise INTRQ after the response latency. */
static void raise_irq(AtaDisk *d) {
    d->irq_pending = true;
    d->irq_epoch++;
    d->irq_ready_at = now(d) + d->cfg.latency;
}

void ata_disk_clear_irq(AtaDisk *d) {
    d->irq_pending = false;
}

bool ata_disk_busy(const AtaDisk *d) {
    return d->irq_pending && now(d) < d->irq_ready_at;
}

uint32_t ata_disk_irq_epoch(const AtaDisk *d) {
    return d->irq_epoch;
}

/* End a command: final status/error, no data phase, completion interrupt. */
static void finish(AtaDisk *d, uint8_t status, uint8_t error) {
    set_data(d, NULL, 0);
    d->status = status;
    d->error = error;
    d->xfer_cmd = 0;
    d->xfer_remaining = 0;
    raise_irq(d);
}

static void abort_cmd(AtaDisk *d) { finish(d, 0x51, 0x04); }

static unsigned block_size(const AtaDisk *d) {
    unsigned n = (d->xfer_cmd == 0xC4 || d->xfer_cmd == 0xC5) ? d->multiple : 1;
    return n < d->xfer_remaining ? n : d->xfer_remaining;
}

/* PIO data-in: present the next block with DRQ and interrupt for it. */
static void read_block(AtaDisk *d) {
    unsigned n = block_size(d);
    size_t len = (size_t)n * 512;
    if (!gemu_drive_read(d->cfg.drive, (uint64_t)d->xfer_lba * 512, d->data, len)) {
        finish(d, 0x51, 0x40);              /* ERR, UNC */
        return;
    }
    set_data(d, d->data, len);
    d->block_sectors = (uint8_t)n;
    raise_irq(d);
}

/* PIO data-out: ask for the next block.  The first block of a command is
 * requested without an interrupt; later ones interrupt (ATA PIO out). */
static void write_block(AtaDisk *d, bool irq) {
    unsigned n = block_size(d);
    set_data(d, NULL, (size_t)n * 512);
    d->data_is_write = true;
    d->block_sectors = (uint8_t)n;
    if (irq)
        raise_irq(d);
}

/* The host has read the whole current data-in block. */
static void data_in_done(AtaDisk *d) {
    if (d->cfg.debug)
        fprintf(stderr, "ata: data-in block read lba=%u sectors=%u\n",
                d->xfer_lba, d->block_sectors);
    if (d->xfer_cmd == 0x20 || d->xfer_cmd == 0x21 || d->xfer_cmd == 0xC4) {
        d->xfer_lba += d->block_sectors;
        d->xfer_remaining -= d->block_sectors;
        if (d->xfer_remaining) {
            read_block(d);
            return;
        }
    }
    set_data(d, NULL, 0);                   /* no interrupt at the end */
    d->xfer_cmd = 0;
}

/* The host has written the whole current data-out block. */
static void data_out_done(AtaDisk *d) {
    if (!gemu_drive_write(d->cfg.drive, (uint64_t)d->xfer_lba * 512,
                          d->data, d->data_len)) {
        finish(d, 0x51, 0x10);              /* ERR, IDNF */
        return;
    }
    d->xfer_lba += d->block_sectors;
    d->xfer_remaining -= d->block_sectors;
    if (d->xfer_remaining)
        write_block(d, true);
    else
        finish(d, 0x50, 0);
}

/* A byte-swapped ASCII field, as ATA IDENTIFY strings are packed: each
 * 16-bit word holds two characters swapped from reading order. */
static void id_string(uint8_t *id, unsigned byte_off, unsigned len, const char *text) {
    char buf[40];
    size_t tlen = strlen(text);
    memset(buf, ' ', sizeof(buf));
    memcpy(buf, text, tlen < sizeof(buf) ? tlen : sizeof(buf));
    for (unsigned i = 0; i < len; i += 2) {
        id[byte_off + i]     = (uint8_t)buf[i + 1];
        id[byte_off + i + 1] = (uint8_t)buf[i];
    }
}

static void id_word(uint8_t *id, unsigned word, uint16_t v) {
    id[word * 2] = (uint8_t)v;
    id[word * 2 + 1] = (uint8_t)(v >> 8);
}

/* No DMA is advertised (words 49 bit 8, 63 and 88 clear): there's no
 * bus-master engine behind the disk, so drivers stay on PIO. */
static void identify(AtaDisk *d) {
    uint8_t id[512] = {0};
    uint32_t sectors = total_sectors(d);
    uint32_t cyls = sectors / (ATA_HEADS * ATA_SPT);
    if (cyls > 16383) cyls = 16383;
    uint32_t chs_sectors = cyls * ATA_HEADS * ATA_SPT;

    id_word(id, 0, 0x0040);                     /* fixed device */
    id_word(id, 1, (uint16_t)cyls);
    id_word(id, 3, ATA_HEADS);
    id_word(id, 6, ATA_SPT);
    id_string(id, 20, 20, "GEMU-HDA0001");      /* words 10-19: serial */
    id_string(id, 46, 8, "1.0");                /* words 23-26: firmware */
    id_string(id, 54, 40, "GEMU VIRTUAL HDD");  /* words 27-46: model */
    id_word(id, 47, 0x8000 | ATA_DISK_MAX_MULTIPLE);
    id_word(id, 49, 0x0200);                    /* LBA */
    id_word(id, 51, 0x0200);                    /* PIO timing mode 2 */
    id_word(id, 53, 0x0003);                    /* words 54-58, 64-70 valid */
    id_word(id, 54, (uint16_t)cyls);
    id_word(id, 55, ATA_HEADS);
    id_word(id, 56, ATA_SPT);
    id_word(id, 57, (uint16_t)chs_sectors);
    id_word(id, 58, (uint16_t)(chs_sectors >> 16));
    if (d->multiple)
        id_word(id, 59, 0x0100 | d->multiple);
    id_word(id, 60, (uint16_t)sectors);
    id_word(id, 61, (uint16_t)(sectors >> 16));
    id_word(id, 64, 0x0003);                    /* PIO modes 3 and 4 */
    for (unsigned w = 65; w <= 68; w++)
        id_word(id, w, 120);                    /* cycle times, ns */
    id_word(id, 80, 0x001E);                    /* ATA-1..ATA-4 */
    id_word(id, 82, 0x4000);                    /* NOP */
    id_word(id, 83, 0x4000);                    /* (word valid) */
    id_word(id, 84, 0x4000);
    id_word(id, 85, 0x4000);
    id_word(id, 86, 0x0000);
    id_word(id, 87, 0x4000);
    set_data(d, id, sizeof(id));
    d->xfer_cmd = 0xEC;
    raise_irq(d);
}

/* Start a READ/WRITE (MULTIPLE) or VERIFY over the task-file address and
 * count; false (after aborting) if the range is invalid. */
static bool start_xfer(AtaDisk *d, uint8_t cmd) {
    uint32_t lba;
    uint32_t count = d->count ? d->count : 256;
    if (!address(d, &lba) || (uint64_t)lba + count > total_sectors(d)) {
        finish(d, 0x51, 0x10);              /* ERR, IDNF */
        return false;
    }
    d->xfer_cmd = cmd;
    d->xfer_lba = lba;
    d->xfer_remaining = count;
    return true;
}

static void command(IdeDevice *dev, uint8_t cmd) {
    AtaDisk *d = disk(dev);
    if (d->cfg.debug) {
        fprintf(stderr, "ata: command %#04x device=%#04x lba=%u:%u:%u "
                "count=%u features=%#04x now=%" PRIu64 "\n", cmd, d->device,
                d->lba_high, d->lba_mid, d->lba_low, d->count, d->features, now(d));
        if (d->data_len && d->data_pos < d->data_len)
            fprintf(stderr, "ata: command %#04x abandons %u unread data bytes\n",
                    cmd, d->data_len - d->data_pos);
    }
    d->error = 0;
    ata_disk_clear_irq(d);
    switch (cmd) {
    case 0xEC:                                       /* IDENTIFY DEVICE */
        identify(d);
        break;
    case 0x20: case 0x21:                            /* READ SECTORS */
    case 0xC4:                                       /* READ MULTIPLE */
        if (cmd == 0xC4 && !d->multiple) { abort_cmd(d); break; }
        if (start_xfer(d, cmd))
            read_block(d);
        break;
    case 0x30: case 0x31:                            /* WRITE SECTORS */
    case 0xC5:                                       /* WRITE MULTIPLE */
        if (cmd == 0xC5 && !d->multiple) { abort_cmd(d); break; }
        if (start_xfer(d, cmd))
            write_block(d, false);
        break;
    case 0x40: case 0x41:                            /* READ VERIFY SECTORS */
        if (start_xfer(d, cmd))
            finish(d, 0x50, 0);
        break;
    case 0xC6: {                                     /* SET MULTIPLE MODE */
        unsigned n = d->count;
        if (n > ATA_DISK_MAX_MULTIPLE || (n & (n - 1))) { abort_cmd(d); break; }
        d->multiple = (uint8_t)n;
        finish(d, 0x50, 0);
        break;
    }
    case 0xEF:                                       /* SET FEATURES */
        /* Transfer mode 0x03: PIO (count 0x00-0x0F) only; DMA modes abort
         * since no DMA was advertised.  Other features are no-ops. */
        if (d->features == 0x03 && d->count >= 0x10) { abort_cmd(d); break; }
        finish(d, 0x50, 0);
        break;
    case 0x90:                                       /* EXECUTE DIAGNOSTIC */
        finish(d, 0x50, 0x01);                      /* device 0 passed */
        d->count = d->lba_low = 1;
        d->lba_mid = d->lba_high = 0;
        break;
    case 0xE5:                                       /* CHECK POWER MODE */
        d->count = 0xFF;                            /* active/idle */
        finish(d, 0x50, 0);
        break;
    case 0x91:                       /* INITIALIZE DEVICE PARAMETERS */
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
    case 0x16: case 0x17: case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x1C: case 0x1D: case 0x1E: case 0x1F:     /* RECALIBRATE */
    case 0x70:                                       /* SEEK */
    case 0x00:                                       /* NOP */
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE6:  /* power */
    case 0xE7: case 0xEA:                            /* FLUSH CACHE (EXT) */
        finish(d, 0x50, 0);
        break;
    default:
        /* Includes IDENTIFY PACKET DEVICE (A1) and PACKET (A0). */
        if (d->cfg.debug)
            fprintf(stderr, "ata: unsupported command %#04x\n", cmd);
        abort_cmd(d);
        break;
    }
}

static uint64_t data_read(IdeDevice *dev, unsigned size) {
    AtaDisk *d = disk(dev);
    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++)
        if (d->data_pos < d->data_len)
            v |= (uint64_t)d->data[d->data_pos++] << (i * 8);
    if (!d->data_is_write && d->data_len && d->data_pos >= d->data_len &&
        (d->status & 0x08))
        data_in_done(d);
    return v;
}

static void data_write(IdeDevice *dev, uint64_t val, unsigned size) {
    AtaDisk *d = disk(dev);
    for (unsigned i = 0; i < size; i++)
        if (d->data_pos < d->data_len)
            d->data[d->data_pos++] = (uint8_t)(val >> (i * 8));
    if (d->data_is_write && d->data_len && d->data_pos >= d->data_len)
        data_out_done(d);
}

static uint8_t reg_read(IdeDevice *dev, unsigned reg, bool ack) {
    AtaDisk *d = disk(dev);
    switch (reg) {
    case 1: return d->error;
    case 2: return d->count;
    case 3: return d->lba_low;
    case 4: return d->lba_mid;
    case 5: return d->lba_high;
    case 6: return d->device;
    case 7:
        if (ata_disk_busy(d))
            return 0x80;                    /* BSY: no DRQ/interrupt yet */
        if (ack)
            ata_disk_clear_irq(d);          /* a normal Status read acknowledges */
        return d->status;
    default:
        return 0;
    }
}

static void reg_write(IdeDevice *dev, unsigned reg, uint8_t val) {
    AtaDisk *d = disk(dev);
    switch (reg) {
    case 1: d->features = val; break;
    case 2: d->count = val; break;
    case 3: d->lba_low = val; break;
    case 4: d->lba_mid = val; break;
    case 5: d->lba_high = val; break;
    case 6: d->device = val; break;
    }
}

/* Device control: nIEN (2) holds INTRQ back; SRST (4) resets on its
 * transitions only, so rewriting nIEN doesn't reset mid-command. */
static void control(IdeDevice *dev, uint8_t val) {
    AtaDisk *d = disk(dev);
    if (d->cfg.debug)
        fprintf(stderr, "ata: device control <- %#04x now=%" PRIu64 "\n", val, now(d));
    uint8_t old = d->control;
    d->control = val;
    if (val & 2)
        d->irq_epoch++;                     /* nIEN lowers INTRQ */
    if ((old ^ val) & 4) {
        ata_disk_clear_irq(d);
        set_data(d, NULL, 0);
        d->xfer_cmd = 0;
        d->xfer_remaining = 0;
        if (val & 4) {
            d->status = 0x80;
        } else {                            /* the ATA signature */
            d->status = 0x50;
            d->error = 0x01;
            d->count = 1;
            d->lba_low = 1;
            d->lba_mid = d->lba_high = 0;
            d->device = 0;
        }
    }
}

static bool intrq(const IdeDevice *dev) {
    const AtaDisk *d = cdisk(dev);
    return d->irq_pending && !ata_disk_busy(d) && !(d->control & 6);
}

void ata_disk_rewire(AtaDisk *d, const AtaDiskConfig *cfg) {
    d->cfg = *cfg;
    d->ide = (IdeDevice){
        .data_read = data_read, .data_write = data_write,
        .reg_read = reg_read, .reg_write = reg_write,
        .command = command, .control = control, .intrq = intrq,
    };
}

void ata_disk_reset(AtaDisk *d) {
    d->error = d->features = 0;
    d->count = 1;
    d->lba_low = 1;
    d->lba_mid = d->lba_high = 0;
    d->device = 0;
    d->status = 0x50;
    d->control = 0;
    /* Multiple mode is on at power-up (IDENTIFY word 59), as on typical
     * drives: the i2000 firmware's disk driver issues READ MULTIPLE without
     * a SET MULTIPLE first. */
    d->multiple = ATA_DISK_MAX_MULTIPLE;
    d->xfer_cmd = 0;
    d->xfer_remaining = 0;
    d->block_sectors = 0;
    d->xfer_lba = 0;
    d->data_is_write = false;
    d->data_len = d->data_pos = 0;
    d->irq_pending = false;
    d->irq_ready_at = 0;
    d->irq_epoch++;
}

void ata_disk_init(AtaDisk *d, const AtaDiskConfig *cfg) {
    memset(d, 0, sizeof *d);
    ata_disk_rewire(d, cfg);
    ata_disk_reset(d);
}
