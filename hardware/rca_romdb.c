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
    /* Altair 2 */
    { "e103b4f3171ae4f945d4663cce841d4ef8bf247c10fa636d59e7ef65287d1e46",  0x0000, "altair2", "alta2-2716.ic7"  },
    { "1c1ad37bcca3a250e0f4d50e23eb4b8cf8e6ba7954b5cfa1ff59e5a27f0204a3",  0x0800, "altair2", "alta2-2716.ic8"  },
    { "5afa77d6a957b8be7f6331a26caf110f53c35519eccbd68b3e1454069cb53317",  0x1000, "altair2", "alta2-2716.ic9"  },
    { "d0493155d5b9d9534648bfc85c6f4235f31759d4d33963d1b2cb06a405d804ed",  0x1800, "altair2", "alta2-2716.ic10"  },
    { "428d6d56dea2ef4f2b308e0de2abc3f387671654a28f7c8b1dcbab57296b728a",  0x2000, "altair2", "alta2-2716.ic11"  },
    { "a495dc121091f97a0ef70756590862614e49d01dbe7e5be3b85c7a4860c7bd36",  0x2800, "altair2", "alta2-2716.ic12"  },

    /* Apollo 80 */
    { "44e2132aa7a394dc5e1ad4d0750021b7fcf37505d801065b87b656c529508e37",  0x0000, "apollo80", "86676.ic13"  },
    { "be7a432e440a5614e3790f7d7105d64cf0b02c0c62f4991c0ad8d9bab8271118",  0x0400, "apollo80", "86677b.ic14" },
    { "d826c3bdb10be43f62e6fb15a31dc34c6c62a7afd6249fcdb6d40d46f4522d3c",  0x0c00, "apollo80", "87201.ic12"  },

    /* Conic M-1200 */
    { "44e2132aa7a394dc5e1ad4d0750021b7fcf37505d801065b87b656c529508e37",  0x0000, "cm1200", "86676.ic13"  },
    { "be7a432e440a5614e3790f7d7105d64cf0b02c0c62f4991c0ad8d9bab8271118",  0x0400, "cm1200", "86677b.ic14" },
    { "d826c3bdb10be43f62e6fb15a31dc34c6c62a7afd6249fcdb6d40d46f4522d3c",  0x0c00, "cm1200", "87201.ic12"  },

    /* Cosmac VIP */
    { "292dd825ca0ed779e78d55fbab81c898be84ade5f87a661cae9d4509f9df62b8",  0x0000, "vip", "fpb_color.bin"  },
    { "2fd9b152573adfe1c13f7f4c76900efd6980836a8dc52f6bc59c1af784ce6024",  0x0400, "vip", "vip.32.rom" },

    /* COM-100 */
    { "7a95530b9be7fe487e027a5ea7ffe4a7561762cc3fc8cdc535d6f36e5f23c8a0", 0x0000, "visicom", "visicom.q003" },

    /* Destroyer */
    { "7a07c1f2afa8f54aed3cd25d9dcdbcc003b54ef4712956b5b2cae14c0ecf6ad2",  0x0000, "destroyer", "des d c2.ic4"  },
    { "e80f8cf4fa4f1e6637b50232c621289387fd70b9c02bc5e1653e7b13970cbd88",  0x0800, "destroyer", "des d c2.ic5"  },
    { "824637bfab544ffe56e5cd7ab4e9fcae840316c482e7a3ba56af169b5a5cf53a",  0x1000, "destroyer", "des d c2.ic6"  },
    { "e273f6a3cbcf3d897c4ae12808cfcee1b456beae4649fb36f757837cb5dbc06e",  0x1800, "destroyer", "des d c2.ic7"  },
    // A variant 
    { "d4a4efd063e1391abe26ace1e60638ee8acb9f71650e694d0f9a41bad2fed22d",  0x0000, "destroyer", "destryera_1"  },
    { "129bd155efe64e2e647edd4eeed1161578ca80d8a80214981fdfd433ec491cd3",  0x0800, "destroyer", "destryera_2"  },
    { "fe8b2a80d4e537925f9d4a43ef92da92cf8a251ff072fc96ad5dd756ee7132d2",  0x1000, "destroyer", "destryera_3"  },
    { "27072fcbd3b648d05cac6207a92178fb9a7454abae927520b8cebf2d06685b46",  0x1800, "destroyer", "destryera_4"  },

    /* MPT-02 */
    { "44e2132aa7a394dc5e1ad4d0750021b7fcf37505d801065b87b656c529508e37",  0x0000, "mpt02", "86676.ic13"  },
    { "be7a432e440a5614e3790f7d7105d64cf0b02c0c62f4991c0ad8d9bab8271118",  0x0400, "mpt02", "86677b.ic14" },
    { "d826c3bdb10be43f62e6fb15a31dc34c6c62a7afd6249fcdb6d40d46f4522d3c",  0x0c00, "mpt02", "87201.ic12"  },

    /* MPT-02 Jeu */
    { "44e2132aa7a394dc5e1ad4d0750021b7fcf37505d801065b87b656c529508e37",  0x0000, "mpt02j", "86676.ic13"  },

    /* Mustang 9016 */
    { "44e2132aa7a394dc5e1ad4d0750021b7fcf37505d801065b87b656c529508e37",  0x0000, "mtc9016", "86676.ic13"  },
    { "be7a432e440a5614e3790f7d7105d64cf0b02c0c62f4991c0ad8d9bab8271118",  0x0400, "mtc9016", "86677b.ic14" },
    { "d826c3bdb10be43f62e6fb15a31dc34c6c62a7afd6249fcdb6d40d46f4522d3c",  0x0c00, "mtc9016", "87201.ic12"  },

    /* Sheen M1200 */
    { "44e2132aa7a394dc5e1ad4d0750021b7fcf37505d801065b87b656c529508e37",  0x0000, "sm1200", "86676.ic13"  },
    { "be7a432e440a5614e3790f7d7105d64cf0b02c0c62f4991c0ad8d9bab8271118",  0x0400, "sm1200", "86677b.ic14" },
    { "d826c3bdb10be43f62e6fb15a31dc34c6c62a7afd6249fcdb6d40d46f4522d3c",  0x0c00, "sm1200", "87201.ic12"  },

    /* Pecom 32 */
    { "a8104b9bf317828bd480874d8c434bdbf6eb3990a81f45fc7ac4010a1aa70f36", 0x0000, "pecom32", "090786.bin" },

    /* Pecom 64 */
    { "ff253c34a0e26e00c335ec44c16813535700cadd19d404c59dfc2d77c0b62dc1", 0x4000, "pecom64", "rom_2_g_24.02.88_d.bin" },
    { "c482015b19328c20d2f848aa8766a3215b01f7a531f1e02c4a59633fb3200438", 0x0000, "pecom64", "rom_1_g_24.02.88_l.bin" },
    { "8fd89b395c728b25204247b5cb3cfbe7051b3a7dc9eaa9676543cb17f2969cf5", 0x4000, "pecom64", "170887-rom2.bin" },
    { "02a91a4fd3cff5eb6de10a5a531d66b71f1cefddf9a91c7f7a1493f1882c2a44", 0x0000, "pecom64", "170887-rom1.bin" },

    /* RCA Studio II */
    { "1945550109cec3a8003dd64cd70c02bf78326f2f7f91be1fc6fca8067647c54c", 0x0000, "studio2", "84932.ic11" },
    { "e894799e1757cedd73a17cbacad1e0c9a3846eae1b9cfa9d3f6c2a038cd452e6", 0x0200, "studio2", "84933.ic12" },
    { "1e6541bbbd6ceb30271129384da91b50b2b8503e3cc947e9b6e7cc85e1f609bc", 0x0400, "studio2", "85456.ic13" },
    { "0d4d96a4971e419e2919e96c57c7cbc85b8ab8e02898c0c428f5df84f0e209b3", 0x0600, "studio2", "85457.ic14" },
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
