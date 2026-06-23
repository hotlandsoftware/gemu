#define _POSIX_C_SOURCE 200809L
#include "rca_romdb.h"
#include "gemu/sha256.h"
#include "gemu/memory.h"
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── Known ROM database ──────────────────────────────────────────────────── */

static const RcaRomDbEntry rom_db[] = {
#include "generated/romdb_rca.inc"
    { NULL, 0, NULL, NULL }
};

const RcaRomDbEntry *rca_romdb_lookup(const char *sha256_hex) {
    for (int i = 0; rom_db[i].sha256; i++)
        if (strcmp(rom_db[i].sha256, sha256_hex) == 0)
            return &rom_db[i];
    return NULL;
}

/* ── Directory scanner ───────────────────────────────────────────────────── */

int rca_romdb_load_dir(RcaConfig *cfg, const char *dir_path,
                       const char *machine_alias) {
    DIR *d = opendir(dir_path);
    if (!d) {
        fprintf(stderr, "gemu-rca: cannot open rom dir '%s'\n", dir_path);
        return -1;
    }

    int loaded = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;

        /* Build full path */
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir_path, ent->d_name);

        /* Skip non-regular files */
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        /* Compute SHA256 */
        uint8_t digest[GEMU_SHA256_DIGEST_LEN];
        if (!gemu_sha256_file(path, digest)) {
            fprintf(stderr, "gemu-rca: cannot read '%s'\n", path);
            continue;
        }
        char hex[65];
        gemu_sha256_hex(digest, hex);

        /* Find entry matching BOTH hash and machine alias */
        const RcaRomDbEntry *e = NULL;
        for (int j = 0; rom_db[j].sha256; j++) {
            if (strcmp(rom_db[j].sha256, hex) == 0 &&
                strcmp(rom_db[j].machine, machine_alias) == 0) {
                e = &rom_db[j];
                break;
            }
        }
        if (!e) {
            /* Check if hash is known at all (for a better error message) */
            const RcaRomDbEntry *any = rca_romdb_lookup(hex);
            if (!any)
                fprintf(stderr, "gemu-rca: unknown ROM '%s' (sha256=%s)\n",
                        ent->d_name, hex);
            continue;
        }

        if (cfg->n_roms >= RCA_MAX_ROM_LOADS) {
            fprintf(stderr, "gemu-rca: too many ROMs (max %d)\n", RCA_MAX_ROM_LOADS);
            break;
        }

        /* Store path — must outlive cfg; strdup it onto the heap */
        char *p = strdup(path);
        if (!p) break;
        cfg->roms[cfg->n_roms].path = p;
        cfg->roms[cfg->n_roms].addr = e->addr;
        cfg->n_roms++;
        loaded++;
        printf("gemu-rca: romdb matched %s (%s) → 0x%04X\n",
               ent->d_name, e->label, e->addr);
    }
    closedir(d);
    return loaded;
}

/* ── Missing ROM hint ────────────────────────────────────────────────────── */

void rca_romdb_print_needed(const char *machine_alias) {
    fprintf(stderr, "gemu-rca: no ROMs provided for '%s'\n", machine_alias);
    fprintf(stderr, "  Known ROMs (identify by SHA256, order doesn't matter):\n");
    bool found = false;
    for (int i = 0; rom_db[i].sha256; i++) {
        if (strcmp(rom_db[i].machine, machine_alias) != 0) continue;
        fprintf(stderr, "    %-20s  0x%04X  sha256=%s\n",
                rom_db[i].label, rom_db[i].addr, rom_db[i].sha256);
        found = true;
    }
    if (!found)
        fprintf(stderr, "    (no entries in database — use -rom addr:file)\n");
    fprintf(stderr, "  Or point at a directory:  -rom /path/to/roms/\n");
}
