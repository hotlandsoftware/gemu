/* Merced MSR file and cache-array model, through the interpreter.
 *
 * Every access goes through the real core: a bundle holding `mov msr[r3]=r2`,
 * `mov r1=msr[r3]`, ld8, st8 or fc is built in RAM and run with merced_step(),
 * so the checks cover the instruction decode, the model, the data-access and
 * fetch hooks and the machine check delivery.  merced_msr_access() is also
 * called directly where only the register file matters.
 *
 * Run with: python3 tools/ia64/run_merced_msr_test.py */
#include "merced.h"
#include "merced_msr.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE   (1u << 20)
#define CODE       0x20000u
#define F(v, lo, w) ((((uint64_t)(v)) & ((UINT64_C(1) << (w)) - 1)) << (lo))

static uint8_t ram[RAM_SIZE];
static unsigned failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "merced_msr:%d: check failed: %s\n", __LINE__, #cond); \
        failures++; \
    } \
} while (0)
#define CHECK_EQ(got, want) do { \
    uint64_t g_ = (uint64_t)(got), w_ = (uint64_t)(want); \
    if (g_ != w_) { \
        fprintf(stderr, "merced_msr:%d: %s = 0x%" PRIx64 ", expected 0x%" PRIx64 "\n", \
                __LINE__, #got, g_, w_); \
        failures++; \
    } \
} while (0)

static uint64_t bus_read(void *ud, uint64_t addr, unsigned size) {
    (void)ud;
    uint64_t v = 0;
    if (addr + size <= RAM_SIZE)
        memcpy(&v, ram + addr, size);
    return v;
}

static void bus_write(void *ud, uint64_t addr, uint64_t v, unsigned size) {
    (void)ud;
    if (addr + size <= RAM_SIZE)
        memcpy(ram + addr, &v, size);
}

static uint64_t ram_u64(uint64_t addr) { return bus_read(NULL, addr, 8); }

/* ── bundles ────────────────────────────────────────────────────────────── */

#define NOP_MI (UINT64_C(1) << 27)

static void put_bundle(uint8_t *at, unsigned tmpl, uint64_t s0, uint64_t s1,
                       uint64_t s2) {
    uint64_t lo = tmpl | (s0 << 5) | (s1 << 46);
    uint64_t hi = (s1 >> 18) | (s2 << 23);
    memcpy(at, &lo, 8);
    memcpy(at + 8, &hi, 8);
}

/* r8 = destination, r9 = value, r10 = index / address */
#define MOV_TO_MSR   (F(1, 37, 4) | F(0x06, 27, 6) | F(10, 20, 7) | F(9, 13, 7))
#define MOV_FROM_MSR (F(1, 37, 4) | F(0x16, 27, 6) | F(10, 20, 7) | F(8, 6, 7))
#define LD8          (F(4, 37, 4) | F(0x03, 30, 6) | F(10, 20, 7) | F(8, 6, 7))
#define ST8          (F(4, 37, 4) | F(0x33, 30, 6) | F(10, 20, 7) | F(9, 13, 7))
#define FC           (F(1, 37, 4) | F(0x30, 27, 6) | F(10, 20, 7))
#define ADDS_R8(imm) (F(8, 37, 4) | F(2, 34, 2) | F((imm) >> 7, 27, 6) | \
                      F(0, 20, 7) | F(imm, 13, 7) | F(8, 6, 7))
#define PSR_MC_BIT   (UINT64_C(1) << 35)
#define CR_IIP       19
#define CR_CMCV      74
#define PALE_CHECK   UINT64_C(0xFFFFFF90)

/* Runs the bundle at `at` (physical) up to its end or an interruption. */
static void run_bundle(Merced *cpu, uint64_t at) {
    cpu->ip = at;
    cpu->group_start = 1;
    for (unsigned i = 0; i < 3 && (cpu->ip & ~UINT64_C(0xF)) == at; i++)
        CHECK_EQ(merced_step(cpu), MERCED_OK);
}

/* One M-unit instruction in slot 0 of an MII bundle.  The bundle is stored
 * through the bus as a guest store would, which drops the decoded copy of
 * what was there. */
