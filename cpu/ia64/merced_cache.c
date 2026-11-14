/* merced_cache.c -- the Merced (Itanium) cache arrays behind PAL's
 * array-access MSRs, as the B1+ self-tests drive them.
 *
 * Undocumented; derived from fw130 (HP i2000 / Intel SDV) PAL_B.  A-step
 * parts skip these tests (fw130 0xFFDF44F0 gates the B1+ reset path on the
 * CPUID revision); every later stepping runs them at reset.
 *
 * Every failing array test sets one bit of msr[0x684] (PAL's unit-disable
 * record) and SAL's PAL_PROC_SET_FEATURES then refuses to re-enable the unit
 * (helpers 0xFFDC0290/0310/0390 test bits 51/52/53), hanging SAL at
 * 0xFFEA3A80. So these arrays must hold what PAL writes.
 *
 * L2 (test 0xFFE04E40 -> 0xFFE04A20, failure bit 53). 96 KB, 6 ways, 64-byte
 * lines: 256 sets, 16 KB per way. The test builds an address from its own
 * code page, PA = (IP >> 14) << 14 | set << 6, so set = PA 13:6 and the tag
 * is PA >> 14 (30 bits). The command word every port shares is built by
 * 0xFFDC7980: set in 10:0, way in 13:11, a sub-field in 18:14 for a write
 * (23:19 for a read), a command byte in 31:24 (or 63:56; see cmd_is_read),
 * payload from bit 32.
 *   5E1h data (0xFFDC8550 loop): sub-field = 16-bit chunk 0..31 (qword
 *        w's bits 16k+15:16k are chunk 4w+k), payload 47:32 = the chunk,
 *        49:48 = two of the qword's eight ECC bits (0xFFDC9FE0).
 *   5E0h tag (0xFFDC7B20): sub-field 0 carries tag 14:0 in 46:32, 1 carries
 *        tag 29:15 and parity in 49.
 *   5E2h the line's state word (0xFFDC7D00), payload 9:0 with the state in
 *        8:6: 0 invalid, 3 clean, 2 modified. A line is valid while its
 *        state is non-zero.
 * The test loads one qword of the line it has just written and requires the
 * pattern -- an L2 hit -- for every way of sets 0, 7Fh, 80h and FFh.
 * WHEN. Sub-test 1 brackets its loop with msr[0x580] = 0x90000000000 (bits
 * 43 and 40), sub-test 2 (0xFFE04120) with bit 40 alone; normal operation
 * has 15:10 set and both clear. INFERRED: while bit 40 is set the L2 serves
 * accesses from its arrays; outside it PAL has reinitialised the cache
 * before anything relies on it, so memory is what an access sees.
 *
 * ERRORS (sub-test 2, 0xFFE04120). Tag part 1 carries the tag's parity in
 * 49 (PAL computes popcount(tag) & 1, 0xFFE041D0; the test flips it, or
 * flips one tag bit and keeps the old parity). 5E2h op 2 (0xFFDC7D00) puts
 * a 3-bit state in payload 8:6 = word 40:38: 3 clean, 2 modified (the
 * modified cases expect the uncorrected status). An access to a set
 * holding a valid way with bad tag parity is a machine check (the test
 * also writes msr[0x616] = 0x3C0, but the data-ECC test 0xFFE130C0 expects
 * one with 616h clear, so that register does not gate it):
 * PALE_CHECK (0xFFFFFF90) -> 0xFFFF8280
 * matches the test signature in msr[0x684] -> 0xFFDF32D0, which returns
 * 5EAh bits 0/2/3 in r9 16/17/18 and the faulting IIP in r8. The test then
 * reads 5EAh (0xFFE043B0) and expects, its six phases agreeing:
 *   bit 0 valid, 2 uncorrected (= modified line), 10 the bad way's tag
 *   matched the access, 12 the line was modified, 21:19 the access (001
 *   load, 110 store), 32:25 set, 35:33 way, and 24/13/9/7/1 always set.
 * When several ways are bad the one whose tag was written last is reported
 * (the test leaves an earlier way's flipped tag behind and only that rule
 * gives the way and state it expects). A store with an error does not
 * complete.
 *
 * LIVE LINES. The phase-2 data-ECC test (0xFFE130C0) plants lines with the
 * L2 in normal operation (580h = 0) and loads them, so any valid way is
 * visible to accesses, not only in test mode. Its loads come back corrected
 * (SEC-DED below), the error is logged in 5EAh for the bundle's first
 * memory op and 5EBh for its second, and the machine check is raised. A
 * store to a resident line updates the line (write-back), not memory.
 */
#include "merced.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PSR.mc: machine checks are masked; PALE_CHECK runs with it set. */
#define PSR_MC (UINT64_C(1) << 35)
/* CMCV, the corrected machine check vector (masked in bit 16). */
#define CR_CMCV 74

static unsigned plru_victim(uint8_t s);
static uint8_t plru_touch(uint8_t s, unsigned way);

/* Guest time in bundles, for the fill windows below: an interpreted slot is a
 * third of a bundle. */
static uint64_t now_bundles(const Merced *m)
{
    return m->ninsts / 3u;
}

#define MSR_L2_TAG   0x5E0u
#define MSR_L2_DATA  0x5E1u
#define MSR_L2_STATE 0x5E2u
#define MSR_L2_LOG   0x5EAu
#define MSR_L2_MODE  0x580u
/* Bit 40 alone (sub-test 2), 43 + 40 (sub-test 1) and 43 + 10 (phase-2
 * test 0xFFE149F0) all expect accesses served by the arrays. */
#define L2_TESTMODE  ((UINT64_C(1) << 40) | (UINT64_C(1) << 43))

static unsigned way_state(const MercedL2Way *w) { return (w->stword >> 6) & 7u; }
static bool way_valid(const MercedL2Way *w) { return w->tag_lo && w->tag_hi && way_state(w) != 0; }

#define LOG_VALID    (UINT64_C(1) << 0)
#define LOG_UNCORR   (UINT64_C(1) << 2)
#define LOG_CMC_VALID (UINT64_C(1) << 4)
#define LOG_HIT      (UINT64_C(1) << 10)
#define LOG_MODIFIED (UINT64_C(1) << 12)
#define LOG_LOAD     (UINT64_C(1) << 19)
#define LOG_STORE    (UINT64_C(3) << 20)
#define LOG_FIXED    UINT64_C(0x1002282)   /* tag parity: 24, 13, 9, 7, 1 */
/* Data ECC (phase-2 test 0xFFE130C0): 0x601084082 for a load and
 * 0x601304080 for a store, set in 32:25 and way in 35:33 (way 3 there):
 * 24, 14, 7 always, bit 1 only for a load. */
#define LOG_DATA     UINT64_C(0x1004080)
#define LOG_DATA_LD  (UINT64_C(1) << 1)
#define LOG_DATA_UE  UINT64_C(0x1010080)   /* uncorrectable: 24, 16, 7 */
#define LOG_WRITEBACK (UINT64_C(1) << 22)
#define MSR_L2_LOG1  0x5EBu

/* SEC-DED over a qword (fw130 0xFFDC9FE0, the test's own check-bit routine):
 * check bit i = parity(data & g_ecc_mask[i]). Every column is distinct and
 * of odd weight, so a one-bit syndrome is a check-bit error and a syndrome
 * equal to a column the data bit of that column. Verified: the bytes of the
 * test's 0xD60961096B777731 are these bits of its eight patterns. */
static const uint64_t g_ecc_mask[8] = {
    UINT64_C(0x8383838707070787), UINT64_C(0x0C8C8D8919199999),
    UINT64_C(0x1515B4B020A2AAAB), UINT64_C(0x30331E9ACAC8C1C0),
    UINT64_C(0xAB6C405082A9A4F4), UINT64_C(0xC6A85062ADD4D022),
    UINT64_C(0xE8D0A24DF084720C), UINT64_C(0xD0C2E9A4D4720C50),
};

static uint8_t ecc_of(uint64_t d)
{
    uint8_t e = 0;
    for (unsigned i = 0; i < 8; i++) { e |= (uint8_t)((__builtin_popcountll(d & g_ecc_mask[i]) & 1) << i); }
    return e;
}

/* The system-bus check byte of a qword (fw130 0xFFDC8720): parity of the
 * physical address bits 43:19 and of the data, each through eight masks,
 * XORed. Phase-2 test 0xFFE15840 supplies a line's eight bytes through
 * msr[0x60C] with one bit flipped and expects the load to correct it. */
static const uint32_t g_bus_amask[8] = {
    0x11C7777u, 0x1E07BBBu, 0x11F8DDDu, 0x0E38EEEu,
    0x17771C7u, 0x0BBBE07u, 0x0DDD1F8u, 0x1EEEE38u,
};
static const uint64_t g_bus_dmask[8] = {
    UINT64_C(0x4DFF0F00E2111111), UINT64_C(0xC5F0F0F0B2222222),
    UINT64_C(0x4F0FF00F92444444), UINT64_C(0x49000FFFB3888888),
    UINT64_C(0xE21111114DFF0F00), UINT64_C(0xB2222222C5F0F0F0),
    UINT64_C(0x924444444F0FF00F), UINT64_C(0xB388888849000FFF),
};

static uint8_t bus_ecc_of(uint64_t pa, uint64_t d)
{
    const uint32_t a = (uint32_t)((pa >> 19) & 0x1FFFFFFu);
    uint8_t e = 0;
    for (unsigned i = 0; i < 8; i++) {
        e |= (uint8_t)(((__builtin_popcount(a & g_bus_amask[i]) ^
                         __builtin_popcountll(d & g_bus_dmask[i])) & 1) << i);
    }
    return e;
}

static unsigned cmd_set(uint64_t v)   { return (unsigned)(v & 0x7FFu) % MC_L2_SETS; }
static unsigned cmd_way(uint64_t v)   { return (unsigned)((v >> 11) & 7u); }
static unsigned cmd_sub(uint64_t v)   { return (unsigned)((v >> 14) & 0x1Fu); }
/* The command byte (31:24, or 63:56 for the other field set) holds 2-bit
 * opcodes: 01 writes (data 0x40, tags 0x10 / 0x40), 11 reads (tag reads
 * 0x03 / 0x3084000 in fw130's flush routine 0xFFDC0480, state reads 0x30,
 * 0xC0, 0xC3 in 0xFFDC0480 / 0xFFDC7D00). A read leaves the array's content
 * in the register for the mov-from-msr that follows. */
static bool cmd_is_read(uint64_t v)
{
    const unsigned b = (unsigned)(((v >> 24) | (v >> 56)) & 0xFFu);
    for (unsigned f = 0; f < 8; f += 2) {
        if (((b >> f) & 3u) == 3u) { return true; }
    }
    return false;
}

/* Every access is observed while the L2 is in test mode or lines written
 * through the ports are resident (update_observe). */
/* L1D fills are followed (every cacheable load comes here) from an
 * invalidate while the L1D is enabled, for a bounded number of loads; see
 * the L1D section. */
#define L1D_DISABLE      (UINT64_C(0x3F) << 10)
#define L1D_TRACK_LOADS  65536u
static bool l1d_tracking(const MercedCache *mc)
{
    return mc->l1d_known && !(mc->l2mode & L1D_DISABLE) && mc->l1d_loads < L1D_TRACK_LOADS;
}

