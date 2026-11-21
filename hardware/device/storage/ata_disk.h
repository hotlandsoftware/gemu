#pragma once
/*
 * A PIO-only LBA28 ATA-4 hard disk on an IDE channel (ide.h), backed by a
 * GemuDrive of 512-byte sectors.
 *
 * Commands: IDENTIFY DEVICE; READ/WRITE SECTORS and MULTIPLE; READ VERIFY;
 * SET MULTIPLE MODE (powers of two up to ATA_DISK_MAX_MULTIPLE, on at that
 * size after reset, as the firmware it was written for relies on); SET
 * FEATURES (PIO transfer modes; DMA modes abort since none is advertised);
 * EXECUTE DEVICE DIAGNOSTIC; CHECK POWER MODE; and the no-ops (NOP, SEEK,
 * RECALIBRATE, INITIALIZE DEVICE PARAMETERS, power, FLUSH CACHE).  Anything
 * else aborts, which is how drivers tell an ATA disk from an ATAPI device.
 * CHS addressing uses a fixed 16-head, 63-sector geometry.
 *
 * Timing: a command keeps the disk busy (status BSY, no interrupt) for
 * `latency` units of the `now` clock, then raises INTRQ, which stays up
 * until a Status read, a new command, or a software reset.  nIEN holds it
 * back without losing it.
 */
#include "storage/ide.h"

struct GemuDrive;

#define ATA_DISK_MAX_MULTIPLE 16u

typedef struct {
    struct GemuDrive *drive;
    uint64_t (*now)(void *ctx);
    void     *ctx;
    uint64_t  latency;          /* command to INTRQ, in now() units */
    bool      debug;            /* log commands to stderr */
} AtaDiskConfig;

typedef struct {
    IdeDevice     ide;          /* first: the channel's view */
    AtaDiskConfig cfg;

    /* Task file */
    uint8_t  error, features, count, lba_low, lba_mid, lba_high, device, status;
    uint8_t  control;           /* device control: nIEN (2), SRST (4) */

    /* Transfer in progress */
    uint8_t  multiple;          /* SET MULTIPLE block size, 0 = disabled */
    uint8_t  xfer_cmd;          /* data command in progress, 0 = none */
    uint8_t  block_sectors;     /* sectors in the current DRQ block */
    uint32_t xfer_lba;          /* first sector of the current block */
    uint32_t xfer_remaining;    /* sectors left, current block included */
    bool     data_is_write;
    uint32_t data_len, data_pos;
    uint8_t  data[ATA_DISK_MAX_MULTIPLE * 512];

    /* Interrupt */
    bool     irq_pending;
    uint64_t irq_ready_at;      /* now() at which BSY drops and INTRQ can rise */
    uint32_t irq_epoch;         /* bumped by each new interrupt and by nIEN */
} AtaDisk;

/* Wire and power on. */
void ata_disk_init(AtaDisk *d, const AtaDiskConfig *cfg);
/* Hardware reset: the power-on state, keeping the wiring. */
void ata_disk_reset(AtaDisk *d);
/* Restore the wiring (callbacks, drive) after the struct was copied in from
 * a saved state; the device state is kept. */
void ata_disk_rewire(AtaDisk *d, const AtaDiskConfig *cfg);

/* Still working on the last command: status BSY, no interrupt yet. */
bool ata_disk_busy(const AtaDisk *d);
/* Changes whenever the disk starts a new interrupt or nIEN masks one: a
 * host that remembers the epoch it delivered can tell a fresh edge. */
uint32_t ata_disk_irq_epoch(const AtaDisk *d);
/* Drop a pending interrupt, as a Status read does, without the read. */
void ata_disk_clear_irq(AtaDisk *d);
