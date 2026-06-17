#define _POSIX_C_SOURCE 200809L
#include "romdb.h"
#include "gemu/sha256.h"
#include "gemu/memory.h"
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── Known ROM database ──────────────────────────────────────────────────── */

static const MosRomDbEntry rom_db[] = {
    /* Generic MOS 6502 */
    { "9f00c8a4055f441894683462ac8ecbf007d15df29cef77e03555977f9a03a2f0",  0x0000, "generic", "6502_functional_test.bin"  },

    /* KIM-1 */
    { "79c6dbd54aa3911f53c1068c4310aabceb7a6ce9714d1c402b73af76ea109237",  0x1800, "kim1",    "6530-002.bin"               },
    { "80f5c6393e0bbcfc8a5b68fd73858da34e6d6a415f32d4820f21edb52fecafec",  0x1C00, "kim1",    "6530-003.bin"               },

    /* NES — Famicom Disk System BIOS */
    { "99c18490ed9002d9c6d999b9d8d15be5c051bdfa7cc7e73318053c9a994b0178",  0xE000, "nes",     "disksys.rom"               },
    { "99c18490ed9002d9c6d999b9d8d15be5c051bdfa7cc7e73318053c9a994b0178",  0xE000, "famicom", "disksys.rom"               },

    { NULL, 0, NULL, NULL }
};

/* ── Lookup ──────────────────────────────────────────────────────────────── */

const MosRomDbEntry *mos_romdb_lookup(const char *sha256_hex) {
    for (int i = 0; rom_db[i].sha256; i++)
        if (strcmp(rom_db[i].sha256, sha256_hex) == 0)
            return &rom_db[i];
    return NULL;
}

/* ── Directory scanner ───────────────────────────────────────────────────── */

int mos_romdb_load_dir(MosConfig *cfg, const char *dir_path,
                       const char *machine_alias) {
    DIR *d = opendir(dir_path);
    if (!d) {
        fprintf(stderr, "gemu-6502: cannot open rom dir '%s'\n", dir_path);
        return -1;
    }

    int loaded = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;

        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir_path, ent->d_name);

        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        uint8_t digest[GEMU_SHA256_DIGEST_LEN];
        if (!gemu_sha256_file(path, digest)) {
            fprintf(stderr, "gemu-6502: cannot read '%s'\n", path);
            continue;
        }
        char hex[65];
        gemu_sha256_hex(digest, hex);

        const MosRomDbEntry *e = NULL;
        for (int j = 0; rom_db[j].sha256; j++) {
            if (strcmp(rom_db[j].sha256, hex) == 0 &&
                strcmp(rom_db[j].machine, machine_alias) == 0) {
                e = &rom_db[j];
                break;
            }
        }
        if (!e) {
            const MosRomDbEntry *any = mos_romdb_lookup(hex);
            if (!any)
                fprintf(stderr, "gemu-6502: unknown ROM '%s' (sha256=%s)\n",
                        ent->d_name, hex);
            continue;
        }

        if (cfg->n_roms >= MOS_MAX_ROM_LOADS) {
            fprintf(stderr, "gemu-6502: too many ROMs (max %d)\n", MOS_MAX_ROM_LOADS);
            break;
        }

        char *p = strdup(path);
        if (!p) break;
        cfg->roms[cfg->n_roms].path = p;
        cfg->roms[cfg->n_roms].addr = e->addr;
        cfg->n_roms++;
        loaded++;
        printf("gemu-6502: romdb matched %s (%s) → 0x%04X\n",
               ent->d_name, e->label, (unsigned)e->addr);
    }
    closedir(d);
    return loaded;
}

/* ── Missing ROM hint ────────────────────────────────────────────────────── */

void mos_romdb_print_needed(const char *machine_alias) {
    fprintf(stderr, "gemu-6502: no ROMs provided for '%s'\n", machine_alias);
    fprintf(stderr, "  Known ROMs (identified by SHA256, order doesn't matter):\n");
    bool found = false;
    for (int i = 0; rom_db[i].sha256; i++) {
        if (strcmp(rom_db[i].machine, machine_alias) != 0) continue;
        fprintf(stderr, "    %-32s  0x%04X  sha256=%s\n",
                rom_db[i].label, (unsigned)rom_db[i].addr, rom_db[i].sha256);
        found = true;
    }
    if (!found)
        fprintf(stderr, "    (no entries in database — use -rom ADDR:FILE)\n");
    fprintf(stderr, "  Or point at a directory:  -rom /path/to/roms/\n");
}