/* The same for L3 fills, from the 8140h flush (the L3 is then empty). */
#define L3_TRACK_LOADS  65536u
static bool l3_tracking(const MercedCache *mc)
{
    return mc->l3_known && mc->l3_loads < L3_TRACK_LOADS;
}

/* And for the L2, from its invalidate while it is enabled (0xFFDC0390
 * clears 5E4h bits 6 and 2 to enable it). */
#define L2_DISABLE         ((UINT64_C(1) << 6) | (UINT64_C(1) << 2))
#define L2_TRACK_ACCESSES  65536u
static bool l2_tracking(const MercedCache *mc)
{
    return mc->l2_known && !(mc->l2ctl & L2_DISABLE) && !(mc->l2inv & (UINT64_C(1) << 17)) &&
           mc->l2_accesses < L2_TRACK_ACCESSES;
}

static void update_observe(Merced *m, const MercedCache *mc)
{
    const bool on = mc->testmode || mc->nl3_planted != 0 || mc->nvalid != 0 ||
                    mc->nplanted != 0 || mc->nl1d_planted != 0 || mc->nstate != 0 ||
                    l1d_tracking(mc) || l2_tracking(mc) || l3_tracking(mc);
    m->cache.observe = on;
}

static unsigned l3_set(uint64_t pa) { return (unsigned)(pa >> 6) & (MC_L3_SETS - 1u); }

static void l3_recount(Merced *m, MercedCache *mc)
{
    unsigned n = 0, p = 0;
    for (unsigned i = 0; i < MC_INJ_LINES; i++) {
        const MercedL3Line *l = &mc->inj[i];
        if (!l->used) { continue; }
        n++;
        if (l->planted && l->state != 0u) { p++; }
    }
    mc->ninj = n;
    mc->nl3_planted = p;
    mc->l3_dirty = true;         /* every change to a line passes here */
    update_observe(m, mc);
}

/* The line held in (set, way), if any. */
static MercedL3Line *l3_at(MercedCache *mc, unsigned set, unsigned way)
{
    for (unsigned i = 0; mc->ninj && i < MC_INJ_LINES; i++) {
        MercedL3Line *l = &mc->inj[i];
        if (l->used && l->way == way && l3_set(l->pa) == set) { return l; }
    }
    return NULL;
}

/* A slot for a line in (set, way): the one there, else a free one, else the
 * oldest. */
static MercedL3Line *l3_slot(MercedCache *mc, unsigned set, unsigned way)
{
    MercedL3Line *l = l3_at(mc, set, way);
    if (l) { return l; }
    MercedL3Line *oldest = &mc->inj[0];
    for (unsigned i = 0; i < MC_INJ_LINES; i++) {
        if (!mc->inj[i].used) { return &mc->inj[i]; }
        if (mc->inj[i].seq < oldest->seq) { oldest = &mc->inj[i]; }
    }
    return oldest;
}

/* A filled line's words are memory's (it was a cacheable load). */
static void l3_words(Merced *m, const MercedL3Line *l, uint64_t data[8])
{
    if (l->planted) { memcpy(data, l->data, sizeof l->data); return; }
    for (unsigned q = 0; q < 8; q++) { data[q] = merced_bus_read(m, l->pa + 8u * q, 8); }
}

void merced_l3_line_write(Merced *m, uint64_t addr, unsigned way, const uint64_t data[8],
                          uint64_t mask, bool ecc_given, unsigned state)
{
    MercedCache *mc = &m->cache;
    const uint64_t pa = addr & ~(UINT64_C(1) << 63) & ~UINT64_C(63);
    MercedL3Line *l = l3_slot(mc, l3_set(pa), way);
    l->used = true;
    l->planted = true;
    l->pa = pa;
    l->addr = addr;
    l->way = way;
    l->state = state;
    l->mask = mask;
    l->seq = ++mc->seq;
    memcpy(l->data, data, sizeof l->data);
    for (unsigned q = 0; q < 8; q++) {
        l->ecc[q] = ecc_given ? (uint8_t)(mask >> (8u * q)) : bus_ecc_of(pa, data[q]);
    }
    l3_recount(m, mc);
}

bool merced_l3_line_read(Merced *m, uint64_t addr, unsigned way, uint64_t data[8],
                         uint64_t *mask, unsigned *state, uint64_t *line_addr)
{
    MercedCache *mc = &m->cache;
    const MercedL3Line *l = l3_at(mc, l3_set(addr & ~(UINT64_C(1) << 63)), way);
    if (!l) { return false; }
    *line_addr = l->addr;
    l3_words(m, l, data);
    uint64_t ecc = 0;
    for (unsigned q = 0; q < 8; q++) {
        ecc |= (uint64_t)(l->planted ? l->ecc[q] : bus_ecc_of(l->pa, data[q])) << (8u * q);
    }
    *mask = l->planted ? l->mask : ecc;
    *state = l->state;
    return true;
}

#define MSR_BUS_ADDR  0x603u
#define MSR_BUS_STAT  0x604u
#define BUS_STAT_DATA UINT64_C(0x120)    /* what 0xFFE15840 reads back */
#define BUS_ADDR_VALID (UINT64_C(1) << 44)

/* Check an injected line's bus ECC on access: correct one bit, log the
 * line's address in 603h and the status in 604h, and signal it -- a CMC
 * interrupt (status bit 2, which 0xFFDB3000 tests) when CMCV is unmasked,
 * else a machine check (bit 0). INFERRED for anything the test does not
 * exercise: an uncorrectable syndrome is left uncorrected and signalled as
 * a machine check. */
static void check_injected(Merced *m, MercedCache *mc, MercedL3Line *l)
{
    for (unsigned q = 0; q < 8; q++) {
        uint8_t syn = (uint8_t)(bus_ecc_of(l->pa, l->data[q]) ^ l->ecc[q]);
        if (!syn) { continue; }
        bool corrected = __builtin_popcount(syn) == 1;
        for (unsigned j = 0; !corrected && j < 64; j++) {
            uint8_t col = 0;
            for (unsigned i = 0; i < 8; i++) { col |= (uint8_t)(((g_bus_dmask[i] >> j) & 1u) << i); }
            if (col == syn) { l->data[q] ^= UINT64_C(1) << j; corrected = true; }
        }
        if (corrected) { l->ecc[q] = bus_ecc_of(l->pa, l->data[q]); }
        m->msr[MSR_BUS_ADDR] = l->pa | BUS_ADDR_VALID;
        m->msr_written[MSR_BUS_ADDR] = true;
        m->msr_written[MSR_BUS_STAT] = true;
        const uint64_t cmcv = m->cr[CR_CMCV];
        if (corrected && !((cmcv >> 16) & 1u)) {
            m->msr[MSR_BUS_STAT] = BUS_STAT_DATA | 4u;
            merced_raise_external(m, (uint8_t)(cmcv & 0xFFu));
        } else {
            m->msr[MSR_BUS_STAT] = BUS_STAT_DATA | 1u;
            mc->mca_pending = true;
        }
        return;
    }
}

/* The port's sequence terminator (merced_msr.c, operand 8140h). Every test
 * uses the lines it planted before one, and PAL runs a command-B sweep
 * ending in one between its last L2 test and handing off to SAL, whose
 * cacheable accesses would otherwise meet the bad-parity tags the error
 * test leaves behind (a machine check at SAL 0xFFFF3640). INFERRED: it
 * flushes and invalidates; the arrays keep their contents, the lines are
 * no longer valid. */
void merced_cache_drop_injected(Merced *m)
{
    MercedCache *mc = &m->cache;
    /* No write-back: 0xFFE0E100 leaves modified lines in the L3 (an L2
     * victim's data, state 3), runs this teardown and expects memory to
     * still hold what it held before. */
    /* The L3 is empty now, so its fills can be followed (0xFFE0E100). Its
     * replacement state restarts too: that test flushes, loads lines of
     * sets it used before and reads them back in way 0. */
    mc->l3_known = true;
    mc->l3_t0 = now_bundles(m);
    mc->l3_loads = 0;
    memset(mc->l3_plru, 0, sizeof mc->l3_plru);
    memset(mc->inj, 0, sizeof mc->inj);
    mc->ninj = 0;
    mc->nl3_planted = 0;
    mc->l3_dirty = true;
    for (unsigned s = 0; s < MC_L2_SETS; s++) {
        for (unsigned w = 0; w < MC_L2_WAYS; w++) { mc->l2[s][w].stword = 0; mc->l2[s][w].modified = false; }
    }
    mc->nvalid = 0;
    mc->nstate = 0;
    update_observe(m, mc);
}

#define MSR_IC_CTL   0x420u
#define MSR_IC_TAG   0x421u
#define MSR_IC_DATA  0x422u
#define MSR_IC_LRU   0x423u
#define MSR_IC_LOG   0x425u
static void icache_msr_write(Merced *m, unsigned mi, uint64_t v);
#define MSR_L2_CTL    0x5E4u
#define MSR_L2_INV    0x5E5u
#define L2_INVAL      (UINT64_C(1) << 17)   /* 0xFFDC0230 pulses it */
static void l2_invalidate_all(Merced *m, MercedCache *mc);
#define MSR_L1D_TAG   0x5D0u
#define MSR_L1D_VALID 0x5D1u
#define MSR_L1D_LRU   0x5D2u
#define MSR_L1D_WP    0x5D3u
#define MSR_L1D_DATA  0x5D8u
#define L1D_INVAL     (UINT64_C(1) << 40)
static void l1d_invalidate_all(Merced *m, MercedCache *mc);
static bool l1d_valid(const MercedCache *mc, unsigned set, unsigned way);
static void l1d_recount(Merced *m, MercedCache *mc);
static void l1d_msr_write(Merced *m, unsigned mi, uint64_t v);
static int  l1d_load(Merced *m, MercedCache *mc, uint64_t pa, unsigned size, uint64_t *raw);
static void l1d_store(MercedCache *mc, uint64_t pa, unsigned size, uint64_t raw);

