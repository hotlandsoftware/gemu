/* What the rca family's tests (stage R) share: writing the stand-ins out,
 * hashing, a CHECK.  The machine's .c is included before this. */
#include "data/rca/standin.h"
#include "gemu/sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

/* RCA_ROM=FILE (a real ROM image) stands in for the stand-in. */
static const char *write_file(const char *name, const unsigned char *data, size_t len) {
    static unsigned char real[0x10000];
    const char *env = getenv("RCA_ROM");
    if (env) {
        FILE *r = fopen(env, "rb");
        CHECK(r);
        len = fread(real, 1, sizeof real, r);
        fclose(r);
        data = real;
    }
    FILE *f = fopen(name, "wb");
    CHECK(f && fwrite(data, 1, len, f) == len);
    fclose(f);
    return name;
}

static void hash(const char *label, const void *p, size_t n) {
    uint8_t d[GEMU_SHA256_DIGEST_LEN];
    GemuSha256Ctx c;
    gemu_sha256_init(&c);
    gemu_sha256_update(&c, p, n);
    gemu_sha256_final(&c, d);
    char hex[65];
    gemu_sha256_hex(d, hex);
    printf(" %s %.12s", label, hex);
}

static void cpu_report(const Cdp1802 *c) {
    printf(" D%02X DF%d P%X X%X IE%d Q%d R0 %04X R3 %04X R4 %04X R5 %04X cyc %llu insn %llu\n",
           c->D, c->DF, c->P, c->X, c->IE, c->Q, c->R[0], c->R[3], c->R[4], c->R[5],
           (unsigned long long)c->cycle_count, (unsigned long long)c->insn_count);
}

static RcaConfig rca_cfg(RcaMachineType m, RcaVgaType vga, const char *rom, uint32_t addr) {
    RcaConfig c;
    memset(&c, 0, sizeof c);
    c.machine = m;
    c.cpu = RCA_CPU_CDP1802;
    c.vga = vga;
    c.display_type = GEMU_DISPLAY_NONE;
    c.roms[0].path = rom;
    c.roms[0].addr = addr;
    c.n_roms = 1;
    return c;
}
