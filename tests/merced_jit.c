/* Differential test: -accel jit against the interpreter.
 *
 * Generates random IA-64 programs from the instructions the translator
 * handles natively or by direct call (A-unit ALU and compares, zxt/sxt,
 * tbit/extr/dep.z/shrp, movl, loads/stores with post-increment, br.cond,
 * br.cloop, plus a system instruction that takes the full helper path),
 * runs each from the same random register state - static, banked and
 * rotating stacked GRs, rotating predicates, NaT bits, a timer deadline -
 * through merced_run_fast() under both engines, and compares the whole
 * architectural state and memory.  Faults land on dead-loop bundles, so
 * fault delivery is compared too.
 *
 * Run with: python3 tools/ia64/run_merced_jit_test.py [programs] [seed] */
#include "merced.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE   (1u << 20)
#define CODE_BASE  0x20000u
#define DATA_BASE  0x80000u
#define DATA_SIZE  0x10000u
#define N_BUNDLES  48

typedef struct {
    uint8_t *ram;
} Mem;

static uint64_t mem_read(void *ud, uint64_t addr, unsigned size) {
    Mem *m = ud;
    uint64_t v = 0;
    if (addr < RAM_SIZE && addr + size <= RAM_SIZE)
        memcpy(&v, m->ram + addr, size);
    else
        v = size == 8 ? ~UINT64_C(0) : (UINT64_C(1) << (size * 8)) - 1;
    return v;
}

static void mem_write(void *ud, uint64_t addr, uint64_t v, unsigned size) {
    Mem *m = ud;
    if (addr < RAM_SIZE && addr + size <= RAM_SIZE)
        memcpy(m->ram + addr, &v, size);
}

static uint8_t *mem_page(void *ud, uint64_t pa_page, uint16_t *slow_off) {
    Mem *m = ud;
    *slow_off = 0xFFFF;
    return pa_page + 0x1000 <= RAM_SIZE ? m->ram + pa_page : NULL;
}

/* ── random source (xorshift64*) ──────────────────────────────────────── */
static uint64_t rng_state;
static uint64_t rnd(void) {
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * UINT64_C(2685821657736338717);
}
static unsigned rn(unsigned n) { return (unsigned)(rnd() % n); }

/* ── encoders ─────────────────────────────────────────────────────────── */
#define F(v, lo, w) ((((uint64_t)(v)) & ((UINT64_C(1) << (w)) - 1)) << (lo))

static const uint64_t NOP_MI = UINT64_C(1) << 27;     /* nop.m / nop.i */
static const uint64_t NOP_B  = F(2, 37, 4);

static void put_bundle(uint8_t *at, unsigned tmpl, uint64_t s0, uint64_t s1,
                       uint64_t s2) {
    uint64_t lo = tmpl | (s0 << 5) | (s1 << 46);
    uint64_t hi = (s1 >> 18) | (s2 << 23);
    memcpy(at, &lo, 8);
    memcpy(at + 8, &hi, 8);
}

/* "nop.m; nop.i; br self": merced_step() halts on it (MERCED_HALT_DEADLOOP) */
static void put_deadloop(uint8_t *at) {
    put_bundle(at, 0x10, NOP_MI, NOP_MI, F(4, 37, 4));
}

/* Registers: never r2/r3 as a destination (the memory base pointers). */
static unsigned rsrc(void) {
    switch (rn(4)) {
    case 0:  return rn(16);
    case 1:  return 16 + rn(16);
    default: return 32 + rn(96);
    }
}
static unsigned rdst(void) {
    unsigned r;
    do r = rsrc(); while (r == 2 || r == 3);
    return r;
}
static unsigned rqp(void) { return rn(2) ? 0 : rn(64); }
static unsigned rpr(void) { return rn(64); }