void merced_cache_msr_write(Merced *m, unsigned mi, uint64_t v)
{
    switch (mi) {
    case MSR_IC_CTL:
    case MSR_IC_TAG:
    case MSR_IC_DATA:
    case MSR_IC_LRU:
        icache_msr_write(m, mi, v);
        break;
    case MSR_L2_MODE: {
        const bool on = (v & L2_TESTMODE) != 0;
        MercedCache *mc = &m->cache;   /* the L1D needs the mode too */
        mc->testmode = on;
        mc->l2mode = v;
        if (v & L1D_INVAL) { l1d_invalidate_all(m, mc); }
        update_observe(m, mc);
        break;
    }
    case MSR_L2_CTL: {
        MercedCache *mc = &m->cache;
        mc->l2ctl = v;
        update_observe(m, mc);
        break;
    }
    case MSR_L2_INV: {
        MercedCache *mc = &m->cache;
        const bool rise = ((v & L2_INVAL) != 0) && !(mc->l2inv & L2_INVAL);
        mc->l2inv = v;
        if (rise) { l2_invalidate_all(m, mc); }
        update_observe(m, mc);
        break;
    }
    case MSR_L1D_TAG:
    case MSR_L1D_VALID:
    case MSR_L1D_LRU:
    case MSR_L1D_WP:
    case MSR_L1D_DATA:
        l1d_msr_write(m, mi, v);
        break;
    case MSR_L2_DATA: {
        MercedCache *mc = &m->cache;
        const unsigned way = cmd_way(v);
        if (way >= MC_L2_WAYS) { break; }
        MercedL2Way *w = &mc->l2[cmd_set(v)][way];
        if (cmd_is_read(v)) {   /* chunk in 19:23 (0xFFDC7980 with r32 = 0) */
            const unsigned k = (unsigned)((v >> 19) & 0x1Fu), sh = 2u * (k & 3u);
            m->msr[mi] = (v & UINT64_C(0xFFFFFFFF)) | ((uint64_t)w->chunk[k] << 32) |
                         ((uint64_t)((w->ecc[k >> 2] >> sh) & 3u) << 48);
            break;
        }
        const unsigned k = cmd_sub(v);
        w->chunk[k] = (uint16_t)(v >> 32);
        const unsigned sh = 2u * (k & 3u);
        w->ecc[k >> 2] = (uint8_t)((w->ecc[k >> 2] & ~(3u << sh)) | (((v >> 48) & 3u) << sh));
        break;
    }
    case MSR_L2_TAG: {
        MercedCache *mc = &m->cache;
        const unsigned way = cmd_way(v);
        if (way >= MC_L2_WAYS) { break; }
        MercedL2Way *w = &mc->l2[cmd_set(v)][way];
        if (cmd_is_read(v)) {   /* half in 19 (0xFFDC79F0, 0xFFDC0480) */
            const bool hi = ((v >> 19) & 0x1Fu) != 0;
            const uint64_t half = hi ? (w->tag >> 15) & 0x7FFFu : w->tag & 0x7FFFu;
            m->msr[mi] = (v & UINT64_C(0xFFFFFFFF)) | (half << 32) |
                         ((hi && w->parity) ? UINT64_C(1) << 49 : 0);
            break;
        }
        const bool was_valid = way_valid(w);
        const uint32_t half = (uint32_t)((v >> 32) & 0x7FFFu);
        if (cmd_sub(v) == 0u) {
            w->tag = (w->tag & ~UINT32_C(0x7FFF)) | half;
            w->tag_lo = true;
        } else {
            w->tag = (w->tag & UINT32_C(0x7FFF)) | (half << 15);
            w->tag_hi = true;
            w->parity = ((v >> 49) & 1u) != 0;
        }
        w->tag_seq = ++mc->seq;
        if (way_valid(w) != was_valid) {
            mc->nvalid += way_valid(w) ? 1u : (unsigned)-1;
            update_observe(m, mc);
        }
        break;
    }
    case MSR_L2_STATE: {
        MercedCache *mc = &m->cache;
        const unsigned way = cmd_way(v);
        if (way >= MC_L2_WAYS) { break; }
        MercedL2Way *w = &mc->l2[cmd_set(v)][way];
        if (cmd_is_read(v)) {
            m->msr[mi] = (v & UINT64_C(0xFFFFFFFF)) | ((uint64_t)w->stword << 32);
            break;
        }
        const bool was_valid = way_valid(w);
        const bool had_state = way_state(w) != 0u;
        w->stword = (uint16_t)((v >> 32) & 0x3FFu);
        /* 0xFFE0D520 expects the write-back from state 6 as from 2. */
        w->modified = (way_state(w) & 3u) == 2u;
        if (way_valid(w) != was_valid) {
            mc->nvalid += way_valid(w) ? 1u : (unsigned)-1;
        }
        if ((way_state(w) != 0u) != had_state) {
            mc->nstate += had_state ? (unsigned)-1 : 1u;
        }
        update_observe(m, mc);
        break;
    }
    default:
        break;
    }
}

/* Which of the bundle's memory ops this is: the first logs in 5EAh, the
 * second in 5EBh (0xFFE130C0 loads the bad line in slot 0 or 1 of one
 * bundle and expects r9 = 5EAh bit 0 << 16, or 5EBh bit 0 << 24). */
static unsigned access_port(const Merced *m, MercedCache *mc)
{
    const uint64_t ip = m->ip & ~UINT64_C(0xF);
    const unsigned ri = (unsigned)(m->ip & 3u);
    if (ip != mc->port_ip || ri <= mc->port_ri) { mc->port_ip = ip; mc->port = 0; }
    else { mc->port++; }
    mc->port_ri = ri;
    return mc->port ? 1u : 0u;
}

/* Signal a logged error. A corrected one is a CMC interrupt on CMCV's vector
 * while CMCV (cr74) is unmasked, logged with bit 4: 0xFFE130C0's store phase
 * unmasks CMCV, lowers TPR and expects the external-interrupt handler
 * (0xFFDB3000), which tests 5EAh bit 4 (r9 bit 19). Otherwise -- uncorrected,
 * or CMCV masked as PAL leaves it (0x10000) for the other phases -- it is a
 * machine check, logged with bit 0, which PALE_CHECK's handler tests. */
static void log_error(Merced *m, MercedCache *mc, unsigned port, uint64_t log, bool corrected)
{
    const unsigned mi = port ? MSR_L2_LOG1 : MSR_L2_LOG;
    const uint64_t cmcv = m->cr[CR_CMCV];
    if (corrected && !((cmcv >> 16) & 1u)) {
        m->msr[mi] = (log & ~LOG_VALID) | LOG_CMC_VALID;
        m->msr_written[mi] = true;
        merced_raise_external(m, (uint8_t)(cmcv & 0xFFu));
        return;
    }
    m->msr[mi] = log;
    m->msr_written[mi] = true;
    mc->mca_pending = true;
}

/* A tag-parity error in the set of `pa`: log it and raise the machine
 * check. True when the access must not complete. */
static bool check_set(Merced *m, MercedCache *mc, uint64_t pa, bool store, unsigned port)
{
    const unsigned set = (unsigned)(pa >> 6) & (MC_L2_SETS - 1u);
    const uint32_t tag = (uint32_t)((pa >> 14) & 0x3FFFFFFFu);
    int bad = -1;
    for (unsigned way = 0; way < MC_L2_WAYS; way++) {
        const MercedL2Way *w = &mc->l2[set][way];
        if (!way_valid(w) || (bool)(__builtin_popcount(w->tag) & 1) == w->parity) { continue; }
        if (bad < 0 || w->tag_seq > mc->l2[set][bad].tag_seq) { bad = (int)way; }
    }
    if (bad < 0) { return false; }
    const MercedL2Way *w = &mc->l2[set][bad];
    uint64_t log = LOG_FIXED | LOG_VALID | (store ? LOG_STORE : LOG_LOAD) |
                   ((uint64_t)set << 25) | ((uint64_t)bad << 33);
    if (w->tag == tag)  { log |= LOG_HIT; }
    if (w->modified)    { log |= LOG_MODIFIED | LOG_UNCORR; }
    log_error(m, mc, port, log, false);
    return true;
}

/* The way of `pa`'s set holding its tag (the one tagged last), or NULL. */
static MercedL2Way *find_hit(MercedCache *mc, uint64_t pa, unsigned *way_out)
{
    const unsigned set = (unsigned)(pa >> 6) & (MC_L2_SETS - 1u);
    const uint32_t tag = (uint32_t)((pa >> 14) & 0x3FFFFFFFu);
    MercedL2Way *hit = NULL;
    for (unsigned way = 0; way < MC_L2_WAYS; way++) {
        MercedL2Way *w = &mc->l2[set][way];
        if (way_valid(w) && w->tag == tag && (!hit || w->tag_seq > hit->tag_seq)) {
            hit = w; *way_out = way;
        }
    }
    return hit;
}

static uint64_t way_qword(const MercedL2Way *w, unsigned q)
{
    uint64_t d = 0;
    for (unsigned k = 0; k < 4; k++) { d |= (uint64_t)w->chunk[4 * q + k] << (16u * k); }
    return d;
}

static void way_set_qword(MercedL2Way *w, unsigned q, uint64_t d)
{
    for (unsigned k = 0; k < 4; k++) { w->chunk[4 * q + k] = (uint16_t)(d >> (16u * k)); }
}

/* Check qword q of a hit line: corrects a single-bit error in place
 * (logging it and raising the machine check), returns the data. */
static uint64_t checked_qword(Merced *m, MercedCache *mc, MercedL2Way *w, unsigned set,
                              unsigned way, unsigned q, bool store, unsigned port)
{
    uint64_t d = way_qword(w, q);
    const uint8_t syn = (uint8_t)(ecc_of(d) ^ w->ecc[q]);
    if (syn == 0) { return d; }
    uint64_t log = LOG_VALID | (store ? LOG_STORE : (LOG_LOAD | LOG_DATA_LD)) |
                   ((uint64_t)set << 25) | ((uint64_t)way << 33);
    bool corrected = true;
    if (__builtin_popcount(syn) != 1) {
        int bit = -1;
        for (unsigned j = 0; j < 64; j++) {
            uint8_t col = 0;
            for (unsigned i = 0; i < 8; i++) { col |= (uint8_t)(((g_ecc_mask[i] >> j) & 1u) << i); }
            if (col == syn) { bit = (int)j; break; }
        }
        if (bit >= 0) { d ^= UINT64_C(1) << bit; }
        else          { corrected = false; }
    }
    if (corrected) {
        log |= LOG_DATA;
        w->ecc[q] = ecc_of(d);
        way_set_qword(w, q, d);
    } else {
        /* Two flipped check bits (0xFFE13D00, both lines in state 2) expect
         * 0x401090082: type 16 in place of 14, and neither bit 12 nor the
         * uncorrected status -- those two follow the state only for a tag
         * error (sub-test 2's P2); no data-error case sets them. */
        log |= LOG_DATA_UE;
    }
    /* A store over an uncorrectable qword is signalled like a corrected
     * error: 0xFFE13D00's st4 phase (CMCV unmasked) expects the CMC
     * interrupt, taken after the store bundle (IIP = the next one), where
     * its ld8 phase expects the machine check. */
    log_error(m, mc, port, log, corrected || store);
    return d;
}

/* ---- L2 and L3 fills ------------------------------------------------------
 *
 * Phase-2 test 0xFFE0E100 (failure bit 55), after its L1D and L3 parts,
 * works one L2 set: six lines 16 KB apart (A), six more (B), loads, stores,
 * the L2 ports (data 0xFFDC8070, state 0xFFDC7C20) and the 600h port. It
 * expects, in order:
 *   - loads of A fill the six ways in the order 0, 2, 4, 1, 3, 5 in state 3
 *     (clean) and the L3 (state 2); loads of B then replace them in the same
 *     order -- a pair LRU over (0,1)(2,3)(4,5), true LRU between the pairs
 *     and a bit within each (l2_victim), reset by the invalidate;
 *   - stores to B stay in the L2 (the lines turn modified, memory keeps B);
 *   - loading A again evicts the modified B lines into their L3 lines,
 *     which read back modified (3) with the stored data;
 *   - the L2 invalidate (5E5h bit 17, 0xFFDC0230) and the 600h teardown
 *     (8140h) then discard all of it: memory still holds B.
 * So fills are followed, as for the L1s, from the invalidate while the L2 is
 * enabled (5E4h bits 6 and 2 clear, 0xFFDC0390) and bit 17 is clear, for
 * L2_TRACK_ACCESSES accesses; the lookup goes L1D, L2, L3, memory. A fill
 * copies the line (from the L3 if it holds it, else memory), and the L2 is
 * write-back, so these lines are the data. When following ends the lines
 * the fills brought in are written back (to the L3 line or memory) and
 * dropped -- the model's bound, not the hardware's: it keeps SAL's stores
 * from living in the model once it stops watching. */
