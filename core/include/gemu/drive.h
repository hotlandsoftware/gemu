#pragma once
#include <stdbool.h>

/*
 * Shared QEMU-style "-drive" argument, e.g.:
 *   -drive file=disk.iso,if=ide,index=1,media=cdrom
 *
 * One parser, reused by every machine family's setup code (ia64's -cdrom/
 * -hda are sugar built on top of it; x86 is expected to consume it too).
 * Mapping a parsed spec onto actual hardware (which bus, which of that
 * bus's few emulated device slots) is still up to the caller - this only
 * turns the string into structured fields.
 */

typedef enum {
    GEMU_DRIVE_IF_IDE,
    GEMU_DRIVE_IF_FLOPPY,
} GemuDriveInterface;

typedef enum {
    GEMU_DRIVE_MEDIA_DISK,
    GEMU_DRIVE_MEDIA_CDROM,
} GemuDriveMedia;

typedef struct {
    char               file[512];
    GemuDriveInterface if_type;  /* default: ide */
    int                index;    /* default: 0 */
    GemuDriveMedia     media;    /* default: disk */
    bool               readonly; /* default: false; forced true for media=cdrom */
} GemuDriveSpec;

/*
 * Parse a "-drive" argument (comma-separated key=value pairs: file=, if=,
 * index=, media=, readonly=, format=raw). file= is the only required key.
 *
 * Returns true and fills *out on success. Returns false and prints
 * "gemu: -drive: <reason>" to stderr on malformed input or an unknown/
 * unsupported key or value.
 */
bool gemu_parse_drive_spec(const char *arg, GemuDriveSpec *out);
