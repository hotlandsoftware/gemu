/*
 * x86-sst: runs GEMU's 8086/8088 core against the SingleStepTests 8088 V2
 * suite (https://github.com/SingleStepTests/8088) - per-instruction register,
 * flag and memory results captured from a real AMD D8088.
 *
 *   ninja x86-sst
 *   ./bin/x86-sst roms/tests/8088            # every opcode file
 *   ./bin/x86-sst roms/tests/8088 F6.7 27    # just these
 *
 * Options: -v       print every failing test, not just the first per file
 *          -undef   also check undefined flags and undefined opcodes
 *
 * Only the architectural result is checked; the suite's per-cycle bus
 * traces and prefetch-queue state need a cycle-exact core, which this
 * isn't (see cpu/x86/i8086.h).  Flags the 8088 leaves undefined are masked
 * out using metadata.json, and opcodes it marks "undefined" are skipped,
 * unless -undef is given.
 */
#include "i8086.h"
#include "cJSON.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

#define RAM_SIZE (1u << 20)

static uint8_t  ram[RAM_SIZE];
static uint32_t written[4096];     /* addresses the instruction wrote */
static int      n_written;

static uint8_t mem_read8(uint32_t addr, void *ud) {
    (void)ud;
    return ram[addr & (RAM_SIZE - 1)];
}

static void mem_write8(uint32_t addr, uint8_t val, void *ud) {
    (void)ud;
    addr &= RAM_SIZE - 1;
    ram[addr] = val;
    if (n_written < (int)(sizeof(written) / sizeof(written[0])))
        written[n_written++] = addr;
}

/* The suite's IN reads 0xFF; OUT goes nowhere. */
static uint8_t io_read8(uint16_t port, void *ud) { (void)port; (void)ud; return 0xFF; }
static void    io_write8(uint16_t port, uint8_t val, void *ud) { (void)port; (void)val; (void)ud; }

/* ── Files ───────────────────────────────────────────────────────────────── */

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

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    if (buf && fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); buf = NULL; }
    if (buf) buf[len] = '\0';
    fclose(f);
    return buf;
}

/* ── metadata.json: status and undefined-flag mask per opcode/reg ────────── */

static cJSON *metadata;

/* "F6.7" -> the opcode's entry, or its reg sub-entry. */
static const cJSON *meta_entry(const char *key) {
    char op[3] = { key[0], key[1], 0 };
    const cJSON *e = cJSON_GetObjectItem(cJSON_GetObjectItem(metadata, "opcodes"), op);
    if (e && key[2] == '.') {
        char reg[2] = { key[3], 0 };
        const cJSON *r = cJSON_GetObjectItem(cJSON_GetObjectItem(e, "reg"), reg);
        if (r) e = r;
    }
    return e;
}

static const char *meta_status(const char *key) {
    const cJSON *s = cJSON_GetObjectItem(meta_entry(key), "status");
    return cJSON_IsString(s) ? s->valuestring : "normal";
}

static uint16_t meta_flags_mask(const char *key) {
    const cJSON *m = cJSON_GetObjectItem(meta_entry(key), "flags-mask");
    return cJSON_IsNumber(m) ? (uint16_t)m->valueint : 0xFFFF;
}

/* ── Running one test ────────────────────────────────────────────────────── */

typedef struct { const char *name; size_t off; } RegField;
#define R(n, f) { n, offsetof(X86Cpu, f) }
static const RegField regs[] = {
    R("ax", ax), R("bx", bx), R("cx", cx), R("dx", dx),
    R("cs", cs), R("ss", ss), R("ds", ds), R("es", es),
    R("sp", sp), R("bp", bp), R("si", si), R("di", di),
    R("ip", ip), R("flags", flags),
};
#undef R

static uint16_t *reg_ptr(X86Cpu *c, const RegField *r) {
    return (uint16_t *)((char *)c + r->off);
}