static void l2_lru_reset(MercedCache *mc)
{
    for (unsigned s = 0; s < MC_L2_SETS; s++) {
        mc->l2_order[s][0] = 0;
        mc->l2_order[s][1] = 1;
        mc->l2_order[s][2] = 2;
        mc->l2_inpair[s] = 0;
    }
}

static unsigned l2_victim(const MercedCache *mc, unsigned set)
{
    const unsigned p = mc->l2_order[set][0];
    return 2u * p + ((mc->l2_inpair[set] >> p) & 1u);
}

static void l2_touch(MercedCache *mc, unsigned set, unsigned way)
{
    const unsigned p = way / 2u;
    uint8_t *o = mc->l2_order[set];
    unsigned k = 0;
    while (k < 2u && o[k] != p) { k++; }
    for (; k < 2u; k++) { o[k] = o[k + 1u]; }
    o[2] = (uint8_t)p;
    if (way & 1u) { mc->l2_inpair[set] = (uint8_t)(mc->l2_inpair[set] & ~(1u << p)); }
    else          { mc->l2_inpair[set] = (uint8_t)(mc->l2_inpair[set] | (1u << p)); }
}

static void l2_recount(Merced *m, MercedCache *mc)
{
    unsigned v = 0, s = 0;
    for (unsigned set = 0; set < MC_L2_SETS; set++) {
        for (unsigned w = 0; w < MC_L2_WAYS; w++) {
            if (way_valid(&mc->l2[set][w])) { v++; }
            if (way_state(&mc->l2[set][w]) != 0u) { s++; }
        }
    }
    mc->nvalid = v;
    mc->nstate = s;
    update_observe(m, mc);
}

static void l2_invalidate_all(Merced *m, MercedCache *mc)
{
    for (unsigned s = 0; s < MC_L2_SETS; s++) {
        for (unsigned w = 0; w < MC_L2_WAYS; w++) {
            mc->l2[s][w].stword = 0;
            mc->l2[s][w].modified = false;
            mc->l2[s][w].owned = false;
        }
    }
    mc->nvalid = 0;
    mc->nstate = 0;
    mc->l2_known = true;
    mc->l2_t0 = now_bundles(m);
    mc->l2_accesses = 0;
    l2_lru_reset(mc);
    update_observe(m, mc);
}

/* The L3 line holding `line` (the one written last), if any. Every load,
 * store and fetch asks while the model observes, so the lines are indexed
 * by line address (rebuilt after any change, l3_recount): scanning all of
 * them each time was most of the self-tests' run time. */
static unsigned l3_hkey(uint64_t line) { return (unsigned)(line >> 6) & (MC_L3_HASH - 1u); }

static MercedL3Line *l3_find(MercedCache *mc, uint64_t line)
{
    if (!mc->ninj) { return NULL; }
    if (mc->l3_dirty) {
        memset(mc->l3_head, 0xFF, sizeof mc->l3_head);
        for (unsigned i = 0; i < MC_INJ_LINES; i++) {
            if (!mc->inj[i].used) { continue; }
            const unsigned k = l3_hkey(mc->inj[i].pa);
            mc->l3_next[i] = mc->l3_head[k];
            mc->l3_head[k] = (int16_t)i;
        }
        mc->l3_dirty = false;
    }
    MercedL3Line *l = NULL;
    for (int i = mc->l3_head[l3_hkey(line)]; i >= 0; i = mc->l3_next[i]) {
        MercedL3Line *x = &mc->inj[i];
        if (!x->used || x->state == 0u || x->pa != line) { continue; }
        if (!l || x->seq > l->seq) { l = x; }
    }
    return l;
}

/* A modified L2 line leaves: into its L3 line (which turns modified, test
 * 0xFFE0E100), else to memory. */
static void l2_writeback(Merced *m, MercedCache *mc, const MercedL2Way *w, unsigned set)
{
    const uint64_t line = ((uint64_t)w->tag << 14) | ((uint64_t)set << 6);
    MercedL3Line *l = l3_find(mc, line);
    for (unsigned q = 0; q < 8; q++) {
        const uint64_t d = way_qword(w, q);
        if (l) { l->data[q] = d; l->ecc[q] = bus_ecc_of(line, d); }
        else   { merced_bus_write(m, line + 8u * q, d, 8); }
    }
    if (l) { l->planted = true; l->state = 3u; l3_recount(m, mc); }
}

static void l2_fill(Merced *m, MercedCache *mc, uint64_t line, const MercedL3Line *src)
{
    const unsigned set = (unsigned)(line >> 6) & (MC_L2_SETS - 1u), v = l2_victim(mc, set);
    MercedL2Way *w = &mc->l2[set][v];
    if (way_valid(w) && w->modified) { l2_writeback(m, mc, w, set); }
    for (unsigned q = 0; q < 8; q++) {
        const uint64_t d = src ? src->data[q] : merced_bus_read(m, line + 8u * q, 8);
        way_set_qword(w, q, d);
        w->ecc[q] = ecc_of(d);
    }
    w->tag = (uint32_t)((line >> 14) & 0x3FFFFFFFu);
    w->parity = (__builtin_popcount(w->tag) & 1) != 0;
    w->tag_lo = w->tag_hi = true;
    w->stword = 3u << 6;            /* clean */
    w->modified = false;
    w->owned = true;
    w->tag_seq = ++mc->seq;
    l2_touch(mc, set, v);
    l2_recount(m, mc);
}

/* Following ends: the fills' lines go (modified ones written back first). */
static void l2_untrack(Merced *m, MercedCache *mc)
{
    for (unsigned s = 0; s < MC_L2_SETS; s++) {
        for (unsigned v = 0; v < MC_L2_WAYS; v++) {
            MercedL2Way *w = &mc->l2[s][v];
            if (!w->owned) { continue; }
            if (way_valid(w) && w->modified) { l2_writeback(m, mc, w, s); }
            w->stword = 0;
            w->modified = false;
            w->owned = false;
        }
    }
    l2_recount(m, mc);
}

static void l3_untrack(Merced *m, MercedCache *mc)
{
    for (unsigned i = 0; mc->ninj && i < MC_INJ_LINES; i++) {
        MercedL3Line *l = &mc->inj[i];
        if (!l->used || !l->owned) { continue; }
        if (l->planted && l->state == 3u) {
            for (unsigned q = 0; q < 8; q++) { merced_bus_write(m, l->pa + 8u * q, l->data[q], 8); }
        }
        l->used = false;
    }
    l3_recount(m, mc);
}

/* One cacheable access while the L2 is followed: true while it still is. */
static bool l2_count(Merced *m, MercedCache *mc)
{
    if (!l2_tracking(mc)) { return false; }
    if (++mc->l2_accesses >= L2_TRACK_ACCESSES) {
        mc->l2_known = false;
        l2_untrack(m, mc);
        return false;
    }
    return true;
}

/* The qword at `pa` as the level below the L1s has it: the L2, else a line
 * the L3 holds, else memory -- what a filled L1 line holds (the L1s fill
 * from the L2, and the L2 can hold stores memory has not seen). */
static uint64_t below_l1_qword(Merced *m, MercedCache *mc, uint64_t pa)
{
    if (mc->nvalid) {
        unsigned way = 0;
        const MercedL2Way *w = find_hit(mc, pa, &way);
        if (w) { return way_qword(w, (unsigned)(pa >> 3) & 7u); }
    }
    const MercedL3Line *l = mc->ninj ? l3_find(mc, pa & ~UINT64_C(63)) : NULL;
    if (l && l->planted) { return l->data[(pa >> 3) & 7u]; }
    return merced_bus_read(m, pa & ~UINT64_C(7), 8);
}

/* An instruction fetch that misses the I-cache: the L2 is unified, so a
 * bundle a store left in it (the I-cache fill test 0xFFE0F860 copies its
 * chain with st8 and runs it) comes from there, else from a line the L3
 * holds, else from memory. */
bool merced_cache_fetch(Merced *m, uint64_t pa, uint64_t *lo, uint64_t *hi)
{
    MercedCache *mc = &m->cache;
    const uint64_t line = pa & ~UINT64_C(63);
    const unsigned q = (unsigned)(pa >> 3) & 6u;
    if (mc->nvalid) {
        unsigned way = 0;
        const MercedL2Way *w = find_hit(mc, pa, &way);
        if (w) { *lo = way_qword(w, q); *hi = way_qword(w, q + 1u); return true; }
    }
    const MercedL3Line *l = mc->ninj ? l3_find(mc, line) : NULL;
    if (l && l->planted) { *lo = l->data[q]; *hi = l->data[q + 1u]; return true; }
    return false;
}

bool merced_cache_store(Merced *m, uint64_t pa, unsigned size, uint64_t raw)
{
    MercedCache *mc = &m->cache;
    if (((pa & 7u) + size) > 8u) {      /* not within one qword: byte by byte */
        for (unsigned k = 0; k < size; k++) {
            const uint64_t b = (raw >> (8u * k)) & 0xFFu;
            if (!merced_cache_store(m, pa + k, 1, b)) { merced_bus_write(m, pa + k, (uint8_t)b, 1); }
        }
        return true;
    }
    if (mc->nl1d) { l1d_store(mc, pa, size, raw); }   /* write-through: continues */
    const uint64_t line = pa & ~UINT64_C(63);
    const unsigned q = (unsigned)(pa >> 3) & 7u;
    const bool l2t = l2_count(m, mc);
    if ((mc->testmode || mc->nvalid != 0 || l2t) && !(m->psr & PSR_MC)) {
        const unsigned port = access_port(m, mc);
        if (check_set(m, mc, pa, true, port)) { return true; }
        unsigned way = 0;
        MercedL2Way *w = find_hit(mc, pa, &way);
        if (w) {
            /* A store to a resident line stays in the (write-back) L2, and
             * a clean line turns modified (0xFFE0E100). */
            const unsigned set = (unsigned)(pa >> 6) & (MC_L2_SETS - 1u);
            uint64_t d = checked_qword(m, mc, w, set, way, q, true, port);
            for (unsigned i = 0; i < size; i++) {
                const unsigned b = (unsigned)(pa & 7u) + i;
                d = (d & ~(UINT64_C(0xFF) << (8u * b))) | (((raw >> (8u * i)) & 0xFFu) << (8u * b));
            }
            way_set_qword(w, q, d);
            w->ecc[q] = ecc_of(d);
            if ((way_state(w) & 3u) == 3u) {
                w->stword = (uint16_t)((w->stword & ~(7u << 6)) | (2u << 6));
            }
            w->modified = true;
                    if (l2t) { l2_touch(mc, set, way); }
            return true;
        }
    }
    /* A store to a line the port placed updates the line (0xFFE16590 stores
     * into its injected line over flash before flushing it); one a fill
     * brought in takes the store and turns modified (the L3 is write-back
     * too). */
    MercedL3Line *l = l3_find(mc, line);
    if (!l) { return false; }
    if (!l->planted) {
        for (unsigned k = 0; k < 8; k++) {
            l->data[k] = merced_bus_read(m, line + 8u * k, 8);
            l->ecc[k] = bus_ecc_of(line, l->data[k]);
        }
        l->planted = true;
        l->state = 3u;
        l3_recount(m, mc);
    }
    uint64_t d = l->data[q];
    for (unsigned k = 0; k < size; k++) {
        const unsigned b = (unsigned)(pa & 7u) + k;
        d = (d & ~(UINT64_C(0xFF) << (8u * b))) | (((raw >> (8u * k)) & 0xFFu) << (8u * b));
    }
    l->data[q] = d;
    l->ecc[q] = bus_ecc_of(l->pa, d);
    return true;
}