/* A-unit instruction (valid in M and I slots) */
static uint64_t gen_alu(void) {
    unsigned qp = rqp();
    switch (rn(9)) {
    case 0: {                                   /* A1 add/sub/logic */
        static const uint8_t x4x2b[][2] = { {0,0},{0,1},{1,1},{1,0},
                                            {3,0},{3,1},{3,2},{3,3} };
        unsigned k = rn(8);
        return F(8,37,4) | F(x4x2b[k][0],29,4) | F(x4x2b[k][1],27,2) |
               F(rsrc(),20,7) | F(rsrc(),13,7) | F(rdst(),6,7) | F(qp,0,6);
    }
    case 1:                                     /* A2 shladd */
        return F(8,37,4) | F(4,29,4) | F(rn(4),27,2) |
               F(rsrc(),20,7) | F(rsrc(),13,7) | F(rdst(),6,7) | F(qp,0,6);
    case 2: {                                   /* A3 imm8 sub/logic */
        unsigned imm = rn(256);
        bool sub = rn(5) == 0;
        return F(8,37,4) | F(imm >> 7,36,1) | F(sub ? 9 : 0xB,29,4) |
               F(sub ? 1 : rn(4),27,2) | F(rsrc(),20,7) | F(imm,13,7) |
               F(rdst(),6,7) | F(qp,0,6);
    }
    case 3: {                                   /* A4 adds imm14 */
        unsigned imm = rn(1u << 14);
        return F(8,37,4) | F(imm >> 13,36,1) | F(2,34,2) | F(imm >> 7,27,6) |
               F(rsrc(),20,7) | F(imm,13,7) | F(rdst(),6,7) | F(qp,0,6);
    }
    case 4: {                                   /* A5 addl imm22 */
        unsigned imm = rn(1u << 22);
        return F(9,37,4) | F(imm >> 21,36,1) | F(imm >> 7,27,9) |
               F(imm >> 16,22,5) | F(rn(4),20,2) | F(imm,13,7) |
               F(rdst(),6,7) | F(qp,0,6);
    }
    case 5: case 6: {                           /* A6 cmp reg */
        return F(0xC + rn(3),37,4) | F(rn(2),34,2) | F(rpr(),27,6) |
               F(rsrc(),20,7) | F(rsrc(),13,7) | F(rn(2),12,1) |
               F(rpr(),6,6) | F(qp,0,6);
    }
    default: {                                  /* A8 cmp imm */
        unsigned imm = rn(256);
        return F(0xC + rn(3),37,4) | F(imm >> 7,36,1) | F(2 + rn(2),34,2) |
               F(rpr(),27,6) | F(rsrc(),20,7) | F(imm,13,7) |
               F(rn(2),12,1) | F(rpr(),6,6) | F(qp,0,6);
    }
    }
}

/* I-unit-only instruction (direct-call path, plus zxt/sxt native) */
static uint64_t gen_i(void) {
    unsigned qp = rqp();
    switch (rn(7)) {
    case 0: {                                   /* zxt/sxt */
        static const uint8_t x6[] = { 0x10, 0x11, 0x12, 0x14, 0x15, 0x16 };
        return F(x6[rn(6)],27,6) | F(rsrc(),20,7) | F(rdst(),6,7) | F(qp,0,6);
    }
    case 1:                                     /* tbit (normal / unc) */
        return F(5,37,4) | F(rpr(),27,6) | F(rsrc(),20,7) | F(rn(64),14,6) |
               F(rn(2),12,1) | F(rpr(),6,6) | F(qp,0,6);
    case 2:                                     /* extr / extr.u */
        return F(5,37,4) | F(1,34,2) | F(rn(64),27,6) | F(rsrc(),20,7) |
               F(rn(64),14,6) | F(rn(2),13,1) | F(rdst(),6,7) | F(qp,0,6);
    case 3:                                     /* dep.z */
        return F(5,37,4) | F(1,34,2) | F(1,33,1) | F(rn(64),27,6) |
               F(rn(64),20,6) | F(rsrc(),13,7) | F(rdst(),6,7) | F(qp,0,6);
    case 4:                                     /* shrp */
        return F(5,37,4) | F(3,34,2) | F(rn(64),27,6) | F(rsrc(),20,7) |
               F(rsrc(),13,7) | F(rdst(),6,7) | F(qp,0,6);
    case 5:                                     /* mov r1 = pr */
        return F(0x33,27,6) | F(rdst(),6,7) | F(qp,0,6);
    default:
        return gen_alu();
    }
}

