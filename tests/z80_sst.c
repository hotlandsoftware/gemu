/*
 * z80-sst: runs GEMU's Z80 core against the SingleStepTests Z80 suite
 * (https://github.com/SingleStepTests/z80) - per-instruction register,
 * memory and T-state results, undocumented flags and MEMPTR included.
 *
 *   ninja z80-sst
 *   ./bin/z80-sst roms/tests/z80                 # every opcode file
 *   ./bin/z80-sst roms/tests/z80 "ed b0" 27      # just these
 *
 * Options: -v   print every failing test, not just the first per file
 *
 * Checked: every register (WZ, Q, the EI/P latches, IFF1/2, IM, R
 * included), RAM, the I/O writes, and the instruction's length in
 * T-states.  IN reads are fed the values the suite recorded.
 */
#include "z80.h"
#include "cJSON.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

static uint8_t  ram[65536];
static uint16_t written[256];
static int      n_written;

/* the suite's port activity for the current test */
static const cJSON *ports;
static int  port_pos;
static bool port_bad;
static char port_why[128];

static uint8_t mem_read(void *ud, uint16_t a) { (void)ud; return ram[a]; }

static void mem_write(void *ud, uint16_t a, uint8_t v) {
    (void)ud;
    ram[a] = v;
    if (n_written < 256) written[n_written++] = a;
}

static const cJSON *next_port(uint16_t port, const char *dir) {
    const cJSON *p = cJSON_GetArrayItem(ports, port_pos++);
    if (!p) {
        if (!port_bad) snprintf(port_why, sizeof(port_why), " extra %s %04X", dir, port);
        port_bad = true;
        return NULL;
    }
    uint16_t want = (uint16_t)cJSON_GetArrayItem(p, 0)->valueint;
    const char *wdir = cJSON_GetArrayItem(p, 2)->valuestring;
    if (want != port || strcmp(wdir, dir)) {
        if (!port_bad) snprintf(port_why, sizeof(port_why), " %s %04X (want %s %04X)", dir, port, wdir, want);
        port_bad = true;
    }
    return p;
}

static uint8_t io_in(void *ud, uint16_t port) {
    (void)ud;
    const cJSON *p = next_port(port, "r");
    return p ? (uint8_t)cJSON_GetArrayItem(p, 1)->valueint : 0xFF;
}

static void io_out(void *ud, uint16_t port, uint8_t v) {
    (void)ud;
    const cJSON *p = next_port(port, "w");
    if (p && cJSON_GetArrayItem(p, 1)->valueint != v) {
        if (!port_bad) snprintf(port_why, sizeof(port_why), " out %04X=%02X (want %02X)", port, v,
                                cJSON_GetArrayItem(p, 1)->valueint);
        port_bad = true;
    }
}

static char *read_gz(const char *path) {
    gzFile f = gzopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 24, len = 0;
    char *buf = malloc(cap + 1);
    int n;
    while (buf && (n = gzread(f, buf + len, (unsigned)(cap - len))) > 0) {
        len += (size_t)n;
        if (len == cap) {
            char *nb = realloc(buf, (cap *= 2) + 1);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb;
        }
    }
    gzclose(f);
    if (buf) buf[len] = '\0';
    return buf;
}

/* ── Registers ───────────────────────────────────────────────────────────── */

typedef struct { const char *name; size_t off; int size; } RegField;
#define R8(n, f)  { n, offsetof(Z80, f), 1 }
#define R16(n, f) { n, offsetof(Z80, f), 2 }
#define RB(n, f)  { n, offsetof(Z80, f), 0 }
static const RegField regs[] = {
    R8("a", a), R8("f", f), R8("b", b), R8("c", c), R8("d", d), R8("e", e), R8("h", h), R8("l", l),
    R16("af_", af_), R16("bc_", bc_), R16("de_", de_), R16("hl_", hl_),
    R16("ix", ix), R16("iy", iy), R16("sp", sp), R16("pc", pc), R16("wz", wz),
    R8("i", i), R8("r", r), R8("im", im), R8("q", q),
    RB("iff1", iff1), RB("iff2", iff2), RB("ei", ei), RB("p", p),
};

static int get_reg(const Z80 *c, const RegField *r) {
    const char *p = (const char *)c + r->off;
    switch (r->size) {
    case 1: return *(const uint8_t *)p;
    case 2: return *(const uint16_t *)p;
    default: return *(const bool *)p;
    }
}

static void set_reg(Z80 *c, const RegField *r, int v) {
    char *p = (char *)c + r->off;
    switch (r->size) {
    case 1: *(uint8_t *)p = (uint8_t)v; break;
    case 2: *(uint16_t *)p = (uint16_t)v; break;
    default: *(bool *)p = v != 0; break;
    }
}

static int ram_value(const cJSON *list, uint16_t addr) {
    const cJSON *e;
    cJSON_ArrayForEach(e, list)
        if ((uint16_t)cJSON_GetArrayItem(e, 0)->valueint == addr)
            return cJSON_GetArrayItem(e, 1)->valueint;
    return -1;
}

#define WHY(...) (n += (size_t)snprintf(why + n, n < why_len ? why_len - n : 0, __VA_ARGS__))