/* fc (fw130 phase-2 test 0xFFE14EB0): the test plants a modified line at a
 * RAM address with one check bit flipped, unmasks CMCV and runs fc on it;
 * it expects the CMC interrupt with 5EAh = 0x1404080 | set << 25 -- the
 * corrected-data type with access bit 22 (a write-back, beside 19 load and
 * 20/21 store) and no way field -- and a later ld8 from the address to
 * return the corrected pattern from memory. So the line is checked,
 * corrected, written back when modified, and invalidated. */
static void icache_fc(Merced *m, MercedCache *mc, uint64_t pa);

void merced_cache_fc(Merced *m, uint64_t pa)
{
    MercedCache *mc = &m->cache;
    icache_fc(m, mc, pa);
    /* Every level drops the line (0xFFE0D520 plants it in the I-cache, L1D,
     * L2 and through the 600h port, runs fc and reads each back invalid);
     * the L1D is write-through, so it has nothing to write back. INFERRED:
     * that test sets the L1D way valid with no 5D0h tag write at all (only
     * 5D8h commands 10h/40h with constant payloads), so the tag there never
     * matches the flushed address, and still expects the way invalid --
     * fc drops the whole set's lines from the L1D. */
    if (mc->nl1d) {
        const unsigned set = (unsigned)(pa >> 5) & (MC_L1D_SETS - 1u);
        mc->l1d_valid[set & 0x3Fu] &= (uint8_t)~(0xFu << (4u * ((set >> 6) & 1u)));
        l1d_recount(m, mc);
    }
    /* A line the port placed: fw130's test 0xFFE16590 runs fc on one at a
     * RAM address (0x800000 + n) carrying a bad bus check bit inside its
     * window and expects the CMC interrupt there, then a plain ld8 of the
     * address to return the injected pattern -- so the corrected line is
     * written back to memory -- and its own st8s to land in memory. That
     * line is in state 3; 0xFFE0D520 expects memory unchanged after fc of
     * one in state 1 or 2, so only 3 (modified) is written back. */
    for (unsigned i = 0; mc->ninj && i < MC_INJ_LINES; i++) {
        MercedL3Line *l = &mc->inj[i];
        if (!l->used || l->pa != (pa & ~UINT64_C(63))) { continue; }
        const bool wb = l->planted && l->state == 3u;
        if (l->planted && l->state != 0u && !(m->psr & PSR_MC)) { check_injected(m, mc, l); }
        if (wb) {
            for (unsigned q = 0; q < 8; q++) { merced_bus_write(m, l->pa + 8u * q, l->data[q], 8); }
        }
        l->used = false;
    }
    l3_recount(m, mc);
    /* INFERRED, as for the L1D: 0xFFE0D520 gives a way of the set a state
     * (5E2h) but never writes its tag -- the tag left there by earlier tests
     * is not the flushed address's -- and expects the way invalid after fc,
     * and, when the state was modified (2 or 6), its data in memory at the
     * flushed line. So every way of the set holding a line takes part, and
     * a modified one is written back to the line fc names. */
    const unsigned set = (unsigned)(pa >> 6) & (MC_L2_SETS - 1u);
    bool logged = false;
    for (unsigned way = 0; way < MC_L2_WAYS; way++) {
        MercedL2Way *w = &mc->l2[set][way];
        if (way_state(w) == 0u) { continue; }
        /* A line the model filled holds its own tag: fc names it only by
         * that tag. */
        if (w->owned && w->tag != (uint32_t)((pa >> 14) & 0x3FFFFFFFu)) { continue; }
        for (unsigned q = 0; q < 8; q++) {
            uint64_t d = way_qword(w, q);
            const uint8_t syn = (uint8_t)(ecc_of(d) ^ w->ecc[q]);
            if (syn && !logged && !(m->psr & PSR_MC)) {
                bool corrected = __builtin_popcount(syn) == 1;
                for (unsigned j = 0; !corrected && j < 64; j++) {
                    uint8_t col = 0;
                    for (unsigned i = 0; i < 8; i++) { col |= (uint8_t)(((g_ecc_mask[i] >> j) & 1u) << i); }
                    if (col == syn) { d ^= UINT64_C(1) << j; corrected = true; }
                }
                log_error(m, mc, 0, (corrected ? LOG_DATA : LOG_DATA_UE) | LOG_VALID | LOG_WRITEBACK |
                                   ((uint64_t)set << 25), corrected);
                logged = true;
            }
            if (w->modified) { merced_bus_write(m, (pa & ~UINT64_C(63)) + 8u * q, d, 8); }
        }
        const bool was_valid = way_valid(w);
        w->stword = 0;
        w->modified = false;
        if (was_valid) { mc->nvalid--; }
        mc->nstate--;
    }
    update_observe(m, mc);
}

bool merced_cache_take_mca(Merced *m)
{
    MercedCache *mc = &m->cache;
    if (!mc->mca_pending) { return false; }
    mc->mca_pending = false;
    return true;
}

bool merced_cache_load(Merced *m, uint64_t pa, unsigned size, uint64_t *raw)
{
    MercedCache *mc = &m->cache;
    /* Not within one qword (phase-2 test 0xFFE0A0F0 loads 8 bytes at +31h
     * of a line the L2 holds modified): byte by byte, each from wherever
     * the hierarchy has it. */
    if (((pa & 7u) + size) > 8u) {
        uint64_t v = 0;
        for (unsigned k = 0; k < size; k++) {
            uint64_t b = 0;
            if (!merced_cache_load(m, pa + k, 1, &b)) { b = merced_bus_read(m, pa + k, 1); }
            v |= (b & 0xFFu) << (8u * k);
        }
        *raw = v;
        return true;
    }
    const uint64_t line = pa & ~UINT64_C(63);
    /* L1D first (planted lines, parity errors, fills). */
    if (mc->nl1d || l1d_tracking(mc)) {
        const int r = l1d_load(m, mc, pa, size, raw);
        if (r >= 0) { return r != 0; }
    }
    /* Then the L2. PSR.mc masks machine checks: PALE_CHECK runs with it set
     * and its own (uncacheable) accesses must reach memory -- without this
     * the stub's table walk (0xFFFF8620 ld8 [0xFFFFFFE0], set FFh) re-enters
     * it. */
    const bool l2t = l2_count(m, mc);
    bool l2_miss = false;
    if ((mc->testmode || mc->nvalid != 0 || l2t) && !(m->psr & PSR_MC) && ((pa & 7u) + size) <= 8u) {
        const unsigned port = access_port(m, mc);
        if (check_set(m, mc, pa, false, port)) { *raw = 0; return true; }
        /* Several ways can hold the tag (the tests retag way after way); the
         * one tagged last is the line the test means. */
        unsigned way = 0;
        MercedL2Way *w = find_hit(mc, pa, &way);
        if (w) {
            const unsigned set = (unsigned)(pa >> 6) & (MC_L2_SETS - 1u);
            const uint64_t d = checked_qword(m, mc, w, set, way, (unsigned)(pa >> 3) & 7u, false, port);
            *raw = (size == 8u) ? d : ((d >> (8u * (pa & 7u))) & ((UINT64_C(1) << (8u * size)) - 1u));
            if (l2t) { l2_touch(mc, set, way); }
            return true;
        }
        l2_miss = l2t;
    }
    /* Then the L3. Several ways can hold the line (a fill, then the port
     * planting it in another way: 0xFFE15F80); the one written last is it. */
    MercedL3Line *l = l3_find(mc, line);
    const bool l3_hit = l != NULL;
    if (l) { mc->l3_plru[l3_set(line)] = plru_touch(mc->l3_plru[l3_set(line)], l->way); }
    if (l2_miss) { l2_fill(m, mc, line, (l && l->planted) ? l : NULL); }
    if (l && l->planted) {
        if (!(m->psr & PSR_MC)) { check_injected(m, mc, l); }
        const uint8_t *b = (const uint8_t *)l->data;   /* host LE = memory order */
        uint64_t v = 0;
        for (unsigned k = 0; k < size; k++) { v |= (uint64_t)b[(pa & 63u) + k] << (8u * k); }
        *raw = v;
        return true;
    }
    /* L3 fills (0xFFE0E100), followed after an 8140h flush: a miss takes
     * the set's victim way in state 2 (INFERRED: the test accepts 1 or 2). */
    if (l3_tracking(mc)) {
        if (++mc->l3_loads >= L3_TRACK_LOADS) {
            mc->l3_known = false;
            l3_untrack(m, mc);
        } else if (!l3_hit) {
            const unsigned set = l3_set(line), v = plru_victim(mc->l3_plru[set]);
            MercedL3Line *f = l3_slot(mc, set, v);
            /* A modified victim is written back (0xFFE0E100 marks four
             * lines modified, then fills the set with others and expects
             * their values in memory). */
            if (f->used && f->planted && f->state == 3u) {
                for (unsigned q = 0; q < 8; q++) { merced_bus_write(m, f->pa + 8u * q, f->data[q], 8); }
            }
            memset(f, 0, sizeof *f);
            f->used = true;
            f->owned = true;
            f->pa = line;
            f->addr = line;
            f->way = v;
            f->state = 2u;
            f->seq = ++mc->seq;
            mc->l3_plru[set] = plru_touch(mc->l3_plru[set], v);
            l3_recount(m, mc);
        }
    }
    return false;
}