static int ram_value(const cJSON *list, uint32_t addr) {
    const cJSON *e;
    cJSON_ArrayForEach(e, list)
        if ((uint32_t)cJSON_GetArrayItem(e, 0)->valuedouble == addr)
            return cJSON_GetArrayItem(e, 1)->valueint;
    return -1;
}

/* Runs a test; on failure writes what differed to why and returns false. */
static bool run_test(const cJSON *t, uint16_t flags_mask, char *why, size_t why_len) {
    const cJSON *init = cJSON_GetObjectItem(t, "initial");
    const cJSON *fin  = cJSON_GetObjectItem(t, "final");
    const cJSON *iregs = cJSON_GetObjectItem(init, "regs"), *fregs = cJSON_GetObjectItem(fin, "regs");
    const cJSON *iram = cJSON_GetObjectItem(init, "ram"), *fram = cJSON_GetObjectItem(fin, "ram");

    X86Cpu c;
    memset(&c, 0, sizeof(c));
    c.type = X86_CPU_8088;
    x86_reset(&c);
    c.mem_read8 = mem_read8; c.mem_write8 = mem_write8;
    c.io_read8 = io_read8;   c.io_write8 = io_write8;
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++)
        *reg_ptr(&c, &regs[i]) = (uint16_t)cJSON_GetObjectItem(iregs, regs[i].name)->valueint;

    const cJSON *e;
    cJSON_ArrayForEach(e, iram)
        ram[(uint32_t)cJSON_GetArrayItem(e, 0)->valuedouble & (RAM_SIZE - 1)] =
            (uint8_t)cJSON_GetArrayItem(e, 1)->valueint;
    n_written = 0;

    x86_step(&c);

    bool ok = true;
    size_t n = 0;
    why[0] = '\0';
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        const cJSON *v = cJSON_GetObjectItem(fregs, regs[i].name);
        uint16_t want = (uint16_t)(v ? v : cJSON_GetObjectItem(iregs, regs[i].name))->valueint;
        uint16_t got = *reg_ptr(&c, &regs[i]);
        uint16_t mask = strcmp(regs[i].name, "flags") == 0 ? flags_mask : 0xFFFF;
        if ((want & mask) != (got & mask)) {
            ok = false;
            n += (size_t)snprintf(why + n, n < why_len ? why_len - n : 0, " %s=%04X(want %04X)",
                                  regs[i].name, got, want);
        }
    }
    /* A divide error (the suite points INT 0 at 0000:0400) pushes FLAGS,
     * undefined bits and all: compare that word with the flags mask too. */
    uint32_t pushed_flags = 0xFFFFFFFFu;
    if (c.cs == 0 && c.ip == 0x400)
        pushed_flags = x86_linear(c.ss, (uint16_t)(c.sp + 4));
    #define BYTE_MASK(a) ((a) == pushed_flags ? (uint8_t)flags_mask : \
                          (a) == ((pushed_flags + 1) & (RAM_SIZE - 1)) ? (uint8_t)(flags_mask >> 8) : 0xFF)

    /* Memory: every byte the suite says changed, and nothing written elsewhere. */
    cJSON_ArrayForEach(e, fram) {
        uint32_t a = (uint32_t)cJSON_GetArrayItem(e, 0)->valuedouble & (RAM_SIZE - 1);
        int want = cJSON_GetArrayItem(e, 1)->valueint;
        if ((ram[a] ^ want) & BYTE_MASK(a)) {
            ok = false;
            n += (size_t)snprintf(why + n, n < why_len ? why_len - n : 0, " [%05X]=%02X(want %02X)",
                                  a, ram[a], want);
        }
    }
    for (int i = 0; i < n_written; i++) {
        uint32_t a = written[i];
        int want = ram_value(fram, a);
        if (want < 0) want = ram_value(iram, a);
        if (want < 0) want = 0;    /* never touched by the real CPU */
        if ((ram[a] ^ want) & BYTE_MASK(a)) {
            ok = false;
            n += (size_t)snprintf(why + n, n < why_len ? why_len - n : 0, " stray [%05X]=%02X", a, ram[a]);
        }
    }

    #undef BYTE_MASK

    /* back to all-zero RAM for the next test */
    cJSON_ArrayForEach(e, iram) ram[(uint32_t)cJSON_GetArrayItem(e, 0)->valuedouble & (RAM_SIZE - 1)] = 0;
    for (int i = 0; i < n_written; i++) ram[written[i]] = 0;
    return ok;
}