static void run_insn(Merced *cpu, uint64_t insn) {
    uint64_t word[2];
    put_bundle((uint8_t *)word, 0x00, insn, NOP_MI, NOP_MI);
    merced_bus_write(cpu, CODE, word[0], 8);
    merced_bus_write(cpu, CODE + 8, word[1], 8);
    run_bundle(cpu, CODE);
}

static void wrmsr(Merced *cpu, unsigned idx, uint64_t v) {
    cpu->gr_static[9] = v;
    cpu->gr_static[10] = idx;
    run_insn(cpu, MOV_TO_MSR);
}

static uint64_t rdmsr(Merced *cpu, unsigned idx) {
    cpu->gr_static[10] = idx;
    run_insn(cpu, MOV_FROM_MSR);
    return cpu->gr_static[8];
}

static uint64_t load8(Merced *cpu, uint64_t pa) {
    cpu->gr_static[10] = pa;
    run_insn(cpu, LD8);
    return cpu->gr_static[8];
}

static void store8(Merced *cpu, uint64_t pa, uint64_t v) {
    cpu->gr_static[10] = pa;
    cpu->gr_static[9] = v;
    run_insn(cpu, ST8);
}

static void flush_line(Merced *cpu, uint64_t pa) {
    cpu->gr_static[10] = pa;
    run_insn(cpu, FC);
}

static Merced *new_cpu(bool model) {
    memset(ram, 0, sizeof(ram));
    MercedBus bus = { .ram_size = RAM_SIZE, .read = bus_read, .write = bus_write };
    Merced *cpu = merced_create(&bus);
    if (!cpu) {
        fprintf(stderr, "merced_msr: out of memory\n");
        exit(1);
    }
    merced_set_msr_model(cpu, model);
    return cpu;
}

/* ── the 600h command port ──────────────────────────────────────────────── */

#define MSR_CTL     0x600
#define MSR_KICK    0x60D
#define MSR_BUSY    0x60E
#define MSR_ACK     0x612
#define MSR_OPERAND 0x615
#define MSR_RATIO   0x617
#define MSR_LINE0   0x400
#define MSR_MASK    0x60C

#define STATUS(ctl) (((ctl) >> 45) & 3u)

/* PAL's sequence: operand, control word, kick. */
static void kick(Merced *cpu, uint64_t operand, uint64_t ctl) {
    wrmsr(cpu, MSR_OPERAND, operand);
    wrmsr(cpu, MSR_CTL, ctl);
    wrmsr(cpu, MSR_KICK, 1);
}

static void test_storage(void) {
    Merced *cpu = new_cpu(true);
    wrmsr(cpu, 0x6A0, UINT64_C(0x123456789ABCDEF0));
    CHECK_EQ(rdmsr(cpu, 0x6A0), UINT64_C(0x123456789ABCDEF0));
    CHECK_EQ(rdmsr(cpu, 0x6A1), 0);
    /* Never written, read over and over: 0 every time (no alternation). */
    for (unsigned i = 0; i < 500; i++) {
        CHECK_EQ(rdmsr(cpu, 0x5DD), 0);
        CHECK_EQ(rdmsr(cpu, 0x616), 0);
    }
    /* Written, read over and over: still what was written. */
    for (unsigned i = 0; i < 500; i++)
        CHECK_EQ(rdmsr(cpu, 0x6A0), UINT64_C(0x123456789ABCDEF0));
    /* A direct call is the same register file. */
    merced_msr_access(cpu, true, 0x6A2, 77);
    CHECK_EQ(merced_msr_access(cpu, false, 0x6A2, 0), 77);
    CHECK_EQ(rdmsr(cpu, 0x6A2), 77);
    merced_destroy(cpu);
}

static void test_plain_model_off(void) {
    Merced *cpu = new_cpu(false);
    wrmsr(cpu, MSR_CTL, ~UINT64_C(0));
    CHECK_EQ(rdmsr(cpu, MSR_CTL), ~UINT64_C(0));        /* no read-only status */
    wrmsr(cpu, MSR_BUSY, 1);
    CHECK_EQ(rdmsr(cpu, MSR_BUSY), 1);
    for (unsigned i = 0; i < 500; i++)
        CHECK_EQ(rdmsr(cpu, 0x5DD), 0);
    merced_destroy(cpu);
}

