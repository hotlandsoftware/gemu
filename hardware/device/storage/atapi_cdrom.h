#pragma once
/*
 * An ATAPI CD-ROM drive on an IDE channel (ide.h), backed by a GemuDrive of
 * 2048-byte sectors whose medium can be absent (an empty tray).
 *
 * ATA commands: PACKET, IDENTIFY PACKET DEVICE and DEVICE RESET; anything
 * else aborts.  Packet commands: TEST UNIT READY, REQUEST SENSE, INQUIRY,
 * START STOP UNIT, PREVENT/ALLOW MEDIUM REMOVAL, READ CAPACITY, READ TOC (one
 * data track), READ(10) and READ(12); others fail with ILLEGAL REQUEST.
 * Without a medium the medium commands report NOT READY.  Data moves by PIO
 * in phases no longer than the host's byte limit, with an interrupt per
 * phase and one for command completion.
 *
 * Timing, in units of the `now` clock: PACKET and IDENTIFY PACKET keep the
 * drive busy (BSY) for `latency`, ended by atapi_cdrom_poll() once that has
 * passed; each interrupt becomes visible `latency` after it is raised.  With
 * latency 0 there is no busy interval and interrupts are immediate.
 *
 * The drive answers as device 0 only.  Selecting device 1 and back (a
 * probe of the absent slave) drops any interrupt and, on the way back,
 * restores the ATAPI signature, so a failed probe can't complete the
 * drive's next command early.
 */
#include "storage/ide.h"
#include <stddef.h>

struct GemuDrive;

typedef struct {
    struct GemuDrive *drive;
    uint64_t (*now)(void *ctx);
    void     *ctx;
    uint64_t  latency;
    bool      debug;            /* log commands and phases to stderr */
} AtapiCdromConfig;

typedef struct {
    IdeDevice        ide;       /* first: the channel's view */
    AtapiCdromConfig cfg;

    /* Task file */
    uint8_t  error, features, count, lba_low, lba_mid, lba_high, device, status;
    uint8_t  control;           /* device control: nIEN (2), SRST (4) */

    /* Command */
    uint8_t  packet[12];
    uint8_t  packet_pos;
    bool     packet_command;    /* the command in progress is PACKET */
    uint32_t byte_limit;        /* largest PIO phase, from the PACKET command */

    /* Data, host-owned memory: see atapi_cdrom_free() */
    uint8_t *data;
    size_t   data_len, data_pos, phase_end;

    /* BSY until status_ready_at, then ready_status (and an interrupt) */
    bool     status_pending;
    uint64_t status_ready_at;
    uint8_t  ready_status;
    bool     ready_irq;

    /* Interrupt */
    bool     irq_pending;
    uint64_t irq_ready_at;
    uint32_t irq_epoch;         /* bumped by each new interrupt and by nIEN */
} AtapiCdrom;

void atapi_cdrom_init(AtapiCdrom *d, const AtapiCdromConfig *cfg);
/* Hardware reset: the power-on state, keeping the wiring. */
void atapi_cdrom_reset(AtapiCdrom *d);
/* Restore the wiring after the struct was copied in from a saved state.
 * The data pointer is the caller's to restore. */
void atapi_cdrom_rewire(AtapiCdrom *d, const AtapiCdromConfig *cfg);
void atapi_cdrom_free(AtapiCdrom *d);
/* End a BSY interval whose time has come. */
void atapi_cdrom_poll(AtapiCdrom *d);
uint32_t atapi_cdrom_irq_epoch(const AtapiCdrom *d);
/* Drop a pending interrupt, as a Status read does, without the read. */
void atapi_cdrom_clear_irq(AtapiCdrom *d);
