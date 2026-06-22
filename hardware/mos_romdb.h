#pragma once
#include "mos6502cfg.h"
#include <stdint.h>

typedef struct {
    const char *sha256;   /* 64-char lowercase hex digest */
    uint16_t    addr;     /* CPU load address */
    const char *machine;  /* machine alias (matches -M argument) */
    const char *label;    /* canonical filename */
} MosRomDbEntry;

const MosRomDbEntry *mos_romdb_lookup(const char *sha256_hex);

/* Scan a directory, hash every file, load any that match machine_alias.
 * Returns number of ROMs loaded, or -1 on error. */
int  mos_romdb_load_dir(MosConfig *cfg, const char *dir_path,
                        const char *machine_alias);

/* Print the known ROMs for machine_alias to stderr (no-ROM hint). */
void mos_romdb_print_needed(const char *machine_alias);
