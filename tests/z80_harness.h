/* What the z80 family's tests (stage Z) share: a CHECK, hashing, the
 * report going to its own stream (the machines print to stdout, which the
 * tests send to a file and hash).  The machine's .c is included before
 * this. */
#include "gemu/sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#  include <unistd.h>
#endif

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

static FILE *rep;   /* the report (the real stdout) */

/* The machine's stdout to OUT_FILE; the report to the real stdout. */
static void harness_start(const char *out_file) {
#ifndef _WIN32
    rep = fdopen(dup(1), "w");
#else
    rep = stderr;
#endif
    CHECK(rep && freopen(out_file, "w", stdout));
}

static void hash(const char *label, const void *p, size_t n) {
    uint8_t d[GEMU_SHA256_DIGEST_LEN];
    GemuSha256Ctx c;
    gemu_sha256_init(&c);
    gemu_sha256_update(&c, p, n);
    gemu_sha256_final(&c, d);
    char hex[65];
    gemu_sha256_hex(d, hex);
    fprintf(rep, " %s %.12s", label, hex);
}

/* What the machine printed so far. */
static inline void hash_output(const char *out_file) {
    fflush(stdout);
    FILE *f = fopen(out_file, "rb");
    static uint8_t buf[1 << 20];
    size_t n = f ? fread(buf, 1, sizeof buf, f) : 0;
    if (f) fclose(f);
    hash("out", buf, n);
}

static inline const char *write_file(const char *name, const unsigned char *data, size_t len) {
    FILE *f = fopen(name, "wb");
    CHECK(f && fwrite(data, 1, len, f) == len);
    fclose(f);
    return name;
}

static void z80_report(const Z80 *c, uint64_t insns) {
    fprintf(rep, " AF %02X%02X BC %02X%02X DE %02X%02X HL %02X%02X IX %04X IY %04X SP %04X PC %04X"
            " IFF %d%d IM %d R %02X T %llu insn %llu\n",
            c->a, c->f, c->b, c->c, c->d, c->e, c->h, c->l, c->ix, c->iy, c->sp, c->pc,
            c->iff1, c->iff2, c->im, c->r, (unsigned long long)c->cycles, (unsigned long long)insns);
}