/* M-unit instruction */
static uint64_t gen_m(void) {
    unsigned qp = rqp();
    unsigned base = 2 + rn(2);
    switch (rn(8)) {
    case 0: case 1:                             /* ld1-8 (+ reg postinc) */
        return F(4,37,4) | F(rn(4) ? 0 : 1,36,1) | F(rn(4),30,6) |
               F(base,20,7) | F(rsrc(),13,7) | F(rdst(),6,7) | F(qp,0,6);
    case 2: {                                   /* ld imm postinc */
        unsigned imm = rn(512);
        return F(5,37,4) | F(imm >> 8,36,1) | F(rn(4),30,6) | F(imm >> 7,27,1) |
               F(base,20,7) | F(imm,13,7) | F(rdst(),6,7) | F(qp,0,6);
    }
    case 3:                                     /* st1-8 */
        return F(4,37,4) | F(0x30 + rn(4),30,6) | F(base,20,7) |
               F(rsrc(),13,7) | F(qp,0,6);
    case 4: {                                   /* st imm postinc */
        unsigned imm = rn(512);
        return F(5,37,4) | F(imm >> 8,36,1) | F(0x30 + rn(4),30,6) |
               F(imm >> 7,27,1) | F(base,20,7) | F(rsrc(),13,7) |
               F(imm,6,7) | F(qp,0,6);
    }
    case 5:
        if (rn(4) == 0)                         /* mov.m ar.ccv = r (helper) */
            return F(1,37,4) | F(0x2A,27,6) | F(32,20,7) | F(rsrc(),13,7) |
                   F(qp,0,6);
        return gen_alu();
    default:
        return gen_alu();
    }
}

static uint64_t gen_b(unsigned at, unsigned n_bundles) {
    unsigned qp = rn(16) ? 1 + rn(63) : 0;    /* rarely unconditional */
    unsigned target = at + 1 + rn(n_bundles + 1 - at); /* forward, or past
                                                         * the loop branch */
    int64_t disp = (int64_t)target - (int64_t)at;     /* in bundles */
    switch (rn(4)) {
    case 0:  return NOP_B;
    case 1:                                            /* br.cloop */
        return F(4,37,4) | F((uint64_t)disp >> 20,36,1) | F(disp,13,20) |
               F(5,6,3);
    default:                                           /* br.cond */
        return F(4,37,4) | F((uint64_t)disp >> 20,36,1) | F(disp,13,20) |
               F(qp,0,6);
    }
}

static void gen_program(uint8_t *ram) {
    static const uint8_t tmpls[] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
                                     0x08, 0x09, 0x0A, 0x0B, 0x10, 0x11,
                                     0x18, 0x19 };
    for (unsigned i = 0; i < N_BUNDLES; i++) {
        uint8_t *at = ram + CODE_BASE + 16 * i;
        unsigned t = tmpls[rn(sizeof tmpls)];
        uint64_t s[3];
        if (t == 0x04 || t == 0x05) {                  /* MLX movl */
            uint64_t imm = rnd();
            s[0] = rn(2) ? gen_m() : NOP_MI;
            s[1] = (imm >> 22) & ((UINT64_C(1) << 41) - 1);
            s[2] = F(6,37,4) | F(imm >> 63,36,1) | F(imm >> 7,27,9) |
                   F(imm >> 16,22,5) | F(imm >> 21,21,1) | F(imm,13,7) |
                   F(rdst(),6,7) | F(rqp(),0,6);
        } else {
            static const char *units[] = { [0x00] = "MII", [0x01] = "MII",
                [0x02] = "MII", [0x03] = "MII", [0x08] = "MMI",
                [0x09] = "MMI", [0x0A] = "MMI", [0x0B] = "MMI",
                [0x10] = "MIB", [0x11] = "MIB", [0x18] = "MMB",
                [0x19] = "MMB" };
            for (unsigned k = 0; k < 3; k++) {
                char u = units[t][k];
                s[k] = rn(6) == 0 ? (u == 'B' ? NOP_B : NOP_MI)
                     : u == 'M' ? gen_m() : u == 'I' ? gen_i()
                     : gen_b(i, N_BUNDLES);
            }
        }
        put_bundle(at, t, s[0], s[1], s[2]);
    }
    /* loop back to the start ar.lc more times: re-entered blocks */
    int64_t back = -(int64_t)N_BUNDLES;
    put_bundle(ram + CODE_BASE + 16 * N_BUNDLES, 0x11, NOP_MI, NOP_MI,
               F(4,37,4) | F((uint64_t)back >> 20,36,1) | F(back,13,20) |
               F(5,6,3));
}

/* ── state ────────────────────────────────────────────────────────────── */

