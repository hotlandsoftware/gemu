/*
 * IA-64 architectural microprogram harness.
 *
 * Runs a flat list of instruction bundles on a bare Merced core backed by a
 * plain RAM buffer - no machine, no firmware, no guest OS - and prints the
 * resulting architectural state.
 *
 * Input file (see tools/ia64_conformance.py, which generates it):
 *   entry     <hex>            initial IP (bundle address | slot)
 *   terminal  <hex>            stop once IP reaches this bundle (optional)
 *   maxinsts  <dec>            instruction budget (default 200000)
 *   bundle    <addr> <lo> <hi> 16-byte bundle, two 64-bit halves
 *   mem       <addr> <size> <value>   memory initializer, size in {1,2,4,8}
 *
 * Output: one "IA64TEST key=value ..." line, then per-register lines.  Values
 * are hex without a 0x prefix so the driver can parse them uniformly.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "merced.h"

/* Cases address the whole 64-bit physical space - some deliberately test
 * memory above 4 GiB - so back it sparsely: 4 KiB pages allocated on first
 * touch, in a small open-addressed hash.  A flat buffer would either waste
 * gigabytes or turn "we do not model this address" into a false test failure. */
#define SELFTEST_PAGE_SHIFT 12
#define SELFTEST_PAGE_SIZE  (1u << SELFTEST_PAGE_SHIFT)
#define SELFTEST_PAGE_SLOTS 4096          /* power of two; 16 MiB resident max */

typedef struct {
    uint64_t tag;                          /* page number + 1; 0 = empty */
    uint8_t *data;
} SelftestPage;

typedef struct {
    SelftestPage pages[SELFTEST_PAGE_SLOTS];
} SelftestBus;

static uint8_t *selftest_page(SelftestBus *b, uint64_t addr, bool create) {
    uint64_t pn = addr >> SELFTEST_PAGE_SHIFT;
    uint64_t tag = pn + 1;
    size_t i = (size_t)((pn * 0x9E3779B97F4A7C15ull) >> 52) &
               (SELFTEST_PAGE_SLOTS - 1);
    for (size_t probe = 0; probe < SELFTEST_PAGE_SLOTS; probe++) {
        SelftestPage *p = &b->pages[(i + probe) & (SELFTEST_PAGE_SLOTS - 1)];
        if (p->tag == tag) return p->data;
        if (p->tag == 0) {
            if (!create) return NULL;
            p->data = calloc(1, SELFTEST_PAGE_SIZE);
            if (!p->data) return NULL;
            p->tag = tag;
            return p->data;
        }
    }
    return NULL;                            /* table full: treat as open bus */
}

static uint64_t selftest_read(void *ud, uint64_t addr, unsigned size) {
    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        uint8_t *pg = selftest_page(ud, addr + i, false);
        /* Untouched memory reads as all-ones, matching the machines' open bus. */
        uint8_t byte = pg ? pg[(addr + i) & (SELFTEST_PAGE_SIZE - 1)] : 0xFF;
        v |= (uint64_t)byte << (i * 8);
    }
    return v;
}

static void selftest_write(void *ud, uint64_t addr, uint64_t val,
                           unsigned size) {
    for (unsigned i = 0; i < size; i++) {
        uint8_t *pg = selftest_page(ud, addr + i, true);
        if (pg) pg[(addr + i) & (SELFTEST_PAGE_SIZE - 1)] =
                    (uint8_t)(val >> (i * 8));
    }
}

