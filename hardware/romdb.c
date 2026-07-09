#define _POSIX_C_SOURCE 200809L
#include "romdb.h"
#include "gemu/sha256.h"
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#ifdef HAVE_LIBZIP
#include <zip.h>
#endif

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
        fprintf(stderr, "    (no entries in database - use -rom ADDR:FILE)\n");
    fprintf(stderr, "  Or point at a directory:  -rom /path/to/roms/\n");
#ifdef HAVE_LIBZIP
    fprintf(stderr, "  Or a zip file:            -rom /path/to/roms.zip\n");
#endif
}

#ifdef HAVE_LIBZIP
/* Extracted once per -rom FILE.zip, cleaned up on normal process exit -
 * the extracted paths need to outlive this function (the machine's own
 * create() reads them later, after ROM-database matching is done). */
static char zip_tmpdir[64];
static bool zip_tmpdir_active = false;

static void romdb_zip_cleanup(void) {
    if (!zip_tmpdir_active) return;
    DIR *d = opendir(zip_tmpdir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char path[600];
            snprintf(path, sizeof(path), "%s/%s", zip_tmpdir, ent->d_name);
            remove(path);
        }
        closedir(d);
    }
    rmdir(zip_tmpdir);
    zip_tmpdir_active = false;
}

/* Extracts every regular file in the zip to a fresh temp directory (flat -
 * subfolder structure inside the zip, if any, is discarded) and returns
 * that directory's path, or NULL on failure. */
static const char *romdb_extract_zip(const char *zip_path) {
    int err = 0;
    zip_t *za = zip_open(zip_path, ZIP_RDONLY, &err);
    if (!za) {
        fprintf(stderr, "gemu: cannot open zip '%s'\n", zip_path);
        return NULL;
    }

    snprintf(zip_tmpdir, sizeof(zip_tmpdir), "/tmp/gemu-rom-XXXXXX");
    if (!mkdtemp(zip_tmpdir)) {
        fprintf(stderr, "gemu: cannot create temp dir for '%s'\n", zip_path);
        zip_close(za);
        return NULL;
    }
    zip_tmpdir_active = true;
    atexit(romdb_zip_cleanup);

    zip_int64_t n = zip_get_num_entries(za, 0);
    for (zip_int64_t i = 0; i < n; i++) {
        const char *name = zip_get_name(za, i, 0);
        if (!name || !*name) continue;
        /* Flatten any subfolder path and skip directory entries (those
         * end in '/'); also avoids zip-slip writes outside the temp dir. */
        const char *base = strrchr(name, '/');
        base = base ? base + 1 : name;
        if (!*base) continue;

        zip_stat_t zst;
        if (zip_stat_index(za, i, 0, &zst) != 0) continue;

        zip_file_t *zf = zip_fopen_index(za, i, 0);
        if (!zf) continue;
        uint8_t *buf = malloc(zst.size);
        if (!buf) { zip_fclose(zf); continue; }
        zip_int64_t got = zip_fread(zf, buf, zst.size);
        zip_fclose(zf);
        if (got != (zip_int64_t)zst.size) { free(buf); continue; }

        char outpath[600];
        snprintf(outpath, sizeof(outpath), "%s/%s", zip_tmpdir, base);
        FILE *out = fopen(outpath, "wb");
        if (out) {
            fwrite(buf, 1, zst.size, out);
            fclose(out);
        }
        free(buf);
    }
    zip_close(za);
    return zip_tmpdir;
}
#endif

bool romdb_is_zip(const char *path) {
#ifdef HAVE_LIBZIP
    size_t len = strlen(path);
    return len > 4 && strcasecmp(path + len - 4, ".zip") == 0;
#else
    (void)path;
    return false;
#endif
}

int romdb_load_dir(const char *dir, const char *machine, RomDbAddFn fn, void *ud) {
#ifdef HAVE_LIBZIP
    if (romdb_is_zip(dir)) {
        const char *extracted = romdb_extract_zip(dir);
        if (!extracted) return -1;
        dir = extracted;
    }
#endif
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