static void init_state(Merced *m, uint64_t seed) {
    rng_state = seed * UINT64_C(0x9E3779B97F4A7C15) + 1;
    for (unsigned r = 1; r < 32; r++) {
        m->gr_static[r] = rnd();
        m->nat_static[r] = rn(30) == 0;
    }
    for (unsigned r = 0; r < 16; r++) {
        m->gr_bank0[r] = rnd();
        m->nat_bank0[r] = rn(30) == 0;
    }
    for (unsigned r = 0; r < MERCED_RSE_CAPACITY; r++) {
        m->gr_stack[r] = rnd();
        m->nat_stack[r] = rn(30) == 0;
    }
    /* memory base pointers (r2, r3 in both banks): valid, no NaT */
    m->gr_static[2] = DATA_BASE + DATA_SIZE / 4 + rn(DATA_SIZE / 2);
    m->gr_static[3] = DATA_BASE + DATA_SIZE / 4 + rn(DATA_SIZE / 2);
    m->nat_static[2] = m->nat_static[3] = 0;
    m->pr = rnd() | 1;
    unsigned sor = rn(13);                             /* sor*8 <= 96 */
    uint64_t rrb_gr = sor ? rn(sor * 8) : 0;
    uint64_t rrb_pr = rn(48);
    m->cfm = 96 | ((uint64_t)sor << 14) | (rrb_gr << 18) | (rrb_pr << 32);
    m->bof = rn(MERCED_RSE_CAPACITY);
    m->bof_total = 100000 + rn(100000);
    m->rse_flushed_regs = (int64_t)(m->bof_total + rn(200));
    /* physical mode, interruption collection on, interrupts off, bank? */
    m->psr = (UINT64_C(1) << 13) | (rn(2) ? UINT64_C(1) << 44 : 0);
    m->ar[65] = rn(4);                                 /* ar.lc */
    m->ar[44] = rnd() >> 8;                            /* ar.itc */
    m->cr[1] = rn(2) ? m->ar[44] + 20 + rn(300) : 0;   /* cr.itm */
    m->cr[2] = 0;                                      /* cr.iva */
    m->ip = CODE_BASE;
    m->group_start = 1;
}

static uint8_t *fresh_ram(uint64_t seed) {
    uint8_t *ram = malloc(RAM_SIZE);
    for (unsigned a = 0; a < RAM_SIZE; a += 16)
        put_deadloop(ram + a);
    rng_state = seed * UINT64_C(0xD1B54A32D192ED03) + 7;
    for (unsigned a = 0; a < DATA_SIZE; a += 8) {
        uint64_t v = rnd();
        memcpy(ram + DATA_BASE + a, &v, 8);
    }
    gen_program(ram);
    return ram;
}

/* Everything architectural, with host-only caches cleared. */
static void normalize(Merced *m) {
    memset(&m->bus, 0, sizeof(m->bus));
    m->translation_cache = NULL;
    m->bundle_cache_valid = m->bundle_cache_translation_valid = false;
    m->bundle_cache_va = m->bundle_cache_pa = 0;
    m->bundle_cache_lo = m->bundle_cache_hi = 0;
    m->bundle_cache_tmpl = 0;
    memset(m->bundle_cache_slots, 0, sizeof(m->bundle_cache_slots));
    m->itr_hint = m->dtr_hint = m->itc_hint = m->dtc_hint = 0;
}

static uint64_t total_slots, total_faulted, total_blocks_end;