static void test_busy(void) {
    Merced *cpu = new_cpu(true);
    CHECK_EQ(rdmsr(cpu, MSR_BUSY), 0);
    wrmsr(cpu, MSR_BUSY, ~UINT64_C(0));
    CHECK_EQ(rdmsr(cpu, MSR_BUSY), 0);
    merced_destroy(cpu);
}

static void test_status(void) {
    Merced *cpu = new_cpu(true);

    kick(cpu, 0x04000240, 0);                           /* bits 27:26 = 01 */
    CHECK_EQ(STATUS(rdmsr(cpu, MSR_CTL)), 1);
    CHECK_EQ(STATUS(rdmsr(cpu, MSR_CTL)), 1);

    kick(cpu, 0x0C000240, 0);                           /* bits 27:26 = 11 */
    CHECK_EQ(STATUS(rdmsr(cpu, MSR_CTL)), 2);           /* transitional */
    CHECK_EQ(STATUS(rdmsr(cpu, MSR_CTL)), 3);
    CHECK_EQ(STATUS(rdmsr(cpu, MSR_CTL)), 3);

    /* Status is the processor's: software cannot write it, and the rest of
     * the word reads back. */
    kick(cpu, 0x240, 0);
    CHECK_EQ(STATUS(rdmsr(cpu, MSR_CTL)), 0);
    wrmsr(cpu, MSR_CTL, ~UINT64_C(0));
    CHECK_EQ(rdmsr(cpu, MSR_CTL), ~UINT64_C(0) & ~(UINT64_C(3) << 45));
    kick(cpu, 0x04000240, 0);
    wrmsr(cpu, MSR_CTL, 0);
    CHECK_EQ(STATUS(rdmsr(cpu, MSR_CTL)), 1);

    /* Select 0x240 with bits 27:26 clear: bits 23:22 give the status. */
    kick(cpu, 0x00400240, 0);
    CHECK_EQ(STATUS(rdmsr(cpu, MSR_CTL)), 1);
    kick(cpu, 0x00C00240, 0);
    CHECK_EQ(STATUS(rdmsr(cpu, MSR_CTL)), 2);
    CHECK_EQ(STATUS(rdmsr(cpu, MSR_CTL)), 3);
    /* Bits 27:26 take precedence. */
    kick(cpu, 0x04400240, 0);
    CHECK_EQ(STATUS(rdmsr(cpu, MSR_CTL)), 1);
    /* Another select with bits 23:22 set is not a status wait. */
    kick(cpu, 0x00808100, 0);
    CHECK_EQ(STATUS(rdmsr(cpu, MSR_CTL)), 0);

    /* The kicked operand stays in 615h. */
    kick(cpu, 0x04000240, 0);
    CHECK_EQ(rdmsr(cpu, MSR_OPERAND), 0x04000240);
    merced_destroy(cpu);
}

static void test_operand_done(void) {
    Merced *cpu = new_cpu(true);
    /* Mode 2 (600h bits 4:0), select 0x240, index in 31:12: completion in
     * bit 24; bit 25 stays the operand's. */
    kick(cpu, 0x240 | (5u << 12), 2);
    CHECK_EQ(rdmsr(cpu, MSR_OPERAND), 0x240 | (5u << 12) | (UINT64_C(1) << 24));
    kick(cpu, 0x240 | (5u << 12) | (UINT64_C(1) << 25), 2);
    CHECK_EQ(rdmsr(cpu, MSR_OPERAND),
             0x240 | (5u << 12) | (UINT64_C(1) << 25) | (UINT64_C(1) << 24));
    /* Other modes, or another select, leave the operand alone. */
    kick(cpu, 0x240 | (5u << 12), 3);
    CHECK_EQ(rdmsr(cpu, MSR_OPERAND), 0x240 | (5u << 12));
    kick(cpu, 0x241 | (5u << 12), 2);
    CHECK_EQ(rdmsr(cpu, MSR_OPERAND), 0x241 | (5u << 12));
    merced_destroy(cpu);
}

