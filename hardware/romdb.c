#define _POSIX_C_SOURCE 200809L
#include "romdb.h"
#include "gemu/sha256.h"
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>

static const RomDbEntry rom_db[] = {
#include "generated/romdb.inc"
    { NULL, 0, NULL, NULL, NULL }
};

const RomDbEntry *romdb_lookup(const char *sha256_hex) {
    for (int i = 0; rom_db[i].sha256; i++)
        if (strcmp(rom_db[i].sha256, sha256_hex) == 0)
            return &rom_db[i];
    return NULL;
}

void romdb_print_needed(const char *machine_alias) {
    fprintf(stderr, "gemu: no ROMs provided for '%s'\n", machine_alias);
    fprintf(stderr, "  Known ROMs (identified by SHA256, order doesn't matter):\n");
    bool found = false;
    for (int i = 0; rom_db[i].sha256; i++) {
        if (strcmp(rom_db[i].machine, machine_alias) != 0) continue;
        fprintf(stderr, "    %-32s  %-8s  0x%04X  sha256=%s\n",
                rom_db[i].label, rom_db[i].region ? rom_db[i].region : "",
                (unsigned)rom_db[i].addr, rom_db[i].sha256);
        found = true;
    }
    if (!found)
        fprintf(stderr, "    (no entries in database — use -rom ADDR:FILE)\n");
    fprintf(stderr, "  Or point at a directory:  -rom /path/to/roms/\n");
}

int romdb_load_dir(const char *dir, const char *machine, RomDbAddFn fn, void *ud) {
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "gemu: cannot open rom dir '%s'\n", dir);
        return -1;
    }

    int loaded = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;

        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        uint8_t digest[GEMU_SHA256_DIGEST_LEN];
        if (!gemu_sha256_file(path, digest)) {
            fprintf(stderr, "gemu: cannot read '%s'\n", path);
            continue;
        }
        char hex[65];
        gemu_sha256_hex(digest, hex);

        const RomDbEntry *e = NULL;
        for (int j = 0; rom_db[j].sha256; j++) {
            if (strcmp(rom_db[j].sha256, hex) == 0 &&
                strcmp(rom_db[j].machine, machine) == 0) {
                e = &rom_db[j];
                break;
            }
        }
        if (!e) {
            if (!romdb_lookup(hex))
                fprintf(stderr, "gemu: unknown ROM '%s' (sha256=%s)\n", ent->d_name, hex);
            continue;
        }

        printf("gemu: romdb matched %s (%s) → %s:0x%04X\n",
               ent->d_name, e->label, e->region ? e->region : "",
               (unsigned)e->addr);
        if (!fn(path, e->region, e->addr, ud))
            break;
        loaded++;
    }
    closedir(d);
    return loaded;
}