static int run_one(uint64_t seed, bool verbose) {
    Mem mem[2];
    Merced *cpu[2];
    MercedStatus st[2];
    uint64_t slots[2];
    for (int e = 0; e < 2; e++) {
        mem[e].ram = fresh_ram(seed);
        MercedBus bus = { .ud = &mem[e], .ram_size = RAM_SIZE,
                          .read = mem_read, .write = mem_write };
        cpu[e] = merced_create(&bus);
        if (e == 1) {
            if (merced_set_accel(cpu[e], MERCED_ACCEL_JIT) != MERCED_ACCEL_JIT) {
                fprintf(stderr, "merced_jit: JIT unavailable on this host\n");
                exit(77);
            }
            merced_set_direct_ram(cpu[e], mem_page);
        }
        init_state(cpu[e], seed);
        slots[e] = 0;
        do {
            unsigned n = 0;
            st[e] = merced_run_fast(cpu[e], 64 + (unsigned)(seed % 200), &n,
                                    0, 0);
            slots[e] += n;
        } while (st[e] == MERCED_OK && slots[e] < 200000);
    }

    int bad = 0;
    if (st[0] != st[1] || slots[0] != slots[1]) {
        fprintf(stderr, "seed %" PRIu64 ": status %d/%d after %" PRIu64
                "/%" PRIu64 " slots\n", seed, st[0], st[1], slots[0], slots[1]);
        bad = 1;
    }
    if (memcmp(mem[0].ram, mem[1].ram, RAM_SIZE)) {
        for (unsigned a = 0; a < RAM_SIZE; a++)
            if (mem[0].ram[a] != mem[1].ram[a]) {
                fprintf(stderr, "seed %" PRIu64 ": memory differs at 0x%x "
                        "(%02x vs %02x)\n", seed, a, mem[0].ram[a],
                        mem[1].ram[a]);
                break;
            }
        bad = 1;
    }
    Merced *a = malloc(sizeof(Merced)), *b = malloc(sizeof(Merced));
    memcpy(a, cpu[0], sizeof(Merced));
    memcpy(b, cpu[1], sizeof(Merced));
    normalize(a);
    normalize(b);
    if (memcmp(a, b, sizeof(Merced))) {
        const uint8_t *pa = (const uint8_t *)a, *pb = (const uint8_t *)b;
        unsigned shown = 0;
        static const struct { const char *name; size_t off, size; } fields[] = {
#define FLD(f) { #f, offsetof(Merced, f), sizeof(((Merced *)0)->f) }
            FLD(ip), FLD(psr), FLD(cfm), FLD(pr), FLD(gr_static),
            FLD(gr_bank0), FLD(nat_static), FLD(nat_bank0), FLD(gr_stack),
            FLD(nat_stack), FLD(bof), FLD(bof_total), FLD(rse_flushed_regs),
            FLD(fr), FLD(br), FLD(ar), FLD(cr), FLD(timer_pending),
            FLD(taken), FLD(group_start), FLD(slot_stop_after), FLD(ninsts),
            FLD(nfaults), FLD(last_successful_bundle), FLD(halt_msg),
            FLD(halt_ip),
#undef FLD
        };
        for (size_t i = 0; i < sizeof(Merced) && shown < 6; i++) {
            if (pa[i] == pb[i]) continue;
            const char *name = "?";
            size_t base = i;
            for (size_t f = 0; f < sizeof fields / sizeof *fields; f++)
                if (i >= fields[f].off && i < fields[f].off + fields[f].size) {
                    name = fields[f].name;
                    base = fields[f].off;
                }
            fprintf(stderr, "seed %" PRIu64 ": state differs: %s +%zu "
                    "(%02x vs %02x)\n", seed, name, i - base, pa[i], pb[i]);
            shown++;
            i |= 7;                           /* one report per qword */
        }
        bad = 1;
    }
    total_slots += slots[0];
    total_faulted += cpu[0]->nfaults != 0;
    total_blocks_end += cpu[0]->ip == CODE_BASE + 16 * (N_BUNDLES + 1);
    if (bad || verbose)
        fprintf(stderr, "seed %" PRIu64 ": %s, %" PRIu64 " slots, ip=%016"
                PRIX64 " ninsts=%" PRIu64 " faults=%" PRIu64 "\n", seed,
                bad ? "MISMATCH" : "ok", slots[0], cpu[0]->ip, cpu[0]->ninsts,
                cpu[0]->nfaults);
    free(a);
    free(b);
    for (int e = 0; e < 2; e++) {
        merced_destroy(cpu[e]);
        free(mem[e].ram);
    }
    return bad;
}

int main(int argc, char **argv) {
    unsigned n = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 3000;
    uint64_t first = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;
    unsigned failures = 0;
    for (unsigned i = 0; i < n; i++) {
        failures += (unsigned)run_one(first + i, false);
        if (failures >= 10) break;
    }
    if (failures) {
        fprintf(stderr, "merced_jit: %u of %u programs differ\n", failures, n);
        return 1;
    }
    printf("merced_jit: %u random programs identical under jit and "
           "interpreter (%" PRIu64 " slots; %" PRIu64 " ran to the end, %"
           PRIu64 " took a fault)\n", n, total_slots, total_blocks_end,
           total_faulted);
    return 0;
}