static void test_ack(void) {
    Merced *cpu = new_cpu(true);
    CHECK_EQ(rdmsr(cpu, MSR_ACK), 0x40);                /* bit 9 clear: acknowledged */
    wrmsr(cpu, MSR_ACK, 0x200);
    CHECK_EQ(rdmsr(cpu, MSR_ACK), 0x200);               /* bit 9 set: bit 6 clear */
    wrmsr(cpu, MSR_ACK, 0);
    CHECK_EQ(rdmsr(cpu, MSR_ACK), 0x40);
    wrmsr(cpu, MSR_ACK, 0x240);                         /* software cannot set bit 6 */
    CHECK_EQ(rdmsr(cpu, MSR_ACK), 0x200);
    wrmsr(cpu, MSR_ACK, 0x41);
    CHECK_EQ(rdmsr(cpu, MSR_ACK), 0x41);                /* other bits are storage */
    merced_destroy(cpu);
}

static void test_ratio(void) {
    Merced *cpu = new_cpu(true);
    /* No strap latched: storage. */
    wrmsr(cpu, MSR_RATIO, 0xFFFFFFFF);
    CHECK_EQ(rdmsr(cpu, MSR_RATIO), 0xFFFFFFFF);
    merced_set_clock_ratio_code(cpu, 5);
    CHECK_EQ(rdmsr(cpu, MSR_RATIO), (0xFFFFFFFFu & ~(0xFu << 22)) | (5u << 22));
    wrmsr(cpu, MSR_RATIO, 0);
    CHECK_EQ(rdmsr(cpu, MSR_RATIO), 5u << 22);
    wrmsr(cpu, MSR_RATIO, 0xF << 22);
    CHECK_EQ(rdmsr(cpu, MSR_RATIO), 5u << 22);
    /* The strap survives RESET#, and so does the choice of model. */
    merced_reset(cpu);
    CHECK_EQ(rdmsr(cpu, MSR_RATIO), 5u << 22);
    CHECK(cpu->merced_msrs);
    merced_destroy(cpu);

    cpu = new_cpu(false);
    merced_set_clock_ratio_code(cpu, 7);
    wrmsr(cpu, MSR_RATIO, 0x1);
    CHECK_EQ(rdmsr(cpu, MSR_RATIO), 1u | (7u << 22));
    merced_destroy(cpu);
}

static void test_pmi_latch(void) {
    for (unsigned model = 0; model < 2; model++) {
        Merced *cpu = new_cpu(model);
        merced_pmi_raise(cpu, 3);
        merced_pmi_raise(cpu, 0);
        CHECK_EQ(rdmsr(cpu, 0x624), 9);
        wrmsr(cpu, 0x624, 8);                           /* write one to clear */
        CHECK_EQ(rdmsr(cpu, 0x624), 1);
        merced_destroy(cpu);
    }
}

/* ── line transfers ─────────────────────────────────────────────────────── */

static void set_line_regs(Merced *cpu, const uint64_t line[8], uint64_t mask) {
    for (unsigned w = 0; w < 8; w++)
        wrmsr(cpu, MSR_LINE0 + w, line[w]);
    wrmsr(cpu, MSR_MASK, mask);
}

static void line_write(Merced *cpu, uint64_t addr, unsigned way, unsigned state,
                       const uint64_t line[8], uint64_t mask) {
    set_line_regs(cpu, line, mask);
    kick(cpu, addr, 0x1C | (state << 5) | (way << 9));
}

static void line_read(Merced *cpu, uint64_t addr, unsigned way) {
    kick(cpu, addr, 0x0E | (way << 9));
}