/* ── Main ────────────────────────────────────────────────────────────────── */

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int main(int argc, char **argv) {
    bool verbose = false, undef = false;
    const char *dir = NULL;
    char *only[512];
    int n_only = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = true;
        else if (!strcmp(argv[i], "-undef")) undef = true;
        else if (!dir) dir = argv[i];
        else if (n_only < 512) only[n_only++] = argv[i];
    }
    if (!dir) {
        fprintf(stderr, "usage: x86-sst [-v] [-undef] DIR [OPCODE...]\n"
                        "  DIR holds the SingleStepTests 8088 v2 *.json.gz and metadata.json\n");
        return 2;
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/metadata.json", dir);
    char *meta_text = read_file(path);
    metadata = meta_text ? cJSON_Parse(meta_text) : NULL;
    if (!metadata) fprintf(stderr, "x86-sst: no %s - checking every flag\n", path);

    /* the test files, in opcode order */
    DIR *dp = opendir(dir);
    if (!dp) { perror(dir); return 2; }
    char *files[600];
    int n_files = 0;
    struct dirent *de;
    while ((de = readdir(dp)) && n_files < 600) {
        size_t len = strlen(de->d_name);
        if (len > 8 && !strcmp(de->d_name + len - 8, ".json.gz")) files[n_files++] = strdup(de->d_name);
    }
    closedir(dp);
    qsort(files, (size_t)n_files, sizeof(files[0]), cmp_str);

    long total = 0, failed = 0;
    int files_run = 0, files_failed = 0, skipped = 0;
    char why[2048];
    for (int f = 0; f < n_files; f++) {
        char key[16];
        snprintf(key, sizeof(key), "%.*s", (int)(strlen(files[f]) - 8), files[f]);
        if (n_only) {
            bool want = false;
            for (int i = 0; i < n_only; i++) want |= !strcasecmp(only[i], key);
            if (!want) continue;
        }
        const char *status = metadata ? meta_status(key) : "normal";
        if (!undef && (!strcmp(status, "undefined") || !strcmp(status, "fpu"))) {
            skipped++;
            continue;
        }
        uint16_t mask = (metadata && !undef) ? meta_flags_mask(key) : 0xFFFF;

        snprintf(path, sizeof(path), "%s/%s", dir, files[f]);
        char *text = read_gz(path);
        cJSON *tests = text ? cJSON_Parse(text) : NULL;
        free(text);
        if (!cJSON_IsArray(tests)) {
            fprintf(stderr, "%s: can't read\n", path);
            cJSON_Delete(tests);
            continue;
        }
        long n = 0, bad = 0;
        const cJSON *t;
        cJSON_ArrayForEach(t, tests) {
            n++;
            if (run_test(t, mask, why, sizeof(why))) continue;
            if (bad++ == 0 || verbose)
                printf("  FAIL %-6s #%-5d %-36s%s\n", key,
                       cJSON_GetObjectItem(t, "idx")->valueint,
                       cJSON_GetObjectItem(t, "name")->valuestring, why);
        }
        cJSON_Delete(tests);
        printf("%-6s %-12s %6ld/%-6ld %s\n", key, status, n - bad, n, bad ? "FAIL" : "ok");
        fflush(stdout);
        total += n;
        failed += bad;
        files_run++;
        if (bad) files_failed++;
    }
    printf("\n%d opcode files: %d passed, %d failed (%d undefined/fpu skipped)\n"
           "%ld tests: %ld passed, %ld failed\n",
           files_run, files_run - files_failed, files_failed, skipped, total, total - failed, failed);
    return failed ? 1 : 0;
}