/* ---- L1 instruction cache -----------------------------------------------
 *
 * Phase-1 test 0xFFE04020 (failure bit 51) runs 0xFFE02000 then 0xFFE00D20.
 * Ports, built by the same command-word helper (0xFFDC6460) as the L2's:
 * set (PA 11:5) in 10:0, way in 13:11, sub-field in 18:14 for a write.
 *   421h tag (0xFFDC6620): sub 0 carries tag 15:0 in 47:32; sub 1 tag 31:16
 *        in 47:32, valid in 48 and parity in 49. Tag = PA >> 12; parity
 *        makes popcount(tag) + parity odd (0xFFE02060).
 *   422h data (0xFFDC6A40, 0xFFDC6C90): sub = 16-bit chunk 0..15, payload
 *        47:32; chunk 4w+k is bits 16k+15:16k of array word w, and chunk
 *        4w+2 carries word w's parity in 48 (odd, like the tag's). Words
 *        0/1 hold the line's first bundle, 2/3 its second, each bundle
 *        permuted bit by bit (0xFFDC9720, g_ic_perm below).
 * Both write with 425h bit 3 set around the port write; it is cleared again
 * before anything executes.
 * 420h: bit 1 disables the cache (0xFFDC0290: enable clears it); a 0 -> 1
 * transition of bit 0 invalidates every line -- 0xFFDC0180 pulses it, and
 * PALE_CHECK's handler 0xFFDF32D0 pulses it after an I-cache error.
 * INFERRED (edge, not level): test 0xFFE00D20 holds bit 0 set while it
 * executes the lines it planted, so a held bit invalidates nothing.
 *
 * 0xFFE02000 plants eight lines (sets 0, 1, 3Eh, 3Fh, 40h, 41h, 7Eh, 7Fh of
 * its 0xFFE03000 page) in each way in turn, each "shladd r8 = r8, 1; br
 * next", enables the cache and jumps to 0xFFE03000 physically (cacheable):
 * memory there holds "r8 = -1; br b0", so only fetches from the array give
 * the r8 it expects. 0xFFE00D20 sets 420h bit 6, plants a line with one
 * word's parity flipped (or the tag's), arms the self-test signature and
 * jumps to it: it expects PALE_CHECK's handler with 425h bit 0 (the handler
 * returns it as r9 = 1) and IIP = the line's bundle, and 425h = 30h after
 * the handler has cleared 1:0. INFERRED: with 420h bit 6 clear an error
 * is not reported; the line, a clean copy of memory, is dropped and the
 * fetch goes to memory.
 *
 * FILLS (phase-2 test 0xFFE0F860, failure bit 51 again). It copies 32
 * one-bundle lines to RAM -- four sets, a new page per line -- invalidates,
 * writes 423h = 0 for each set (0xFFDC67F0: the replacement state, payload
 * 34:32), enables the cache and runs the chain cacheably. Then it reads
 * every way back: 421h must give valid and the line's tag, 422h its
 * bundle (read layout in icache_msr_write; 0xFFDC9B80 is the exact inverse
 * of 0xFFDC9720). The k-th line of a set must be in way 0, 2, 1, 3:
 * tree pseudo-LRU (plru_victim). So fills are modelled, as tags and
 * replacement state only -- a filled line's words are read from memory
 * when the port asks, and fetches of it go to memory as before.
 * Fills are only seen by the interpreter's fetch. They are followed from an
 * invalidate (the cache is then empty, so its contents are known) while the
 * cache is enabled -- the JIT stands aside meanwhile -- for IC_TRACK_FETCHES cacheable fetches. That bound is the
 * model's, not the hardware's: the test reads the arrays a few dozen fetches
 * after its invalidate, while the cache stays enabled for good after PAL's
 * last one, and following every fetch from then on would keep SAL, EFI and
 * the OS off the JIT. An array read after the bound is logged as
 * unmodelled.
 */
#define IC_TRACK_FETCHES 65536u
static void icache_untracked(Merced *m);
#define IC_INVAL      (UINT64_C(1) << 0)
#define IC_DISABLE    (UINT64_C(1) << 1)
#define IC_REPORT     (UINT64_C(1) << 6)
#define IC_LOG_ERROR  UINT64_C(0x31)

/* 0xFFDC9720: array word 0 bit i is bundle bit g_ic_perm[0][i], word 1 bit
 * i bundle bit g_ic_perm[1][i] (bundle bits 63:0 the first word in memory
 * order, 127:64 the second). Extracted from its tbit/mov-pr sequence; a
 * bijection on 0..127. */
static const uint8_t g_ic_perm[2][64] = {
    {   5,   6,   7,   8,   9,  10,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,
       25,  26,  27,  28,  29,  30,  31,  39,  40,  41,  46,  47,  48,  49,  50,  51,
      122, 121, 117, 116, 115, 114, 113,  92, 112,  89,  88,  87, 103, 102, 101,  82,
      100,  99,  98,  97,  91,  90,  65,  64,  63,  62,  61,  60,  59,  58,  12,  11 },
    { 104, 105, 106, 123,  56,  57,   4,   3,   2,   1, 120,  79,  38, 125, 127, 124,
      126, 119, 118,  45,  44,  43,  42,  37,  36,  86,  85,  84,  83,  78,  77,   0,
      111, 110, 109, 108, 107,  96,  95,  69,  94,  93,  81,  80,  76,  75,  74,  73,
       72,  71,  70,  68,  67,  66,  55,  54,  53,  52,  35,  34,  33,  32,  14,  13 },
};

static bool parity_odd(uint64_t v, unsigned p)
{
    return (((unsigned)__builtin_popcountll(v) + p) & 1u) != 0;
}

/* A way's validity and origin; keeps nplanted (which gates the model's
 * observation: only planted lines differ from memory). */
static void icache_set_state(Merced *m, MercedCache *mc, MercedL1IWay *w, bool valid, bool planted)
{
    /* Every change to a way's tag or words ends here: refresh its parity
     * verdicts, which every fetch consults. */
    w->tag_bad = !parity_odd(w->tag, w->tpar);
    w->word_bad = 0;
    for (unsigned k = 0; k < MC_L1I_WORDS; k++) {
        if (!parity_odd(w->word[k], (w->wpar >> k) & 1u)) { w->word_bad |= (uint8_t)(1u << k); }
    }
    const bool was = w->valid && w->planted, now = valid && planted;
    w->valid = valid;
    w->planted = planted;
    if (was != now) {
        mc->nplanted += now ? 1u : (unsigned)-1;
        update_observe(m, mc);
    }
}

/* Fills are followed while every one since an invalidate has been seen and
 * the cache is enabled; ic_track sends every fetch here meanwhile. */
static void icache_update_track(MercedCache *mc)
{
    mc->ic_track = mc->ic_known && !(mc->icctl & IC_DISABLE);
}

static void icache_invalidate_all(Merced *m, MercedCache *mc)
{
    for (unsigned s = 0; s < MC_L1I_SETS; s++) {
        for (unsigned w = 0; w < MC_L1I_WAYS; w++) {
            icache_set_state(m, mc, &mc->l1i[s][w], false, false);
        }
    }
    mc->ic_known = true;
    mc->ic_t0 = now_bundles(m);
    mc->ic_unknown_logged = false;
    mc->ic_fetches = 0;
}

/* Tree pseudo-LRU over the 4 ways. INFERRED encoding (only 0 is ever
 * written, 0xFFDC67F0): bit 0 picks the pair (0 = ways 0/1), bit 1 the way
 * in pair 0/1, bit 2 in pair 2/3; a use points them away from the way. From
 * state 0 that fills ways 0, 2, 1, 3 -- the order test 0xFFE0F860 checks,
 * empty ways included (fill order there, not first-invalid). */
static unsigned plru_victim(uint8_t s)
{
    return (s & 1u) ? 2u + ((s >> 2) & 1u) : ((s >> 1) & 1u);
}

static uint8_t plru_touch(uint8_t s, unsigned way)
{
    if (way < 2u) { return (uint8_t)((s & ~3u) | 1u | (way == 0u ? 2u : 0u)); }
    return (uint8_t)((s & ~5u) | (way == 2u ? 4u : 0u));
}

/* The line's array words: planted ones as written, filled ones from the
 * memory they copy (the I-cache holds clean lines only). */
static void icache_words(Merced *m, MercedCache *mc, const MercedL1IWay *w, unsigned set,
                         uint64_t out[MC_L1I_WORDS], uint8_t *par)
{
    /* Only a valid filled line names memory (a fill is a cacheable fetch). */
    if (w->planted || !w->valid) {
        memcpy(out, w->word, sizeof w->word);
        *par = w->wpar;
        return;
    }
    const uint64_t pa = ((uint64_t)w->tag << 12) | ((uint64_t)set << 5);
    *par = 0;
    for (unsigned b = 0; b < 2; b++) {
        const uint64_t in[2] = { below_l1_qword(m, mc, pa + 16u * b),
                                 below_l1_qword(m, mc, pa + 16u * b + 8u) };
        for (unsigned k = 0; k < 2; k++) {
            uint64_t v = 0;
            for (unsigned i = 0; i < 64; i++) {
                const unsigned n = g_ic_perm[k][i];
                v |= ((in[n >> 6] >> (n & 63u)) & 1u) << i;
            }
            out[2u * b + k] = v;
            if (!parity_odd(v, 0)) { *par |= (uint8_t)(1u << (2u * b + k)); }
        }
    }
}

static void icache_msr_write(Merced *m, unsigned mi, uint64_t v)
{
    MercedCache *mc = &m->cache;
    if (mi == MSR_IC_CTL) {
        const bool rise = (v & IC_INVAL) && !(mc->icctl & IC_INVAL);
        mc->icctl = v;
        if (rise) { icache_invalidate_all(m, mc); }
        icache_update_track(mc);
        return;
    }
    const unsigned set = (unsigned)(v & 0x7FFu) % MC_L1I_SETS;
    if (mi == MSR_IC_LRU) {
        if (cmd_is_read(v)) {   /* 0xFFDC6740: 34:32 */
            m->msr[mi] = (v & UINT64_C(0xFFFFFFFF)) | ((uint64_t)mc->plru[set] << 32);
        } else {
            mc->plru[set] = (uint8_t)((v >> 32) & 7u);
        }
        return;
    }
    const unsigned way = cmd_way(v);
    if (way >= MC_L1I_WAYS) { return; }
    MercedL1IWay *w = &mc->l1i[set][way];
    w->dec_ok = 0;              /* words or tag may change below */
    if (cmd_is_read(v)) {
        if (!mc->ic_known && !mc->ic_unknown_logged) {
            mc->ic_unknown_logged = true;
            fprintf(stderr, "merced: msr[0x%03x] I-cache array read while the fill history "
                    "is unknown ip=0x%016" PRIX64 ": fills stopped being followed at "
                    "ip=0x%016" PRIX64 " psr=0x%" PRIX64 " (unmodelled)\n", mi, m->ip,
                    mc->ic_lost_ip, mc->ic_lost_psr);
        }
        const unsigned k = (unsigned)((v >> 19) & 0x1Fu);
        uint64_t r = 0;
        if (mi == MSR_IC_TAG) {
            /* 0xFFDC64E0: the half in 47:32, valid 48 and parity 49 with
             * either half. */
            r = (uint64_t)((k ? w->tag >> 16 : w->tag) & 0xFFFFu) |
                ((uint64_t)w->valid << 16) | ((uint64_t)w->tpar << 17);
        } else if (k < 4u * MC_L1I_WORDS) {
            /* 0xFFDC6890 reads bytewise: chunk k is byte k%8 of words
             * 2(k/8) in 39:32 and 2(k/8)+1 in 47:40, and chunks 4j+3 carry
             * parity bit j in 48. */
            uint64_t words[MC_L1I_WORDS];
            uint8_t par = 0;
            icache_words(m, mc, w, set, words, &par);
            const unsigned sh = 8u * (k & 7u), wa = 2u * (k >> 3);
            r = ((words[wa] >> sh) & 0xFFu) | (((words[wa + 1u] >> sh) & 0xFFu) << 8);
            if ((k & 3u) == 3u) { r |= (uint64_t)((par >> (k >> 2)) & 1u) << 16; }
        }
        m->msr[mi] = (v & UINT64_C(0xFFFFFFFF)) | (r << 32);
        return;
    }
    const unsigned sub = cmd_sub(v);
    const uint32_t half = (uint32_t)((v >> 32) & 0xFFFFu);
    if (!w->planted) {          /* the port now owns the line's words */
        uint8_t par = 0;
        icache_words(m, mc, w, set, w->word, &par);
        w->wpar = par;
    }
    if (mi == MSR_IC_TAG) {
        if (sub == 0u) {
            w->tag = (w->tag & UINT32_C(0xFFFF0000)) | half;
            icache_set_state(m, mc, w, w->valid, true);
        } else {
            w->tag = (w->tag & UINT32_C(0xFFFF)) | (half << 16);
            w->tpar = ((v >> 49) & 1u) != 0;
            icache_set_state(m, mc, w, ((v >> 48) & 1u) != 0, true);
        }
        return;
    }
    if (sub >= 4u * MC_L1I_WORDS) { return; }
    const unsigned word = sub >> 2, sh = 16u * (sub & 3u);
    w->word[word] = (w->word[word] & ~(UINT64_C(0xFFFF) << sh)) | ((uint64_t)half << sh);
    if ((sub & 3u) == 2u) {
        w->wpar = (uint8_t)((w->wpar & ~(1u << word)) | (((v >> 48) & 1u) << word));
    }
    icache_set_state(m, mc, w, w->valid, true);
}