static void test_line_roundtrip(void) {
    static const uint64_t line[8] = {
        0x0123456789ABCDEF, 0xFEDCBA9876543210, 0x1111111111111111, 0x2222222222222222,
        0x3333333333333333, 0x4444444444444444, 0x5555555555555555, 0x6666666666666666,
    };
    Merced *cpu = new_cpu(true);

    /* The example PAL's test runs: state 2 at address 0, read back. */
    line_write(cpu, 0, 0, 2, line, 0);
    set_line_regs(cpu, (const uint64_t[8]){0}, 0);      /* clobber the registers */
    line_read(cpu, 0, 0);
    for (unsigned w = 0; w < 8; w++)
        CHECK_EQ(rdmsr(cpu, MSR_LINE0 + w), line[w]);
    CHECK_EQ((rdmsr(cpu, MSR_CTL) >> 13) & 3u, 2);
    CHECK_EQ(rdmsr(cpu, MSR_OPERAND), 0);

    /* Each way of a set is its own line, and 615h returns the address held. */
    static const uint64_t other[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    line_write(cpu, 0x40000, 1, 3, line, 0);
    line_write(cpu, 0x40000 + (UINT64_C(1) << 20), 2, 1, other, 0);
    set_line_regs(cpu, (const uint64_t[8]){0}, 0);
    line_read(cpu, 0x40000, 1);
    CHECK_EQ(rdmsr(cpu, MSR_LINE0 + 7), line[7]);
    CHECK_EQ((rdmsr(cpu, MSR_CTL) >> 13) & 3u, 3);
    CHECK_EQ(rdmsr(cpu, MSR_OPERAND), 0x40000);
    line_read(cpu, 0x40000, 2);
    CHECK_EQ(rdmsr(cpu, MSR_LINE0), 1);
    CHECK_EQ((rdmsr(cpu, MSR_CTL) >> 13) & 3u, 1);
    CHECK_EQ(rdmsr(cpu, MSR_OPERAND), 0x40000 + (UINT64_C(1) << 20));

    /* A (set, way) holding nothing: the registers stand, state 0. */
    set_line_regs(cpu, other, 0xAA);
    line_read(cpu, 0x41000, 3);
    CHECK_EQ(rdmsr(cpu, MSR_LINE0), 1);
    CHECK_EQ(rdmsr(cpu, MSR_MASK), 0xAA);
    CHECK_EQ((rdmsr(cpu, MSR_CTL) >> 13) & 3u, 0);
    merced_destroy(cpu);
}

static void test_line_visible_to_loads(void) {
    static const uint64_t line[8] = { 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7 };
    Merced *cpu = new_cpu(true);
    const uint64_t pa = 0x42000;
    for (unsigned w = 0; w < 8; w++)
        memcpy(ram + pa + 8 * w, &(uint64_t){0xBAD0 + w}, 8);

    CHECK(!cpu->cache.observe);
    CHECK_EQ(load8(cpu, pa + 16), 0xBAD2);
    line_write(cpu, pa, 0, 1, line, 0);                 /* state 1: resident */
    CHECK(cpu->cache.observe);
    for (unsigned w = 0; w < 8; w++)
        CHECK_EQ(load8(cpu, pa + 8 * w), line[w]);
    CHECK_EQ(ram_u64(pa), 0xBAD0);                      /* memory is untouched */

    /* A store to the resident line updates it, not memory. */
    store8(cpu, pa + 8, 0x1234);
    CHECK_EQ(load8(cpu, pa + 8), 0x1234);
    CHECK_EQ(ram_u64(pa + 8), 0xBAD1);

    /* The sequence terminator (operand 8140h) drops injected lines. */
    wrmsr(cpu, MSR_OPERAND, 0x8140);
    wrmsr(cpu, MSR_CTL, 0);
    wrmsr(cpu, MSR_KICK, 1);
    set_line_regs(cpu, (const uint64_t[8]){0}, 0);
    line_read(cpu, pa, 0);
    CHECK_EQ(rdmsr(cpu, MSR_LINE0), 0);                 /* nothing there */
    CHECK_EQ((rdmsr(cpu, MSR_CTL) >> 13) & 3u, 0);
    for (unsigned w = 0; w < 8; w++)
        CHECK_EQ(load8(cpu, pa + 8 * w), 0xBAD0 + w);
    merced_destroy(cpu);
}

/* ── L2 arrays ──────────────────────────────────────────────────────────── */

#define MSR_L2_TAG   0x5E0
#define MSR_L2_DATA  0x5E1
#define MSR_L2_STATE 0x5E2
#define MSR_L2_LOG   0x5EA

/* SEC-DED over a qword, as PAL's own check-bit routine computes it. */
static uint8_t ecc_of(uint64_t d) {
    static const uint64_t mask[8] = {
        UINT64_C(0x8383838707070787), UINT64_C(0x0C8C8D8919199999),
        UINT64_C(0x1515B4B020A2AAAB), UINT64_C(0x30331E9ACAC8C1C0),
        UINT64_C(0xAB6C405082A9A4F4), UINT64_C(0xC6A85062ADD4D022),
        UINT64_C(0xE8D0A24DF084720C), UINT64_C(0xD0C2E9A4D4720C50),
    };
    uint8_t e = 0;
    for (unsigned i = 0; i < 8; i++)
        e |= (uint8_t)((__builtin_popcountll(d & mask[i]) & 1) << i);
    return e;
}

/* The command word every L2 port shares: set 10:0, way 13:11, sub-field
 * 18:14, command byte 31:24, payload from bit 32. */
static uint64_t l2_cmd(unsigned set, unsigned way, unsigned sub, unsigned cmd,
                       uint64_t payload) {
    return set | ((uint64_t)way << 11) | ((uint64_t)sub << 14) |
           ((uint64_t)cmd << 24) | (payload << 32);
}

/* PA = tag << 14 | set << 6; the data is the line's eight qwords. */
static void l2_write_line(Merced *cpu, unsigned set, unsigned way, uint32_t tag,
                          bool good_parity, unsigned state, const uint64_t data[8]) {
    for (unsigned k = 0; k < 32; k++) {
        const unsigned q = k / 4, i = k % 4;
        const uint64_t chunk = (data[q] >> (16 * i)) & 0xFFFF;
        const uint64_t ecc2 = (ecc_of(data[q]) >> (2 * i)) & 3u;
        wrmsr(cpu, MSR_L2_DATA, l2_cmd(set, way, k, 0x40, chunk | (ecc2 << 16)));
    }
    const bool parity = (__builtin_popcount(tag) & 1) != 0;
    wrmsr(cpu, MSR_L2_TAG, l2_cmd(set, way, 0, 0x10, tag & 0x7FFF));
    wrmsr(cpu, MSR_L2_TAG, l2_cmd(set, way, 1, 0x10,
                                  ((tag >> 15) & 0x7FFF) |
                                  ((uint64_t)(parity == good_parity) << 17)));
    wrmsr(cpu, MSR_L2_STATE, l2_cmd(set, way, 0, 0x40, (uint64_t)state << 6));
}

/* The state in a way's state word (5E2h read: command byte 0x30). */
static unsigned l2_state(Merced *cpu, unsigned set, unsigned way) {
    wrmsr(cpu, MSR_L2_STATE, l2_cmd(set, way, 0, 0x30, 0));
    return (unsigned)((rdmsr(cpu, MSR_L2_STATE) >> 38) & 7u);
}

static void test_l2_hit(void) {
    const uint64_t pa = 0x41048;                        /* set 0x41, tag 0x10, qword 1 */
    const unsigned set = (pa >> 6) & 0xFF, way = 3;
    const uint32_t tag = (uint32_t)(pa >> 14);
    uint64_t data[8];
    Merced *cpu = new_cpu(true);
    for (unsigned q = 0; q < 8; q++) {
        data[q] = UINT64_C(0xC0DE000000000000) | (q << 8) | q;
        const uint64_t mem = UINT64_C(0xFEED000000000000) | q;
        memcpy(ram + (pa & ~UINT64_C(63)) + 8 * q, &mem, 8);
    }

    CHECK_EQ(load8(cpu, pa), UINT64_C(0xFEED000000000001));     /* memory: no line */
    l2_write_line(cpu, set, way, tag, true, 3, data);           /* clean */
    CHECK(cpu->cache.observe);
    CHECK_EQ(load8(cpu, pa), data[1]);                          /* an L2 hit */
    CHECK_EQ(load8(cpu, (pa & ~UINT64_C(63)) + 56), data[7]);
    CHECK_EQ(l2_state(cpu, set, way), 3);
    CHECK(!(cpu->psr & PSR_MC_BIT));
    CHECK_EQ(cpu->nfaults, 0);

    /* A store stays in the write-back L2 and the clean line turns modified. */
    store8(cpu, pa, 0x5555);
    CHECK_EQ(load8(cpu, pa), 0x5555);
    CHECK_EQ(ram_u64(pa), UINT64_C(0xFEED000000000001));
    CHECK_EQ(l2_state(cpu, set, way), 2);

    /* fc writes the modified line back and drops it. */
    flush_line(cpu, pa);
    CHECK_EQ(ram_u64(pa), 0x5555);
    CHECK_EQ(ram_u64((pa & ~UINT64_C(63)) + 56), data[7]);
    CHECK_EQ(l2_state(cpu, set, way), 0);
    CHECK_EQ(load8(cpu, pa), 0x5555);                           /* memory now */
    CHECK(!cpu->cache.observe);

    /* An uncacheable access (physical bit 63) bypasses the line. */
    l2_write_line(cpu, set, way, tag, true, 3, data);
    CHECK_EQ(load8(cpu, pa | (UINT64_C(1) << 63)), 0x5555);
    CHECK_EQ(load8(cpu, pa), data[1]);
    merced_destroy(cpu);
}

/* A line the model filled while following the L2 (invalidated by 5E5h bit 17
 * and then loaded from) is a plain cache line: a flush of another address
 * that maps to the same set must leave it, and its stores, alone. PAL's
 * top-of-memory words are kept this way when it flushes the neighbouring
 * line of a set. */
static void test_fc_leaves_filled_line(void) {
    const uint64_t a = 0x0F7180;                        /* set 0xC6, tag 0x3D */
    const uint64_t b = 0x1FF180;                        /* the same set, another tag */
    Merced *cpu = new_cpu(true);
    const uint64_t mem = UINT64_C(0x1111111111111111);
    memcpy(ram + a, &mem, 8);
    wrmsr(cpu, 0x5E5, UINT64_C(1) << 17);               /* invalidate: fills are followed */
    wrmsr(cpu, 0x5E5, 0);
    CHECK_EQ(load8(cpu, a), mem);                       /* fills the line */
    store8(cpu, a + 8, 0x2222);                         /* modified in the L2, not in memory */
    CHECK_EQ(ram_u64(a + 8), 0);
    flush_line(cpu, b);                                 /* a different line of the set */
    CHECK_EQ(load8(cpu, a + 8), 0x2222);                /* still there */
    CHECK_EQ(ram_u64(a + 8), 0);
    flush_line(cpu, a);                                 /* its own flush writes it back */
    CHECK_EQ(ram_u64(a + 8), 0x2222);
    merced_destroy(cpu);
}

static void test_l2_data_ecc(void) {
    const uint64_t pa = 0x41048;
    const unsigned set = (pa >> 6) & 0xFF, way = 3;
    uint64_t data[8] = { 0, UINT64_C(0xD60961096B777731), 0, 0, 0, 0, 0, 0 };
    Merced *cpu = new_cpu(true);
    cpu->cr[CR_CMCV] = UINT64_C(1) << 16;               /* masked: a machine check */

    /* Flip one data bit after the check bits were computed: the load comes
     * back corrected and the error is logged. */
    l2_write_line(cpu, set, way, (uint32_t)(pa >> 14), true, 3, data);
    wrmsr(cpu, MSR_L2_DATA, l2_cmd(set, way, 4, 0x40,
                                   ((data[1] & 0xFFFF) ^ 0x10) |
                                   ((uint64_t)((ecc_of(data[1]) >> 0) & 3u) << 16)));
    CHECK_EQ(load8(cpu, pa), data[1]);
    CHECK_EQ(cpu->ip, PALE_CHECK);
    CHECK_EQ(cpu->cr[CR_IIP], CODE);
    CHECK_EQ(cpu->msr[MSR_L2_LOG],
             UINT64_C(0x1004080) | 1 | 2 | (UINT64_C(1) << 19) |
             ((uint64_t)set << 25) | ((uint64_t)way << 33));
    merced_destroy(cpu);
}

static void test_l2_tag_parity_mca(void) {
    const uint64_t pa = 0x41048;
    const unsigned set = (pa >> 6) & 0xFF, way = 2;
    uint64_t data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    Merced *cpu = new_cpu(true);
    const uint32_t tag = (uint32_t)(pa >> 14);

    ram[pa] = 0x77;
    l2_write_line(cpu, set, way, tag, false, 3, data);          /* bad parity */
    CHECK(cpu->cache.observe);
    cpu->psr |= UINT64_C(1) << 13;                              /* IC: IPSR is saved */

    /* A load of the set is a machine check into PALE_CHECK, with IIP on the
     * load and the cause in 5EAh. */
    const uint64_t psr = cpu->psr;
    load8(cpu, pa);
    CHECK_EQ(cpu->ip, PALE_CHECK);
    CHECK(cpu->psr & PSR_MC_BIT);
    CHECK_EQ(cpu->cr[CR_IIP], CODE);
    CHECK_EQ(cpu->cr[16], psr);                                 /* IPSR */
    CHECK_EQ(cpu->msr[MSR_L2_LOG],
             UINT64_C(0x1002282) | 1 | (UINT64_C(1) << 19) | (1u << 10) |  /* load, tag matched */
             ((uint64_t)set << 25) | ((uint64_t)way << 33));

    /* PSR.mc holds further machine checks off: the handler's own accesses
     * reach memory. */
    CHECK_EQ(load8(cpu, pa), 0x77);
    CHECK_EQ(cpu->nfaults, 0);
    cpu->psr &= ~PSR_MC_BIT;

    /* A store does not complete either: neither the line nor memory change. */
    store8(cpu, pa, 0x99);
    CHECK_EQ(cpu->ip, PALE_CHECK);
    CHECK_EQ(ram[pa], 0x77);
    cpu->psr &= ~PSR_MC_BIT;

    /* A good tag in the way clears the condition. */
    l2_write_line(cpu, set, way, tag, true, 3, data);
    CHECK_EQ(load8(cpu, pa), data[1]);
    CHECK_EQ(cpu->ip, CODE + 16);
    merced_destroy(cpu);
}

static void test_l2_fetch(void) {
    /* A bundle left in the (unified) L2 is what a cacheable fetch executes. */
    const uint64_t code = 0x44000;                      /* set 0, tag 0x11 */
    uint64_t line[8] = { 0 };
    uint8_t bundle[16];
    Merced *cpu = new_cpu(true);

    put_bundle(bundle, 0x00, ADDS_R8(0x55), NOP_MI, NOP_MI);
    memcpy(&line[0], bundle, 16);
    put_bundle(ram + code, 0x00, ADDS_R8(0x11), NOP_MI, NOP_MI);   /* memory's */
    l2_write_line(cpu, 0, 1, (uint32_t)(code >> 14), true, 3, line);

    run_bundle(cpu, code);
    CHECK_EQ(cpu->gr_static[8], 0x55);

    /* Once the line is gone memory's bundle runs, whatever was cached. */
    flush_line(cpu, code);
    run_bundle(cpu, code);
    CHECK_EQ(cpu->gr_static[8], 0x11);
    merced_destroy(cpu);
}

/* ── L1 instruction cache fills ─────────────────────────────────────────── */

#define MSR_IC_CTL 0x420
#define MSR_IC_TAG 0x421

/* After an invalidate with the cache enabled, cacheable fetches fill it: the
 * k-th line of a set lands in way 0, 2, 1, 3 (tree pseudo-LRU from state 0),
 * and a tag read returns the line's tag with the valid bit. */
static void test_icache_fill_order(void) {
    static const unsigned way_of[4] = { 0, 2, 1, 3 };
    Merced *cpu = new_cpu(true);
    wrmsr(cpu, MSR_IC_CTL, 1);                          /* 0 -> 1: invalidate */
    CHECK(cpu->cache.ic_track);
    for (unsigned i = 0; i < 4; i++) {
        const uint64_t at = 0x30020 + i * 0x1000;       /* set 1, tags 0x30.. */
        put_bundle(ram + at, 0x00, NOP_MI, NOP_MI, NOP_MI);
        run_bundle(cpu, at);
        CHECK_EQ(cpu->ip, at + 16);
    }
    for (unsigned i = 0; i < 4; i++) {
        wrmsr(cpu, MSR_IC_TAG, 1 | ((uint64_t)way_of[i] << 11) | (UINT64_C(0x30) << 24));
        CHECK_EQ((rdmsr(cpu, MSR_IC_TAG) >> 32) & 0x1FFFF, (UINT64_C(1) << 16) | (0x30 + i));
    }
    /* Disabling the cache (bit 1) stops following fills. */
    wrmsr(cpu, MSR_IC_CTL, 2);
    CHECK(!cpu->cache.ic_track);
    merced_destroy(cpu);
}

int main(void) {
    test_storage();
    test_plain_model_off();
    test_busy();
    test_status();
    test_operand_done();
    test_ack();
    test_ratio();
    test_pmi_latch();
    test_line_roundtrip();
    test_line_visible_to_loads();
    test_l2_hit();
    test_fc_leaves_filled_line();
    test_l2_data_ecc();
    test_l2_tag_parity_mca();
    test_l2_fetch();
    test_icache_fill_order();
    if (failures) {
        fprintf(stderr, "merced_msr: %u check(s) failed\n", failures);
        return 1;
    }
    printf("merced_msr: all checks passed\n");
    return 0;
}