static void emit_state(const Merced *m, const char *stop_reason) {
    printf("IA64TEST stop=%s ip=%" PRIX64 " psr=%" PRIX64 " cfm=%" PRIX64
           " pr=%" PRIX64 " nfaults=%" PRIu64 " ninsts=%" PRIu64 "\n",
           stop_reason, m->ip, m->psr, m->cfm, m->pr, m->nfaults, m->ninsts);
    /* cr.iip/cr.ifa/cr.isr describe the last collected interruption; the
     * cases assert on them as fault_ip / fault_code. */
    printf("IA64TEST cr iip=%" PRIX64 " ipsr=%" PRIX64 " ifa=%" PRIX64
           " isr=%" PRIX64 " iim=%" PRIX64 " itir=%" PRIX64 " iha=%" PRIX64
           " iva=%" PRIX64 "\n",
           m->cr[19] /*IIP*/, m->cr[16] /*IPSR*/, m->cr[20] /*IFA*/,
           m->cr[17] /*ISR*/, m->cr[24] /*IIM*/, m->cr[21] /*ITIR*/,
           m->cr[25] /*IHA*/, m->cr[2] /*IVA*/);
    for (unsigned r = 0; r < 128; r++)
        printf("IA64TEST r%u=%" PRIX64 "\n", r, merced_gr(m, r));
    for (unsigned f = 0; f < 32; f++)
        printf("IA64TEST f%u sign=%u exp=%X sig=%" PRIX64 "\n", f,
               m->fr[f].sign, m->fr[f].exp, m->fr[f].sig);
    for (unsigned a = 0; a < 128; a++)
        if (m->ar[a])
            printf("IA64TEST ar%u=%" PRIX64 "\n", a, m->ar[a]);
    /*
     * A few reference cases inspect the translation-cache dump directly
     * instead of expressing the expected entry as a register value.  Keep
     * this compatible with QEMU's `info registers` spelling.
     */
    for (unsigned i = 0; i < MERCED_N_TC; i++) {
        const MercedTlbEntry *e = &m->dtc[i];
        if (!e->valid)
            continue;
        unsigned ar = (unsigned)((e->pte >> 9) & 7);
        unsigned pl = (unsigned)((e->pte >> 7) & 3);
        unsigned perm = (e->pte & 1) ? ar : 0;
        printf("DTLB[%u] TC va=0x%016" PRIx64
               " pa=0x%016" PRIx64 " ps=0x%016x"
               " rid=0x%06" PRIx32 " key=0x000000"
               " ar=%u pl=%u perm=0x%x pte=0x%016" PRIx64 "\n",
               i, e->va_start, e->pfn_base, e->ps, e->rid,
               ar, pl, perm, e->pte);
    }
}

int ia64_selftest_main(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "gemu: cannot open microprogram '%s'\n", path);
        return 1;
    }

    SelftestBus bus_state;
    memset(&bus_state, 0, sizeof(bus_state));

    uint64_t entry = 0x10, terminal = 0;
    bool have_terminal = false;
    uint64_t maxinsts = 200000;
    char line[512];

    while (fgets(line, sizeof(line), f)) {
        uint64_t a, lo, hi, val;
        unsigned sz;
        if (sscanf(line, "bundle %" SCNx64 " %" SCNx64 " %" SCNx64,
                   &a, &lo, &hi) == 3) {
            selftest_write(&bus_state, a, lo, 8);
            selftest_write(&bus_state, a + 8, hi, 8);
        } else if (sscanf(line, "mem %" SCNx64 " %u %" SCNx64,
                          &a, &sz, &val) == 3) {
            if (sz >= 1 && sz <= 8)
                selftest_write(&bus_state, a, val, sz);
        } else if (sscanf(line, "entry %" SCNx64, &a) == 1) {
            entry = a;
        } else if (sscanf(line, "terminal %" SCNx64, &a) == 1) {
            terminal = a; have_terminal = true;
        } else if (sscanf(line, "maxinsts %" SCNu64, &a) == 1) {
            maxinsts = a;
        }
    }
    fclose(f);

    MercedBus bus = { .ud = &bus_state, .ram_size = UINT64_C(0x40000000),
                      .read = selftest_read,
                      .write = selftest_write };
    Merced *m = merced_create(&bus);
    if (!m)
        return 1;
    /* merced_reset() leaves the core at the architected reset vector in
     * physical mode; the cases want to start at their own entry instead. */
    m->ip = entry;
    /* merced_reset() also sets cfm.sof=96 ("whole stacked file addressable")
     * as a deliberate, non-architectural convenience so real firmware can
     * touch r32+ before its first alloc. The conformance suite is written
     * against true architectural reset state (cfm=0: no stacked registers
     * allocated until software calls alloc), so undo that convenience here -
     * this only affects -microprogram runs, not real machine boot. */
    m->cfm = 0;
    /* Keep the architected PALE_RESET PSR.  Individual conformance programs
     * enable IC explicitly when they require collected interruption state;
     * forcing it here destroys tests of 0->1 in-flight IC transitions. */

    const char *reason = "maxinsts";
    for (uint64_t i = 0; i < maxinsts; i++) {
        if (have_terminal && (m->ip & ~0xFull) == (terminal & ~0xFull)) {
            reason = "terminal";
            break;
        }
        MercedStatus st = merced_step(m);
        if (st != MERCED_OK) {
            reason = (st == MERCED_HALT_UNIMPL) ? "unimpl" : "halt";
            break;
        }
    }
    if (have_terminal && (m->ip & ~0xFull) == (terminal & ~0xFull))
        reason = "terminal";

    /* On a bail-out the halt message names the instruction we could not
     * execute - that is the single most actionable line of output here. */
    if (m->halt_msg[0] && (!strcmp(reason, "unimpl") || !strcmp(reason, "halt")))
        printf("IA64TEST halt msg=%s at=%" PRIX64 "\n", m->halt_msg,
               m->halt_ip);
    emit_state(m, reason);
    merced_destroy(m);
    for (size_t i = 0; i < SELFTEST_PAGE_SLOTS; i++)
        free(bus_state.pages[i].data);
    return 0;
}
