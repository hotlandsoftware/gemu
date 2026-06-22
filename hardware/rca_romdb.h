#pragma once
#include "rca.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const char *sha256;   /* 64-char lowercase hex */
    uint32_t    addr;     /* load address */
    const char *machine;  /* machine alias, e.g. "studio2", "apollo80" */
    const char *label;    /* human-readable, e.g. "84932.ic11" */
} RcaRomDbEntry;

/* Returns the entry matching sha256_hex, or NULL if unknown. */
const RcaRomDbEntry *rca_romdb_lookup(const char *sha256_hex);

/*
 * Scan dir_path for files whose SHA256 is in the database and whose machine
 * field matches machine_alias. Matched files are appended to cfg->roms.
 * Returns the number of ROMs loaded, or -1 on directory open error.
 * Unknown files are reported to stderr with their hash so they can be added.
 */
int rca_romdb_load_dir(RcaConfig *cfg, const char *dir_path,
                       const char *machine_alias);

/* Print the known ROM set for machine_alias to stderr with SHA256s. */
void rca_romdb_print_needed(const char *machine_alias);