/* fc drops the line from the I-cache too, probing the set's tags as a
 * fetch does: test 0xFFE10B60 plants a line with bad tag parity (its own
 * tag, then a tag one bit away from the address it flushes) and runs
 * "fc; sync.i" on it with 420h bit 6 set, expecting PALE_CHECK with 425h
 * bit 0 and IIP = the fc's bundle, 425h = 30h after the handler. */
static void icache_fc(Merced *m, MercedCache *mc, uint64_t pa)
{
    MercedL1IWay *ways = mc->l1i[(pa >> 5) & (MC_L1I_SETS - 1u)];
    for (unsigned w = 0; w < MC_L1I_WAYS; w++) {
        if (ways[w].valid && !parity_odd(ways[w].tag, ways[w].tpar) &&
            (mc->icctl & IC_REPORT) && !(m->psr & PSR_MC)) {
            m->msr[MSR_IC_LOG] |= IC_LOG_ERROR;
            m->msr_written[MSR_IC_LOG] = true;
            mc->mca_pending = true;
            return;
        }
    }
    for (unsigned w = 0; w < MC_L1I_WAYS; w++) {
        if (ways[w].valid && ways[w].tag == (uint32_t)(pa >> 12)) {
            icache_set_state(m, mc, &ways[w], false, false);
        }
    }
}

/* Code is about to run without icache fetches being seen, or the window
 * closed: the fill history stops being known. */
static void icache_untracked(Merced *m)
{
    MercedCache *mc = &m->cache;
    mc->ic_known = false;
    mc->ic_lost_ip = m->ip;
    mc->ic_lost_psr = m->psr;
    icache_update_track(mc);
}

MercedIcResult merced_icache_fetch(Merced *m, uint64_t pa, bool wb,
                                   uint64_t *lo, uint64_t *hi)
{
    MercedCache *mc = &m->cache;
    if (!wb || (mc->icctl & IC_DISABLE)) { return MERCED_IC_MISS; }
    const unsigned set = (unsigned)(pa >> 5) & (MC_L1I_SETS - 1u);
    MercedL1IWay *ways = mc->l1i[set];
    const unsigned b = (unsigned)(pa >> 4) & 1u;
    bool bad = false;
    unsigned hw = MC_L1I_WAYS;
    for (unsigned w = 0; w < MC_L1I_WAYS; w++) {
        const MercedL1IWay *x = &ways[w];
        if (!x->valid) { continue; }
        if (x->tag_bad) { bad = true; }
        if (x->tag == (uint32_t)(pa >> 12)) { hw = w; }
    }
    MercedL1IWay *hit = hw < MC_L1I_WAYS ? &ways[hw] : NULL;
    if (hit && hit->planted && !bad && ((hit->word_bad >> (2u * b)) & 3u)) { bad = true; }
    if (bad) {
        if ((mc->icctl & IC_REPORT) && !(m->psr & PSR_MC)) {
            m->msr[MSR_IC_LOG] |= IC_LOG_ERROR;
            m->msr_written[MSR_IC_LOG] = true;
            mc->mca_pending = true;
            return MERCED_IC_MCA;
        }
        for (unsigned w = 0; w < MC_L1I_WAYS; w++) { icache_set_state(m, mc, &ways[w], false, false); }
        hit = NULL;
    }
    if (hit) { mc->plru[set] = plru_touch(mc->plru[set], hw); }
    if (mc->ic_known && ++mc->ic_fetches > IC_TRACK_FETCHES) {
        icache_untracked(m);
    }
    if (!hit && mc->ic_known) {
        /* A fill: the victim takes the line, tagged with good parity. */
        const unsigned v = plru_victim(mc->plru[set]);
        MercedL1IWay *x = &ways[v];
        x->tag = (uint32_t)(pa >> 12);
        x->tpar = !parity_odd(x->tag, 0);
        icache_set_state(m, mc, x, true, false);
        mc->plru[set] = plru_touch(mc->plru[set], v);
    }
    if (!hit || !hit->planted) { return MERCED_IC_MISS; }
    /* Decoded once per port write (icache_msr_write clears dec_ok): the
     * planted chains run their bundles many times. */
    if (!(hit->dec_ok & (1u << b))) {
        uint64_t out[2] = { 0, 0 };
        for (unsigned k = 0; k < 2; k++) {
            const uint64_t word = hit->word[2u * b + k];
            for (unsigned i = 0; i < 64; i++) {
                const unsigned n = g_ic_perm[k][i];
                out[n >> 6] |= ((word >> i) & 1u) << (n & 63u);
            }
        }
        hit->dec[b][0] = out[0];
        hit->dec[b][1] = out[1];
        hit->dec_ok |= (uint8_t)(1u << b);
    }
    *lo = hit->dec[b][0];
    *hi = hit->dec[b][1];
    return MERCED_IC_HIT;
}

/* ---- L1 data cache -------------------------------------------------------
 *
 * Phase-1 test 0xFFE00C20 (failure bit 52) runs 0xFFE00840 then 0xFFE001C0.
 * Ports, through the 8-bit-command form of the helper (0xFFDC6DB0): set in
 * 10:0, way 13:11, sub-field 18:14 for a write, payload from bit 32. A read
 * returns its result in the low bits of the 32-bit half that holds the
 * command (0xFFDC7020, 0xFFDC7450: bits 0.. with the command in 31:24,
 * 32.. with it in 63:56).
 *   5D0h tag (0xFFDC6F50): sub 0 tag 15:0 + parity in 16, sub 1 tag 31:16
 *        + parity; tag = PA >> 12, parity even (popcount & 1, 0xFFE00980).
 *   5D8h data (0xFFDC75C0, read 0xFFDC7450): sub = 16-bit chunk 0..15
 *        (qword k/4, bits 16(k%4)+15..), payload 15:0 and two parity bits in
 *        17:16 -- even parity of each of the chunk's bytes: the test's
 *        0x354D5969 is exactly that for its four qwords.
 *   5D1h valid (0xFFDC7130, read-modify-write): entry set & 3Fh, bit way +
 *        4 * set bit 6.
 *   5D2h (0xFFDC7290/7360) and 5D3h (0xFFDC7820: indexed by set and a hash
 *        of PA 27:11, 3-bit entries of valid + way -- a way predictor) are
 *        held and read back but play no part in a lookup here: the tests
 *        always write a matching entry, so whether hardware needs it is not
 *        shown.
 * 580h: bit 40 is the L1D invalidate -- 0xFFDC01E0 pulses it after the test,
 * PAL_A pulses it at reset (0xFFFF9740), and the L2 tests hold it so their
 * loads reach the L2. Bits 15:10 disable the L1D (0xFFDC0310: enable clears
 * them; PAL runs with them set, SAL clears them for good). Any write with
 * bit 40 set drops every line, but holding it does not hide the L1D:
 * 0xFFE00840 plants its second set's line with 580h still 0x50000000000
 * and requires the hit. So a line is visible while 15:10 are clear.
 * 0xFFE00840 plants lines over its own (cacheable alias) code and
 * requires ld8 of them to return the planted qwords. 0xFFE001C0 plants one
 * with a byte's parity flipped (ld1 of that byte), or a tag half's, and
 * expects PALE_CHECK with r9 = 0x100 (5C8h bit 0) and 5C8h = 0x1D0 after the
 * handler cleared 1:0, i.e. 0x1D1.
 * FILLS (phase-2 test 0xFFE0E100): it stores four values to four pages of
 * one set, flushes them, writes 5D2h = 0 for the set, loads them back and
 * reads the arrays: all four ways valid, holding its values in ways 0, 2,
 * 1, 3 -- the I-cache's tree pseudo-LRU again. As there, fills are kept as
 * tags and replacement state only: a filled line's words are read from
 * memory when the port asks, and loads of it go to memory; only planted
 * lines serve their own words. They are followed from an invalidate while
 * the L1D is enabled, for L1D_TRACK_LOADS cacheable loads (the model
 * observes meanwhile, so the JIT stands aside) -- the bound is the model's, as
 * for the I-cache: PAL's tests read the arrays a few loads after their
 * invalidate, and SAL enables the L1D for good after the last one.
 * Merced's FP loads bypass the L1D; this sees every cacheable integer and
 * FP load alike. Stores write through: a hit on a planted line updates it,
 * and the store continues to memory; there is no write-allocate here.
 */
#define MSR_L1D_LOG  0x5C8u
#define L1D_LOG_ERR  UINT64_C(0x1D1)
/* Phase-2 test 0xFFE12660 plants a tag one bit away from the address it
 * loads, with the parity of the right one: a parity error on a way that
 * does not match, which it expects as 5C8h = 0x111 (0x110 after the
 * handler) -- so every valid way's tag is checked, and 7:6 mark the hit. */
#define L1D_LOG_TAG_MISS UINT64_C(0x111)

static bool l1d_valid(const MercedCache *mc, unsigned set, unsigned way)
{
    return (mc->l1d_valid[set & 0x3Fu] >> (way + 4u * ((set >> 6) & 1u))) & 1u;
}

static void l1d_set_valid(MercedCache *mc, unsigned set, unsigned way, bool v)
{
    const uint8_t bit = (uint8_t)(1u << (way + 4u * ((set >> 6) & 1u)));
    if (v) { mc->l1d_valid[set & 0x3Fu] |= bit; } else { mc->l1d_valid[set & 0x3Fu] &= (uint8_t)~bit; }
}

static void l1d_recount(Merced *m, MercedCache *mc)
{
    unsigned n = 0, p = 0;
    for (unsigned s = 0; s < MC_L1D_SETS; s++) {
        for (unsigned w = 0; w < MC_L1D_WAYS; w++) {
            if (!l1d_valid(mc, s, w)) { continue; }
            n++;
            if (mc->l1d[s][w].planted) { p++; }
        }
    }
    mc->nl1d = n;
    mc->nl1d_planted = p;
    update_observe(m, mc);
}

static void l1d_invalidate_all(Merced *m, MercedCache *mc)
{
    memset(mc->l1d_valid, 0, sizeof mc->l1d_valid);
    mc->nl1d = 0;            /* the caller updates the observation */
    mc->nl1d_planted = 0;
    mc->l1d_known = true;
    mc->l1d_t0 = now_bundles(m);
    mc->l1d_loads = 0;
}

static uint8_t byte_parity(uint64_t q);

/* A filled line's words are memory's: what the port reads, and what it
 * starts from when it writes part of the line. Only a valid filled line
 * names memory (a fill is a cacheable load); any other way keeps what the
 * array holds. */