static bool run_test(const cJSON *t, char *why, size_t why_len) {
    const cJSON *init = cJSON_GetObjectItem(t, "initial");
    const cJSON *fin  = cJSON_GetObjectItem(t, "final");
    const cJSON *iram = cJSON_GetObjectItem(init, "ram"), *fram = cJSON_GetObjectItem(fin, "ram");

    Z80 c;
    z80_init(&c);
    c.read = mem_read; c.write = mem_write; c.in = io_in; c.out = io_out;
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        const cJSON *v = cJSON_GetObjectItem(init, regs[i].name);
        if (v) set_reg(&c, &regs[i], v->valueint);
    }
    const cJSON *e;
    cJSON_ArrayForEach(e, iram)
        ram[(uint16_t)cJSON_GetArrayItem(e, 0)->valueint] = (uint8_t)cJSON_GetArrayItem(e, 1)->valueint;
    n_written = 0;
    ports = cJSON_GetObjectItem(t, "ports");
    port_pos = 0;
    port_bad = false;

    int cycles = z80_step(&c);

    bool ok = true;
    size_t n = 0;
    why[0] = '\0';
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        const cJSON *v = cJSON_GetObjectItem(fin, regs[i].name);
        if (!v) continue;
        int got = get_reg(&c, &regs[i]);
        if (got != v->valueint) {
            ok = false;
            WHY(" %s=%0*X(want %0*X)", regs[i].name, regs[i].size == 2 ? 4 : 2, got,
                regs[i].size == 2 ? 4 : 2, v->valueint);
        }
    }
    int want_cycles = cJSON_GetArraySize(cJSON_GetObjectItem(t, "cycles"));
    if (cycles != want_cycles) { ok = false; WHY(" T=%d(want %d)", cycles, want_cycles); }
    if (port_bad) { ok = false; WHY("%s", port_why); }
    else if (ports && port_pos != cJSON_GetArraySize(ports)) { ok = false; WHY(" ports %d/%d", port_pos, cJSON_GetArraySize(ports)); }

    cJSON_ArrayForEach(e, fram) {
        uint16_t a = (uint16_t)cJSON_GetArrayItem(e, 0)->valueint;
        int want = cJSON_GetArrayItem(e, 1)->valueint;
        if (ram[a] != want) { ok = false; WHY(" [%04X]=%02X(want %02X)", a, ram[a], want); }
    }
    for (int i = 0; i < n_written; i++) {
        uint16_t a = written[i];
        if (ram_value(fram, a) < 0) { ok = false; WHY(" stray [%04X]=%02X", a, ram[a]); }
    }

    cJSON_ArrayForEach(e, iram) ram[(uint16_t)cJSON_GetArrayItem(e, 0)->valueint] = 0;
    for (int i = 0; i < n_written; i++) ram[written[i]] = 0;
    return ok;
}

/* ── Main ────────────────────────────────────────────────────────────────── */

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int main(int argc, char **argv) {
    bool verbose = false;
    const char *dir = NULL;
    char *only[512];
    int n_only = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = true;
        else if (!dir) dir = argv[i];
        else if (n_only < 512) only[n_only++] = argv[i];
    }
    if (!dir) {
        fprintf(stderr, "usage: z80-sst [-v] DIR [OPCODE...]\n"
                        "  DIR holds the SingleStepTests Z80 v1 *.json.gz files\n");
        return 2;
    }

    DIR *dp = opendir(dir);
    if (!dp) { perror(dir); return 2; }
    char **files = NULL;
    int n_files = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(dp))) {
        size_t len = strlen(de->d_name);
        if (len <= 8 || strcmp(de->d_name + len - 8, ".json.gz")) continue;
        if (n_files == cap) files = realloc(files, (size_t)(cap = cap ? cap * 2 : 1024) * sizeof(*files));
        files[n_files++] = strdup(de->d_name);
    }
    closedir(dp);
    qsort(files, (size_t)n_files, sizeof(files[0]), cmp_str);

    long total = 0, failed = 0;
    int files_run = 0, files_failed = 0;
    char why[2048], path[1024];
    for (int f = 0; f < n_files; f++) {
        char key[32];
        snprintf(key, sizeof(key), "%.*s", (int)(strlen(files[f]) - 8), files[f]);
        if (n_only) {
            bool want = false;
            for (int i = 0; i < n_only; i++) want |= !strcasecmp(only[i], key);
            if (!want) continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, files[f]);
        char *text = read_gz(path);
        cJSON *tests = text ? cJSON_Parse(text) : NULL;
        free(text);
        if (!cJSON_IsArray(tests)) {
            fprintf(stderr, "%s: can't read\n", path);
            cJSON_Delete(tests);
            continue;
        }
        long nt = 0, bad = 0;
        const cJSON *t;
        cJSON_ArrayForEach(t, tests) {
            nt++;
            if (run_test(t, why, sizeof(why))) continue;
            if (bad++ == 0 || verbose)
                printf("  FAIL %-14s %s\n", cJSON_GetObjectItem(t, "name")->valuestring, why);
        }
        cJSON_Delete(tests);
        if (bad || verbose) printf("%-14s %5ld/%-5ld FAIL\n", key, nt - bad, nt);
        fflush(stdout);
        total += nt;
        failed += bad;
        files_run++;
        if (bad) files_failed++;
    }
    printf("\n%d opcode files: %d passed, %d failed\n%ld tests: %ld passed, %ld failed\n",
           files_run, files_run - files_failed, files_failed, total, total - failed, failed);
    return failed ? 1 : 0;
}
