#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    const char *sha256;   /* 64-char lowercase hex digest */
    uint32_t    addr;     /* load address or region-relative offset */
    const char *region;   /* optional ROM region, e.g. "prg", "gfx1" */
    const char *machine;  /* machine alias, e.g. "nes", "studio2" */
    const char *label;    /* canonical filename, e.g. "disksys.rom" */
} RomDbEntry;

/* Returns the first entry matching sha256_hex, or NULL if unknown. */
const RomDbEntry *romdb_lookup(const char *sha256_hex);

/* Print the known ROM set for machine_alias to stderr. */
void romdb_print_needed(const char *machine_alias);

/* Scan dir for files whose SHA256 matches machine_alias in the database.
 * Calls fn(path, region, addr, ud) for each match; fn returns false to stop early.
 * Returns number of successful fn calls, or -1 on directory/zip open error.
 * dir may also be a .zip file (see romdb_is_zip) when built with libzip —
 * its contents are extracted to a temp directory first, transparently. */
typedef bool (*RomDbAddFn)(const char *path, const char *region, uint32_t addr, void *ud);
int romdb_load_dir(const char *dir, const char *machine, RomDbAddFn fn, void *ud);

/* True if path ends in ".zip" AND this build has libzip support. Callers
 * use this to decide whether a -rom argument that isn't a directory should
 * still be routed through romdb_load_dir() instead of treated as a single
 * raw ROM file. */
bool romdb_is_zip(const char *path);