static void l1d_own(Merced *m, MercedCache *mc, MercedL1DWay *w, unsigned set, unsigned way)
{
    if (w->planted) { return; }
    w->planted = true;
    if (!l1d_valid(mc, set, way)) { return; }
    const uint64_t pa = ((uint64_t)w->tag << 12) | ((uint64_t)set << 5);
    for (unsigned q = 0; q < 4; q++) {
        const uint64_t d = below_l1_qword(m, mc, pa + 8u * q);
        for (unsigned k = 0; k < 4; k++) { w->chunk[4u * q + k] = (uint16_t)(d >> (16u * k)); }
        w->par[q] = byte_parity(d);
    }
}

static uint8_t byte_parity(uint64_t q)
{
    uint8_t p = 0;
    for (unsigned b = 0; b < 8; b++) {
        p |= (uint8_t)((__builtin_popcount((unsigned)(q >> (8u * b)) & 0xFFu) & 1u) << b);
    }
    return p;
}

static uint64_t l1d_qword(const MercedL1DWay *w, unsigned q)
{
    return (uint64_t)w->chunk[4u * q] | ((uint64_t)w->chunk[4u * q + 1u] << 16) |
           ((uint64_t)w->chunk[4u * q + 2u] << 32) | ((uint64_t)w->chunk[4u * q + 3u] << 48);
}

static void l1d_msr_write(Merced *m, unsigned mi, uint64_t v)
{
    MercedCache *mc = &m->cache;
    const unsigned set = (unsigned)(v & 0x7FFu) % MC_L1D_SETS, way = cmd_way(v);
    const bool read = cmd_is_read(v);
    const unsigned sub = read ? (unsigned)((v >> 19) & 0x1Fu) : cmd_sub(v);
    const uint32_t pay = (uint32_t)(v >> 32);
    MercedL1DWay *w = way < MC_L1D_WAYS ? &mc->l1d[set][way] : NULL;
    uint64_t r = 0, rmask = 0;
    switch (mi) {
    case MSR_L1D_TAG:
        if (!w) { return; }
        if (read) {
            r = ((sub ? w->tag >> 16 : w->tag) & 0xFFFFu) | ((uint64_t)((w->tpar >> (sub ? 1 : 0)) & 1u) << 16);
            rmask = 0x1FFFF;
            break;
        }
        l1d_own(m, mc, w, set, way);
        if (sub == 0u) {
            w->tag = (w->tag & UINT32_C(0xFFFF0000)) | (pay & 0xFFFFu);
            w->tpar = (uint8_t)((w->tpar & 2u) | ((pay >> 16) & 1u));
        } else {
            w->tag = (w->tag & UINT32_C(0xFFFF)) | ((pay & 0xFFFFu) << 16);
            w->tpar = (uint8_t)((w->tpar & 1u) | (((pay >> 16) & 1u) << 1));
        }
        l1d_recount(m, mc);
        break;
    case MSR_L1D_VALID:
        if (read) { r = mc->l1d_valid[set & 0x3Fu]; rmask = 0xFF; }
        else      { mc->l1d_valid[set & 0x3Fu] = (uint8_t)pay; l1d_recount(m, mc); }
        break;
    case MSR_L1D_LRU:
        /* Entry set & 3Fh: 2:0 the replacement state of that set, 5:3 of
         * set + 40h (INFERRED, after 5D1h's layout; PAL writes 0 to 2:0 of
         * the set it fills, 0xFFE0E100 via 0xFFDC7360). */
        if (read) {
            r = (uint64_t)(mc->l1d_plru[set & 0x3Fu] & 7u) |
                ((uint64_t)(mc->l1d_plru[(set & 0x3Fu) | 0x40u] & 7u) << 3);
            rmask = 0x3F;
        } else {
            mc->l1d_plru[set & 0x3Fu] = (uint8_t)(pay & 7u);
            mc->l1d_plru[(set & 0x3Fu) | 0x40u] = (uint8_t)((pay >> 3) & 7u);
        }
        break;
    case MSR_L1D_WP:
        if (read) { r = mc->l1d_wp[set][sub & 7u]; rmask = 0xFFF; }
        else      { mc->l1d_wp[set][sub & 7u] = (uint16_t)(pay & 0xFFFu); }
        break;
    case MSR_L1D_DATA: {
        if (!w || sub >= MC_L1D_CHUNKS) { return; }
        const unsigned q = sub >> 2, sh = 2u * (sub & 3u);
        const unsigned cmd = (unsigned)(((v >> 24) | (v >> 56)) & 0xFFu);
        if (read) {
            MercedL1DWay tmp = *w;
            l1d_own(m, mc, &tmp, set, way);
            r = tmp.chunk[sub] | ((uint64_t)((tmp.par[q] >> sh) & 3u) << 16);
            rmask = 0x3FFFF;
        } else if ((cmd & 3u) != 1u) {
            /* Only command 01h (0xFFDC75C0) is known to write the data;
             * 0xFFE0D520 also writes 10h and 40h here. */
            static uint8_t said[256];
            if (!said[cmd]) {
                said[cmd] = 1;
                fprintf(stderr, "merced: msr[0x5d8] L1D array write command 0x%02x "
                        "(0x%016" PRIX64 ") ignored ip=0x%016" PRIX64 " (unmodelled)\n",
                        cmd, v, m->ip);
            }
        } else {
            l1d_own(m, mc, w, set, way);
            w->chunk[sub] = (uint16_t)pay;
            w->par[q] = (uint8_t)((w->par[q] & ~(3u << sh)) | (((pay >> 16) & 3u) << sh));
            l1d_recount(m, mc);
        }
        break;
    }
    default:
        return;
    }
    if (read) {
        const unsigned at = ((v >> 24) & 0xFFu) ? 0u : 32u;
        m->msr[mi] = (v & ~(rmask << at)) | (r << at);
    }
}

/* -1: not an L1D hit, the caller goes on; 1: served (or a machine check
 * raised -- *raw is then 0 and the instruction does not complete). */
static int l1d_load(Merced *m, MercedCache *mc, uint64_t pa, unsigned size, uint64_t *raw)
{
    if ((mc->l2mode & L1D_DISABLE) || ((pa & 7u) + size) > 8u) { return -1; }
    const unsigned set = (unsigned)(pa >> 5) & (MC_L1D_SETS - 1u);
    const unsigned q = (unsigned)(pa >> 3) & 3u;
    const MercedL1DWay *hit = NULL;
    unsigned hw = 0;
    bool bad_hit = false, bad_other = false;
    for (unsigned way = 0; way < MC_L1D_WAYS; way++) {
        const MercedL1DWay *w = &mc->l1d[set][way];
        if (!l1d_valid(mc, set, way)) { continue; }
        const uint8_t tp = (uint8_t)((__builtin_popcount(w->tag & 0xFFFFu) & 1u) |
                                     ((__builtin_popcount(w->tag >> 16) & 1u) << 1));
        if (w->tag == (uint32_t)(pa >> 12)) {
            hit = w;
            hw = way;
            bad_hit = tp != w->tpar ||
                      (w->planted && byte_parity(l1d_qword(w, q)) != w->par[q]);
        } else if (tp != w->tpar) {
            bad_other = true;
        }
    }
    if ((bad_hit || bad_other) && !(m->psr & PSR_MC)) {
        m->msr[MSR_L1D_LOG] = bad_hit ? L1D_LOG_ERR : L1D_LOG_TAG_MISS;
        m->msr_written[MSR_L1D_LOG] = true;
        mc->mca_pending = true;
        *raw = 0;
        return 1;
    }
    /* Fills (test 0xFFE0E100): while followed, a miss takes the victim way,
     * tagged with good parity; a hit or fill updates the replacement state. */
    if (l1d_tracking(mc)) {
        if (++mc->l1d_loads >= L1D_TRACK_LOADS) { mc->l1d_known = false; update_observe(m, mc); }
        if (!hit) {
            const unsigned v = plru_victim(mc->l1d_plru[set]);
            MercedL1DWay *x = &mc->l1d[set][v];
            x->tag = (uint32_t)(pa >> 12);
            x->tpar = (uint8_t)((__builtin_popcount(x->tag & 0xFFFFu) & 1u) |
                                ((__builtin_popcount(x->tag >> 16) & 1u) << 1));
            x->planted = false;
            l1d_set_valid(mc, set, v, true);
            l1d_recount(m, mc);
            mc->l1d_plru[set] = plru_touch(mc->l1d_plru[set], v);
            return -1;          /* the load itself reads memory */
        }
    }
    if (hit) { mc->l1d_plru[set] = plru_touch(mc->l1d_plru[set], hw); }
    if (!hit || !hit->planted) { return -1; }   /* a filled line is memory's */
    const uint64_t d = l1d_qword(hit, q);
    *raw = (size == 8u) ? d : ((d >> (8u * (pa & 7u))) & ((UINT64_C(1) << (8u * size)) - 1u));
    return 1;
}

static void l1d_store(MercedCache *mc, uint64_t pa, unsigned size, uint64_t raw)
{
    if (mc->l2mode & L1D_DISABLE) { return; }
    const unsigned set = (unsigned)(pa >> 5) & (MC_L1D_SETS - 1u);
    for (unsigned way = 0; way < MC_L1D_WAYS; way++) {
        MercedL1DWay *w = &mc->l1d[set][way];
        if (!l1d_valid(mc, set, way) || w->tag != (uint32_t)(pa >> 12)) { continue; }
        if (!w->planted) { return; }    /* a filled line follows memory */
        const unsigned q = (unsigned)(pa >> 3) & 3u;
        uint64_t d = l1d_qword(w, q);
        for (unsigned k = 0; k < size; k++) {
            const unsigned b = (unsigned)(pa & 7u) + k;
            d = (d & ~(UINT64_C(0xFF) << (8u * b))) | (((raw >> (8u * k)) & 0xFFu) << (8u * b));
        }
        for (unsigned k = 0; k < 4; k++) { w->chunk[4u * q + k] = (uint16_t)(d >> (16u * k)); }
        w->par[q] = byte_parity(d);
        return;
    }
}

/* ---- window lifetime -----------------------------------------------------
 * A fill window (l1d/l2/l3/ic_known) also ends MC_WINDOW_BUNDLES guest
 * bundles after the invalidate that opened it. Its access budget alone was
 * not enough: the budget counts only cacheable accesses reaching that
 * level, and PAL and early SAL run uncached, so an L1D window opened at
 * bundle 473M stayed open to 1145M with the JIT standing aside throughout
 * (A-step too, which runs none of the tests). PAL's tests read the arrays
 * within thousands of bundles of their invalidate. The bound is the
 * model's, like the budget. */
#define MC_WINDOW_BUNDLES UINT64_C(4000000)

void merced_cache_tick(Merced *m)
{
    MercedCache *mc = &m->cache;
    const uint64_t now = now_bundles(m);
    bool changed = false;
    if (mc->l1d_known && now - mc->l1d_t0 > MC_WINDOW_BUNDLES) { mc->l1d_known = false; changed = true; }
    if (mc->l2_known && now - mc->l2_t0 > MC_WINDOW_BUNDLES) {
        mc->l2_known = false;
        l2_untrack(m, mc);
        changed = true;
    }
    if (mc->l3_known && now - mc->l3_t0 > MC_WINDOW_BUNDLES) {
        mc->l3_known = false;
        l3_untrack(m, mc);
        changed = true;
    }
    if (mc->ic_known && now - mc->ic_t0 > MC_WINDOW_BUNDLES) { icache_untracked(m); }
    if (changed) { update_observe(m, mc); }
}
