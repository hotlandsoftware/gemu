#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Block devices: QEMU's -drive and its legacy shorthands.
 *
 *   -drive [file=PATH][,if=TYPE][,bus=N][,unit=N][,index=N][,media=disk|cdrom]
 *          [,readonly=on|off][,snapshot=on|off][,format=raw][,id=NAME]...
 *
 * Same option syntax and semantics as QEMU: commas inside a value are
 * written ",,"; index= maps to bus/unit (two units per IDE bus); without
 * bus/unit/index a drive takes the next free unit on its interface; if= and
 * media= default to the machine's default interface and disk; a removable
 * drive (cdrom, floppy) may be given without file= to start empty;
 * snapshot=on keeps every write in memory and discards it on exit.  Other
 * QEMU keys (cache=, aio=, discard=, werror=, ...) are accepted and ignored.
 *
 * Legacy options, exactly as QEMU defines them:
 *   -hda/-hdb/-hdc/-hdd FILE  = -drive file=FILE,index=0..3,media=disk
 *   -cdrom FILE               = -drive file=FILE,index=2,media=cdrom
 *   -fda/-fdb FILE            = -drive file=FILE,index=0..1,if=floppy
 *   -snapshot                 = snapshot=on for every drive
 * (GEMU also accepts -floppy as -fda.)
 *
 * The argument parser builds the drive table; machines claim drives with
 * gemu_drive_get() and call gemu_drive_check_unclaimed() once they are set
 * up, which rejects drives nothing connected (QEMU: "machine type does not
 * support if=..."). gemu_drive_attach_monitor() makes a drive available to
 * the monitor's info block / change / eject under its QEMU name
 * (ide0-hd0, ide1-cd0, floppy0, ... or id=).
 */

typedef enum {
    GEMU_DRIVE_IF_NONE,
    GEMU_DRIVE_IF_IDE,
    GEMU_DRIVE_IF_SCSI,
    GEMU_DRIVE_IF_FLOPPY,
    GEMU_DRIVE_IF_PFLASH,
    GEMU_DRIVE_IF_MTD,
    GEMU_DRIVE_IF_SD,
    GEMU_DRIVE_IF_VIRTIO,
    GEMU_DRIVE_IF_COUNT,
} GemuDriveInterface;

typedef enum {
    GEMU_DRIVE_MEDIA_DISK,
    GEMU_DRIVE_MEDIA_CDROM,
} GemuDriveMedia;

typedef struct GemuDrive GemuDrive;
typedef struct GemuMonitor GemuMonitor;

/* ── Command line ────────────────────────────────────────────────────────── */

/* Interface used when -drive has no if= (the machine family's default;
 * QEMU's PC default is ide). */
void        gemu_drive_set_default_if(GemuDriveInterface iface);

/* -drive OPTS.  Opens the image; prints a QEMU-style error and returns
 * false on bad options or an unreadable file. */
bool        gemu_drive_add(const char *opts);

/* -hda/-cdrom/-fda style shorthand: file at index on iface. */
bool        gemu_drive_add_legacy(GemuDriveInterface iface, int index,
                                  GemuDriveMedia media, const char *file);

/* -snapshot: snapshot=on for every drive, including ones added later. */
void        gemu_drive_snapshot_all(void);

/* ── Machines ────────────────────────────────────────────────────────────── */

/* The drive at bus/unit on iface (marking it claimed), or NULL. */
GemuDrive  *gemu_drive_get(GemuDriveInterface iface, int bus, int unit);

/* Error out for every drive no device claimed. Returns false if any. */
bool        gemu_drive_check_unclaimed(const char *machine);

/* Register the drive with the monitor (info block, change, eject). */
void        gemu_drive_attach_monitor(GemuDrive *d, GemuMonitor *mon);

/* Called when media is inserted or ejected (monitor change/eject), so the
 * device can raise its media-change signal. */
void        gemu_drive_set_change_cb(GemuDrive *d,
                                     void (*cb)(void *ud, bool inserted),
                                     void *ud);

/* ── I/O ─────────────────────────────────────────────────────────────────── */

const char *gemu_drive_name(const GemuDrive *d);
const char *gemu_drive_filename(const GemuDrive *d);   /* "" when empty */
GemuDriveMedia gemu_drive_media(const GemuDrive *d);
bool        gemu_drive_inserted(const GemuDrive *d);
bool        gemu_drive_readonly(const GemuDrive *d);
uint64_t    gemu_drive_size(const GemuDrive *d);       /* bytes; 0 when empty */

/* Byte-addressed I/O.  false on no media, out-of-range access, a write to
 * a read-only drive, or a host I/O error. */
bool        gemu_drive_read(GemuDrive *d, uint64_t offset, void *buf, size_t len);
bool        gemu_drive_write(GemuDrive *d, uint64_t offset, const void *buf, size_t len);

/* Removable media (cdrom, floppy). */
bool        gemu_drive_insert(GemuDrive *d, const char *file, char *err, size_t err_len);
bool        gemu_drive_eject(GemuDrive *d, char *err, size_t err_len);
