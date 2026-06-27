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
 * Returns number of successful fn calls, or -1 on directory open error. */
typedef bool (*RomDbAddFn)(const char *path, const char *region, uint32_t addr, void *ud);
int romdb_load_dir(const char *dir, const char *machine, RomDbAddFn fn, void *ud);
