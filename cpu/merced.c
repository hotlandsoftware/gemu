/*
 * Intel Itanium (Merced) IA-64 interpreter.
 *
 * Encodings from the Itanium SDM rev 2.1, cross-checked against HP Ski's
 * machine-generated tables (reference/ski/src/encodings/encoding.opcode,
 * encoding.format, encoding.imm). See merced.h for modeling simplifications.
 */

#include "merced.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Field helpers ───────────────────────────────────────────────────────── */

static inline uint64_t bits(uint64_t v, unsigned lo, unsigned w) {
    return (v >> lo) & ((w >= 64) ? ~0ull : ((1ull << w) - 1));
}
static inline int64_t sext(uint64_t v, unsigned w) {
    uint64_t m = 1ull << (w - 1);
    return (int64_t)((v ^ m) - m);
}

/* Verbose boot tracing (TLB fills, PSR transitions, fault vectors) is gated
 * on the MERCED_DEBUG env var so ordinary runs stay quiet. */
static int merced_dbg(void) {
    static int cached = -1;
    if (cached < 0) cached = getenv("MERCED_DEBUG") != NULL;
    return cached;
}

/* ── PSR / CFM / AR / CR layout ──────────────────────────────────────────── */

#define PSR_BE   (1ull << 1)
#define PSR_AC   (1ull << 3)
#define PSR_IC   (1ull << 13)
#define PSR_I    (1ull << 14)
#define PSR_DT   (1ull << 17)
#define PSR_DI   (1ull << 22)
#define PSR_RT   (1ull << 27)
#define PSR_IS   (1ull << 34)
#define PSR_DA   (1ull << 38)
#define PSR_DD   (1ull << 39)
#define PSR_IT   (1ull << 36)
#define PSR_ED   (1ull << 43)
#define PSR_BN   (1ull << 44)
#define PSR_IA   (1ull << 45)
#define PSR_RI_SHIFT 41

#define CFM_SOF(c)    ((unsigned)((c) & 0x7F))
#define CFM_SOL(c)    ((unsigned)(((c) >> 7) & 0x7F))
#define CFM_SOR(c)    ((unsigned)(((c) >> 14) & 0xF))
#define CFM_RRB_GR(c) ((unsigned)(((c) >> 18) & 0x7F))
#define CFM_RRB_FR(c) ((unsigned)(((c) >> 25) & 0x7F))
#define CFM_RRB_PR(c) ((unsigned)(((c) >> 32) & 0x3F))
#define CFM_MASK      0x3FFFFFFFFFull

enum {
    AR_KR0 = 0, AR_RSC = 16, AR_BSP = 17, AR_BSPSTORE = 18, AR_RNAT = 19,
    AR_CCV = 32, AR_UNAT = 36, AR_FPSR = 40, AR_ITC = 44,
    AR_PFS = 64, AR_LC = 65, AR_EC = 66,
};

enum {
    CR_DCR = 0, CR_ITM = 1, CR_IVA = 2, CR_PTA = 8,
    CR_IPSR = 16, CR_ISR = 17, CR_IIP = 19, CR_IFA = 20, CR_ITIR = 21,
    CR_IIPA = 22, CR_IFS = 23, CR_IIM = 24, CR_IHA = 25,
    CR_LID = 64, CR_IVR = 65, CR_TPR = 66, CR_EOI = 67,
    CR_IRR0 = 68, CR_ITV = 72, CR_PMV = 73, CR_CMCV = 74,
    CR_LRR0 = 80, CR_LRR1 = 81,
};

/* Interruption vector offsets (IVA-relative). */
enum {
    VEC_VHPT = 0x0000, VEC_ITLB = 0x0400, VEC_DTLB = 0x0800,
    VEC_ALT_ITLB = 0x0C00, VEC_ALT_DTLB = 0x1000, VEC_NESTED_DTLB = 0x1400,
    VEC_DIRTY = 0x2000, VEC_IACCESS = 0x2400, VEC_DACCESS = 0x2800,
    VEC_BREAK = 0x2C00, VEC_EXTINT = 0x3000,
    VEC_PAGE_NOT_PRESENT = 0x5000, VEC_GENERAL = 0x5400,
    VEC_NAT = 0x5600, VEC_SPEC = 0x5700, VEC_UNALIGNED = 0x5A00,
};

/* one-shot warning ids */
enum {
    WARN_FLUSHRS = 1u << 0, WARN_PTC = 1u << 1, WARN_PROBE = 1u << 2,
    WARN_SEMAPHORE = 1u << 3, WARN_LFETCH_FAULT = 1u << 4,
    WARN_FP_APPROX = 1u << 5,
};

static void warn_once(Merced *m, uint32_t bit, const char *msg) {
    if (!(m->warned & bit)) {
        m->warned |= bit;
        fprintf(stderr, "merced: note: %s (IP=0x%016" PRIX64 ")\n", msg, m->ip);
    }
}

/* ── Register files ──────────────────────────────────────────────────────── */

static unsigned stacked_phys(const Merced *m, unsigned r) {
    unsigned idx = r - 32;
    unsigned sor8 = CFM_SOR(m->cfm) * 8;
    if (sor8 && idx < sor8)
        idx = (idx + CFM_RRB_GR(m->cfm)) % sor8;
    return (m->bof + idx) % MERCED_N_STACKED;
}

static uint64_t gr_read(Merced *m, unsigned r, uint8_t *nat) {
    if (nat) *nat = 0;
    if (r == 0) return 0;
    if (r < 16) { if (nat) *nat = m->nat_static[r]; return m->gr_static[r]; }
    if (r < 32) {
        if (m->psr & PSR_BN) { if (nat) *nat = m->nat_static[r]; return m->gr_static[r]; }
        if (nat) *nat = m->nat_bank0[r - 16];
        return m->gr_bank0[r - 16];
    }
    unsigned p = stacked_phys(m, r);
    if (nat) *nat = m->nat_stack[p];
    return m->gr_stack[p];
}

static void gr_write(Merced *m, unsigned r, uint64_t v, uint8_t nat) {
    if (r == 0) return;   /* writes to r0 fault on HW; ignore here */
    if (r < 16) { m->gr_static[r] = v; m->nat_static[r] = nat; return; }
    if (r < 32) {
        if (m->psr & PSR_BN) { m->gr_static[r] = v; m->nat_static[r] = nat; }
        else {
            m->gr_bank0[r - 16] = v; m->nat_bank0[r - 16] = nat;
        }
        return;
    }
    unsigned p = stacked_phys(m, r);
    m->gr_stack[p] = v;
    m->nat_stack[p] = nat;
}

static unsigned pr_phys(const Merced *m, unsigned i) {
    if (i < 16) return i;
    return 16 + ((i - 16 + CFM_RRB_PR(m->cfm)) % 48);
}
static int pr_read(Merced *m, unsigned i) {
    if (i == 0) return 1;
    return (m->pr >> pr_phys(m, i)) & 1;
}
static void pr_write(Merced *m, unsigned i, int v) {
    if (i == 0) return;
    unsigned p = pr_phys(m, i);
    if (v) m->pr |= 1ull << p; else m->pr &= ~(1ull << p);
}

static unsigned fr_phys(const Merced *m, unsigned r) {
    if (r < 32) return r;
    return 32 + ((r - 32 + CFM_RRB_FR(m->cfm)) % 96);
}
static MercedFpReg fr_read(Merced *m, unsigned r) {
    if (r == 0) { MercedFpReg z = {0, 0, 0, 0}; return z; }
    if (r == 1) { MercedFpReg one = {0x8000000000000000ull, 0xFFFF, 0, 0}; return one; }
    return m->fr[fr_phys(m, r)];
}
static void fr_write(Merced *m, unsigned r, MercedFpReg v) {
    if (r < 2) return;
    m->fr[fr_phys(m, r)] = v;
}

/* FP <-> double conversion (approximate: fine for firmware arithmetic,
 * not IEEE-corner exact). */
static long double fp2d(MercedFpReg f) {
    if (f.exp == 0 && f.sig == 0) return f.sign ? -0.0 : 0.0;
    if (f.exp == 0x1003E) {   /* integer form */
        long double d = (long double)f.sig;
        return f.sign ? -d : d;
    }
    int e = (int)f.exp - 0xFFFF;
    long double d = (long double)f.sig * 0x1p-63L * 0.5L; /* explicit int bit */
    d = d * 2.0;
    /* build 2^e */
    long double p = 1.0L;
    int n = e < 0 ? -e : e;
    long double b = 2.0L;
    while (n) { if (n & 1) p *= b; b *= b; n >>= 1; }
    d = e < 0 ? d / p : d * p;
    return f.sign ? -d : d;
}
static MercedFpReg d2fp(long double d) {
    MercedFpReg f = {0, 0, 0, 0};
    if (d == 0.0) return f;
    if (d < 0) { f.sign = 1; d = -d; }
    int e = 0;
    while (d >= 2.0L) { d *= 0.5L; e++; }
    while (d < 1.0L) { d *= 2.0L; e--; }
    long double scaled = d * 0x1p63L;
    uint64_t sig = (uint64_t)scaled;
    long double fraction = scaled - (long double)sig;
    if (fraction > 0.5L || (fraction == 0.5L && (sig & 1))) {
        sig++;
        if (sig == 0) {
            sig = UINT64_C(0x8000000000000000);
            e++;
        }
    }
    f.sig = sig;
    f.exp = (uint32_t)(e + 0xFFFF);
    return f;
}

/* Round an FMA-family result to the instruction's static precision before
 * converting it back to the Itanium floating-point register format.  The
 * register format has a 64-bit significand, but .s and .d instructions must
 * not retain those extra bits: later Newton iterations (and, in particular,
 * firmware decompression code) observe the rounded intermediate result.
 *
 * fp_result_static() is used for FMA-family results. fp_recip_estimate()
 * remains available for the eventual table-accurate frcpa implementation;
 * frcpa currently uses a full-precision reciprocal so its surrounding
 * Newton-Raphson sequence remains stable while the rest of FP exception and
 * rounding behavior is still incomplete. */
static MercedFpReg fp_result_static(long double d, unsigned precision) {
    if (precision == 1) {
        return d2fp((long double)(float)d);
    }
    if (precision == 2) {
        return d2fp((long double)(double)d);
    }
    return d2fp(d);
}

/* Merced frcpa's architected 8-bit reciprocal estimate.  Ski expresses this
 * as a 256-entry table indexed by significand bits 62:55.  The table is
 * exactly round(2^20 / (513 + 2*index)), so keep the equivalent integer form
 * here instead of embedding 256 constants. */
static MercedFpReg fp_recip_estimate(MercedFpReg den) {
    unsigned index = (unsigned)((den.sig >> 55) & 0xff);
    unsigned divisor = 513 + 2 * index;
    uint64_t estimate = (1048576u + divisor / 2) / divisor;
    MercedFpReg r = {
        .sig = estimate << 53,
        .exp = 0x1fffdU - den.exp,
        .sign = den.sign,
        .nat = den.nat,
    };
    return r;
}

/* ── Memory / translation ────────────────────────────────────────────────── */

static MercedStatus mhalt(Merced *m, const char *fmt, ...);

static const MercedTlbEntry *tlb_search(const MercedTlbEntry *t, int n,
                                        uint32_t rid, uint64_t va) {
    for (int i = 0; i < n; i++) {
        if (t[i].valid && t[i].rid == rid &&
            va >= t[i].va_start && va <= t[i].va_end)
            return &t[i];
    }
    return NULL;
}

/* Returns true and sets *pa on success; on failure delivers a fault (or
 * halts) and returns false with *st set. */
static bool va_translate(Merced *m, uint64_t va, bool ifetch, bool spec,
                         uint64_t *pa, MercedStatus *st);

/* Advance ar.itc and edge-latch the interval-timer interrupt if it just
 * crossed cr.itm. cr.itm == 0 means "never armed" (SAL/EFI haven't
 * programmed a deadline yet), matching real reset state where the timer
 * isn't running until software sets it. */
static void itc_advance(Merced *m, uint64_t delta) {
    m->ar[AR_ITC] += delta;
    if (m->cr[CR_ITM] != 0 && m->ar[AR_ITC] >= m->cr[CR_ITM])
        m->timer_pending = 1;
}

void merced_raise_external(Merced *m, uint8_t vector) {
    if (!m->external_pending) {
        m->external_vector = vector;
        m->external_pending = 1;
    }
}

static MercedStatus deliver_fault(Merced *m, uint32_t vec, uint64_t isr,
                                  uint64_t ifa, bool set_ifa) {
    m->nfaults++;
    if (merced_dbg() &&
        (vec == VEC_ITLB || vec == VEC_DTLB ||
         vec == VEC_ALT_ITLB || vec == VEC_ALT_DTLB) && m->nfaults <= 16) {
        fprintf(stderr, "merced: TLB fault #%-2" PRIu64
                " vec=%04X ip=%016" PRIX64 " ifa=%016" PRIX64
                " ic=%u it=%u dt=%u pta=%016" PRIX64 "\n",
                m->nfaults, vec, m->ip, ifa,
                !!(m->psr & PSR_IC), !!(m->psr & PSR_IT),
                !!(m->psr & PSR_DT), m->cr[CR_PTA]);
        fprintf(stderr, "  iva=%016" PRIX64 "\n", m->cr[CR_IVA]);
        fprintf(stderr, "  rr:");
        for (unsigned i = 0; i < MERCED_N_RR; i++)
            fprintf(stderr, " %u=%016" PRIX64, i, m->rr[i]);
        fprintf(stderr, "\n  bank0:");
        for (unsigned i = 0; i < 16; i++)
            fprintf(stderr, " r%u=%016" PRIX64, i + 16, m->gr_bank0[i]);
        fprintf(stderr, "\n");
        for (unsigned i = 0; i < MERCED_N_TR; i++)
            if (m->dtr[i].valid)
                fprintf(stderr, "  dtr[%u] rid=%06X va=%016" PRIX64
                        "-%016" PRIX64 " pa=%016" PRIX64 " ps=%u\n",
                        i, m->dtr[i].rid, m->dtr[i].va_start,
                        m->dtr[i].va_end, m->dtr[i].pfn_base, m->dtr[i].ps);
        for (unsigned i = 0; i < MERCED_N_TC; i++)
            if (m->dtc[i].valid)
                fprintf(stderr, "  dtc[%u] rid=%06X va=%016" PRIX64
                        "-%016" PRIX64 " pa=%016" PRIX64 " ps=%u\n",
                        i, m->dtc[i].rid, m->dtc[i].va_start,
                        m->dtc[i].va_end, m->dtc[i].pfn_base, m->dtc[i].ps);
    }
    if (!(m->psr & PSR_IC)) {
        /* Translation faults encountered while interruption collection is
         * disabled have dedicated vectors and must not overwrite the saved
         * interruption state.  The handlers use these paths to establish a
         * mapping needed by the original miss handler. */
        if (vec == VEC_ITLB || vec == VEC_ALT_ITLB ||
            vec == VEC_DTLB || vec == VEC_ALT_DTLB) {
            bool instr = (vec == VEC_ITLB || vec == VEC_ALT_ITLB);
            m->cr[CR_ISR] = isr | (1ull << 39); /* ni: nested interruption */
            if (set_ifa)
                m->cr[CR_IFA] = ifa;
            m->ip = (m->cr[CR_IVA] & ~0x7FFFull) +
                    (instr ? VEC_ALT_ITLB : VEC_NESTED_DTLB);
            m->taken = 1;
            return MERCED_OK;
        }
        return mhalt(m, "fault vec=0x%X with PSR.ic=0 (ifa=0x%016" PRIX64 ")",
                     vec, ifa);
    }
    bool ia32 = (m->psr & PSR_IS) != 0;
    unsigned slot = ia32 ? 0 : (unsigned)(m->ip & 3);
    m->cr[CR_IPSR] = m->psr | ((uint64_t)slot << PSR_RI_SHIFT);
    m->cr[CR_IIP]  = ia32 ? (uint32_t)m->ip : (m->ip & ~0xFull);
    m->cr[CR_ISR]  = isr | ((uint64_t)slot << 41);
    m->cr[CR_IIPA] = ia32 ? (uint32_t)m->ip : (m->ip & ~0xFull);
    m->cr[CR_IFS] &= ~(1ull << 63);
    if (set_ifa) {
        m->cr[CR_IFA] = ifa;
        unsigned vrn = (unsigned)(ifa >> 61);
        uint64_t rr = m->rr[vrn];
        m->cr[CR_ITIR] = (rr & 0xFCu) | (((rr >> 8) & 0xFFFFFF) << 8);
        /* short-format VHPT hash for cr.iha */
        uint64_t pta = m->cr[CR_PTA];
        unsigned ps = (unsigned)((rr >> 2) & 0x3F);
        unsigned sz = (unsigned)((pta >> 2) & 0x3F);
        uint64_t mask = (sz >= 64) ? ~0ull : ((1ull << sz) - 1);
        uint64_t off = ((ifa & 0x1FFFFFFFFFFFFFFFull) >> ps) << 3;
        m->cr[CR_IHA] = ((uint64_t)vrn << 61) |
                        ((pta & ~0x7FFull & ~mask) | (off & mask & ~0x7ull));
    }
    /* Every interruption enters the IA-64 instruction set.  IPSR above
     * retains the interrupted PSR.is value so rfi can return to IA-32. */
    m->psr &= ~(PSR_IC | PSR_I | PSR_BN | PSR_IS);
    m->psr &= ~(3ull << PSR_RI_SHIFT);
    m->ip = (m->cr[CR_IVA] & ~0x7FFFull) + vec;
    m->taken = 1;
    return MERCED_OK;
}

static bool va_translate(Merced *m, uint64_t va, bool ifetch, bool spec,
                         uint64_t *pa, MercedStatus *st) {
    *st = MERCED_OK;
    bool on = ifetch ? (m->psr & PSR_IT) != 0 : (m->psr & PSR_DT) != 0;
    if (!on) {
        *pa = va & MERCED_PHYS_MASK;
        return true;
    }
    unsigned vrn = (unsigned)(va >> 61);
    uint32_t rid = (uint32_t)((m->rr[vrn] >> 8) & 0xFFFFFF);
    uint64_t lookup_va = va;
    /* PAL leaves a bootstrap identity mapping over the firmware ROM: code
     * keeps executing through the top-of-4-GiB alias after SAL enables
     * translation, with only its RAM-shadow ranges in the visible TRs.
     * Model that as a fixed ifetch window straight to the ROM PA. */
    if (ifetch && va >= 0xFFC00000ull && va <= 0xFFFFFFFFull) {
        *pa = va & MERCED_PHYS_MASK;
        return true;
    }
    /* ... and matching pinned data translations for the firmware range and
     * the I/O port block (SAL hand-off state: firmware code/data and I/O
     * port space stay accessible in virtual mode). The I/O window also
     * matches the region-4 alias the firmware uses for UC accesses. */
    if (!ifetch) {
        uint64_t v61 = va & 0x1FFFFFFFFFFFFFFFull;
        if ((va >= 0xFFC00000ull && va <= 0xFFFFFFFFull) ||
            v61 - 0xFFFFC000000ull < 0x4000000ull) {
            *pa = v61 & MERCED_PHYS_MASK;
            return true;
        }
    }
    const MercedTlbEntry *e =
        tlb_search(ifetch ? m->itr : m->dtr, MERCED_N_TR, rid, lookup_va);
    if (!e)
        e = tlb_search(ifetch ? m->itc : m->dtc, MERCED_N_TC, rid, lookup_va);
    if (!e && !ifetch) {
        /* Merced-style unified behavior: the i2000 firmware maps its 4 MiB
         * shadow with itr.i only, yet reads/writes data in that region with
         * translation on (e.g. the alt-DTLB handler's own descriptor at
         * [r12+112]). Data lookups therefore fall back to the I-side
         * entries; a strictly split model dead-ends in the nested-miss
         * reporter. */
        e = tlb_search(m->itr, MERCED_N_TR, rid, lookup_va);
        if (!e)
            e = tlb_search(m->itc, MERCED_N_TC, rid, lookup_va);
    }
    if (!e) {
        if (spec) return false;   /* ld.s: caller sets NaT, no fault */
        /* Vector choice depends on whether the VHPT walker would have run
         * for this reference: pta.ve=0 or rr.ve=0 disables it, and misses
         * then raise the Alternate ITLB/DTLB vectors. (We model no walker,
         * so with it enabled we deliver the plain ITLB/DTLB miss vectors -
         * the handlers there do the VHPT search in software anyway.) */
        bool walker = (m->cr[CR_PTA] & 1) && (m->rr[vrn] & 1);
        uint32_t fvec;
        if (ifetch)
            fvec = walker ? VEC_ITLB : VEC_ALT_ITLB;
        else
            fvec = walker ? VEC_DTLB : VEC_ALT_DTLB;
        *st = deliver_fault(m, fvec, 0, va, true);
        return false;
    }
    *pa = (e->pfn_base + (lookup_va - e->va_start)) & MERCED_PHYS_MASK;
    return true;
}

static uint64_t phys_read(Merced *m, uint64_t pa, unsigned size) {
    return m->bus.read(m->bus.ud, pa & MERCED_PHYS_MASK, size);
}

static uint64_t phys_fetch(Merced *m, uint64_t pa, unsigned size) {
    pa &= MERCED_PHYS_MASK;
    return m->bus.fetch ? m->bus.fetch(m->bus.ud, pa, size)
                        : m->bus.read(m->bus.ud, pa, size);
}
static void phys_write(Merced *m, uint64_t pa, uint64_t v, unsigned size) {
    pa &= MERCED_PHYS_MASK;
    m->bus.write(m->bus.ud, pa, v, size);
}

/* ── Halt bookkeeping ────────────────────────────────────────────────────── */

#include <stdarg.h>
static MercedStatus mhalt(Merced *m, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->halt_msg, sizeof(m->halt_msg), fmt, ap);
    va_end(ap);
    m->halt_ip = m->ip;
    return MERCED_HALT_UNIMPL;
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

Merced *merced_create(const MercedBus *bus) {
    Merced *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->bus = *bus;
    merced_reset(m);
    return m;
}

void merced_destroy(Merced *m) { free(m); }

void merced_reset(Merced *m) {
    MercedBus bus = m->bus;
    uint64_t ninsts = 0;
    memset(m, 0, sizeof(*m));
    m->bus = bus;
    m->ninsts = ninsts;

    m->ip  = 0xFFFFFFB0ull;          /* PALE_RESET, 4 GiB - 0x50 */
    m->psr = 0;                      /* physical mode, bank 0 */
    m->pr  = 1;                      /* pr0 = 1 */
    m->cfm = 96;                     /* whole stacked file addressable */
    /* CPUID: "GenuineIntel". cpuid[3] is the version register:
     * {number 7:0, revision 15:8, model 23:16, family 31:24, archrev 39:32}
     * = archrev 0, family 7 (Itanium), model 0 (Merced), rev 0, number 4
     * (cpuid[0..4] implemented). Revision must match the firmware's own
     * cross-check of it in machine_i2000.c's memcard_cfg[0][2][0x05]
     * (CBN:05.2 processor descriptor) - a mismatch reads as "no
     * recognized processor" and parks SAL at FFFE2020. Revision 6 (C2
     * stepping) diverges into a firmware code path that isn't understood
     * yet (see SALE_ENTRY reason-dispatch panic in commit 69fab2c); 0
     * is what's confirmed working. */
    memcpy(&m->cpuid[0], "GenuineI", 8);
    memcpy(&m->cpuid[1], "ntel\0\0\0\0", 8);
    m->cpuid[2] = 0;
    m->cpuid[3] = (7ull << 24) | (0ull << 16) | (0ull << 8) | 4;
    m->cpuid[4] = 0;
    m->cr[CR_LID] = 0;               /* cpu 0 */
    strcpy(m->halt_msg, "never ran");
}

uint64_t merced_gr(const Merced *m, unsigned r) {
    return gr_read((Merced *)m, r, NULL);
}

/* ── Branch helpers ──────────────────────────────────────────────────────── */

static void do_call(Merced *m, unsigned b1, uint64_t target) {
    unsigned h = m->call_history_next++ % MERCED_CALL_HISTORY;
    m->call_history[h].from = m->ip;
    m->call_history[h].to = target;
    m->call_history[h].is_return = 0;
    unsigned sol = CFM_SOL(m->cfm);
    m->ar[AR_PFS] = (m->cfm & CFM_MASK) |
                    ((m->ar[AR_EC] & 0x3F) << 52) |
                    (bits(m->psr, 32, 2) << 62);
    m->br[b1] = (m->ip & ~0xFull) + 16;
    m->bof = (m->bof + sol) % MERCED_N_STACKED;
    m->bof_total += sol;
    uint64_t sof = CFM_SOF(m->cfm) - sol;
    m->cfm = sof;                    /* sol=0 sor=0 rrb=0 */
    m->ip = target;
    m->taken = 1;
}

static void do_ret(Merced *m, uint64_t target) {
    unsigned h = m->call_history_next++ % MERCED_CALL_HISTORY;
    m->call_history[h].from = m->ip;
    m->call_history[h].to = target;
    m->call_history[h].is_return = 1;
    uint64_t pfs = m->ar[AR_PFS];
    uint64_t new_cfm = pfs & CFM_MASK;
    unsigned sol = CFM_SOL(new_cfm);
    m->bof = (m->bof + MERCED_N_STACKED - sol) % MERCED_N_STACKED;
    m->bof_total -= sol;
    m->cfm = new_cfm;
    m->ar[AR_EC] = (pfs >> 52) & 0x3F;
    m->ip = target;
    m->taken = 1;
}

static void rotate_regs(Merced *m) {
    unsigned sor8 = CFM_SOR(m->cfm) * 8;
    unsigned g = CFM_RRB_GR(m->cfm), f = CFM_RRB_FR(m->cfm), p = CFM_RRB_PR(m->cfm);
    if (sor8) g = (g + sor8 - 1) % sor8; else g = 0;
    f = (f + 95) % 96;
    p = (p + 47) % 48;
    m->cfm = (m->cfm & 0x3FFFFull) |
             ((uint64_t)g << 18) | ((uint64_t)f << 25) | ((uint64_t)p << 32);
}

/* br.wtop/br.wexit engine: like ctop/cexit but the loop-continue condition
 * is the qualifying predicate qp rather than LC. Returns taken flag. */
static int do_wtop(Merced *m, int qp, int is_top) {
    int cont = qp || (m->ar[AR_EC] & 0x3F) > 1;

    /* Unlike br.ctop, the while-loop forms always stage a false predicate.
     * They rotate only while the qualifying predicate is true or epilog
     * stages remain. */
    pr_write(m, 63, 0);
    if (qp) {
        rotate_regs(m);
    } else if (m->ar[AR_EC] & 0x3F) {
        m->ar[AR_EC]--;
        rotate_regs(m);
    }
    return is_top ? cont : !cont;
}

/* br.ctop/br.cexit engine; returns taken flag.
 *
 * Per the SDM the staging predicate PR[63] is written *before* the register
 * rename base rotates, so the write targets the physical register that the
 * rotation then exposes as PR[16] on the next iteration. Writing after the
 * rotate (as an earlier version did) lands the fresh staging predicate one
 * physical slot off, desynchronising p16/p17 for a rotation and corrupting
 * software-pipelined loops. */
static int do_ctop(Merced *m, int is_top) {
    int taken = m->ar[AR_LC] != 0 || (m->ar[AR_EC] & 0x3F) > 1;
    if (m->ar[AR_LC] != 0) {
        m->ar[AR_LC]--;
        pr_write(m, 63, 1);
        rotate_regs(m);
    } else if (m->ar[AR_EC] & 0x3F) {
        m->ar[AR_EC]--;
        pr_write(m, 63, 0);
        rotate_regs(m);
    } else {
        pr_write(m, 63, 0);
    }
    return is_top ? taken : !taken;
}

/* ── Compare engine ──────────────────────────────────────────────────────── */

/* ctype: 0 = normal, 1 = unc, 2 = and, 3 = or, 4 = or.andcm */
static void set_preds(Merced *m, unsigned p1, unsigned p2, int qp, int res, int ctype) {
    switch (ctype) {
    case 0:
        if (qp) { pr_write(m, p1, res); pr_write(m, p2, !res); }
        break;
    case 1:
        pr_write(m, p1, qp ? res : 0);
        pr_write(m, p2, qp ? !res : 0);
        break;
    case 2:
        if (qp && !res) { pr_write(m, p1, 0); pr_write(m, p2, 0); }
        break;
    case 3:
        if (qp && res) { pr_write(m, p1, 1); pr_write(m, p2, 1); }
        break;
    case 4:
        if (qp && res) { pr_write(m, p1, 1); pr_write(m, p2, 0); }
        break;
    }
}

/* ── A-unit (executes on M and I) ────────────────────────────────────────── */

static int exec_alu(Merced *m, uint64_t raw, int qp, MercedStatus *st) {
    unsigned major = (unsigned)bits(raw, 37, 4);
    unsigned r1 = (unsigned)bits(raw, 6, 7);
    unsigned r2 = (unsigned)bits(raw, 13, 7);
    unsigned r3 = (unsigned)bits(raw, 20, 7);
    uint8_t n2, n3;
    *st = MERCED_OK;

    if (major == 8) {
        unsigned x2a = (unsigned)bits(raw, 34, 2);
        unsigned ve  = (unsigned)bits(raw, 33, 1);
        unsigned x4  = (unsigned)bits(raw, 29, 4);
        unsigned x2b = (unsigned)bits(raw, 27, 2);
        uint64_t b = gr_read(m, r3, &n3);
        /* A predicated-off instruction has no architectural effect, even
         * when that particular opcode is not implemented yet. */
        if (!qp) return 1;
        if (x2a == 2 || x2a == 3) {           /* A4 adds/addp4 imm14 */
            int64_t imm = sext((bits(raw, 36, 1) << 13) |
                               (bits(raw, 27, 6) << 7) | bits(raw, 13, 7), 14);
            uint64_t res = (uint64_t)imm + b;
            if (x2a == 3) res = (uint32_t)res | ((b >> 61) << 61);  /* addp4 */
            gr_write(m, r1, res, n3);
            return 1;
        }
        if (x2a == 1) {                       /* A9 parallel arithmetic */
            unsigned za = (unsigned)bits(raw, 36, 1);
            unsigned zb = (unsigned)bits(raw, 33, 1);
            unsigned width = za ? 32 : zb ? 16 : 8;
            uint64_t a = gr_read(m, r2, &n2), res = 0;
            uint8_t nat = n2 | n3;
            uint64_t mask = (width >= 64) ? ~0ull : ((1ull << width) - 1);
            uint64_t smin = 1ull << (width - 1);   /* sign bit of a lane */
            for (unsigned sh = 0; sh < 64; sh += width) {
                int64_t la = (int64_t)sext((a >> sh) & mask, width);
                int64_t lb = (int64_t)sext((b >> sh) & mask, width);
                uint64_t ua = (a >> sh) & mask, ub = (b >> sh) & mask;
                uint64_t r;
                switch (x4) {
                case 0:                        /* padd */
                    if (x2b == 0) r = (ua + ub) & mask;
                    else if (x2b == 1) {       /* sss: signed saturate */
                        int64_t s = la + lb;
                        int64_t hi = (int64_t)(smin - 1), lo = -(int64_t)smin;
                        r = (uint64_t)(s > hi ? hi : s < lo ? lo : s) & mask;
                    } else {                   /* uuu / uus: unsigned saturate */
                        uint64_t s = ua + ub;
                        r = s > mask ? mask : s;
                    }
                    break;
                case 1:                        /* psub */
                    if (x2b == 0) r = (ua - ub) & mask;
                    else if (x2b == 1) {       /* sss */
                        int64_t s = la - lb;
                        int64_t hi = (int64_t)(smin - 1), lo = -(int64_t)smin;
                        r = (uint64_t)(s > hi ? hi : s < lo ? lo : s) & mask;
                    } else {                   /* uuu / uus */
                        r = ua < ub ? 0 : (ua - ub);
                    }
                    break;
                case 2:                        /* pavg (+raz rounds up) */
                    r = ((ua + ub + (x2b == 3 ? 1 : 0)) >> 1) & mask;
                    break;
                case 3:                        /* pavgsub */
                    { int64_t s = (la - lb); r = (uint64_t)(s >> 1) & mask; }
                    break;
                case 9:                        /* pcmp: all-ones if true */
                    if (x2b == 0) r = (ua == ub) ? mask : 0;      /* eq */
                    else r = (la > lb) ? mask : 0;                /* gt */
                    break;
                default:
                    *st = mhalt(m, "unimpl A9 parallel x4=%X x2b=%u", x4, x2b);
                    return 0;
                }
                res |= (r & mask) << sh;
            }
            gr_write(m, r1, res, nat);
            return 1;
        }
        if (ve != 0) { *st = mhalt(m, "A-unit ve=1 reserved"); return 0; }
        uint64_t a = gr_read(m, r2, &n2);
        uint8_t nat = n2 | n3;
        uint64_t res;
        switch (x4) {
        case 0:  res = a + b + (x2b == 1 ? 1 : 0); break;            /* add */
        case 1:  res = a - b - (x2b == 0 ? 1 : 0); break;            /* sub */
        case 2:  res = (uint32_t)(a + b) | ((b >> 61) << 61); break; /* addp4 */
        case 3:
            switch (x2b) {
            case 0: res = a & b; break;
            case 1: res = ~a & b; break;      /* andcm */
            case 2: res = a | b; break;
            default: res = a ^ b; break;
            }
            break;
        case 4:  res = (a << (x2b + 1)) + b; break;                  /* shladd */
        case 6:  res = (uint32_t)((a << (x2b + 1)) + b) | ((b >> 61) << 61); break;
        case 9:                                                       /* sub imm8 */
            if (x2b != 1) { *st = mhalt(m, "A3 x4=9 x2b=%u", x2b); return 0; }
            res = (uint64_t)sext((bits(raw, 36, 1) << 7) | bits(raw, 13, 7), 8) - b;
            nat = n3;
            break;
        case 0xB: {                                                   /* logical imm8 */
            uint64_t imm = (uint64_t)sext((bits(raw, 36, 1) << 7) | bits(raw, 13, 7), 8);
            nat = n3;
            switch (x2b) {
            case 0: res = imm & b; break;
            case 1: res = ~imm & b; break;
            case 2: res = imm | b; break;
            default: res = imm ^ b; break;
            }
            break;
        }
        default:
            *st = mhalt(m, "unimpl A-unit major 8 x4=0x%X x2b=%u", x4, x2b);
            return 0;
        }
        gr_write(m, r1, res, nat);
        return 1;
    }

    if (major == 9) {                          /* A5 addl imm22 */
        if (!qp) return 1;
        unsigned r3s = (unsigned)bits(raw, 20, 2);
        int64_t imm = sext((bits(raw, 36, 1) << 21) | (bits(raw, 22, 5) << 16) |
                           (bits(raw, 27, 9) << 7) | bits(raw, 13, 7), 22);
        uint64_t b = gr_read(m, r3s, &n3);
        gr_write(m, r1, (uint64_t)imm + b, n3);
        return 1;
    }

    /* majors C/D/E: compares (A6/A7/A8) */
    if (major >= 0xC && major <= 0xE) {
        unsigned tb = (unsigned)bits(raw, 36, 1);
        unsigned x2 = (unsigned)bits(raw, 34, 2);
        unsigned ta = (unsigned)bits(raw, 33, 1);
        unsigned p2 = (unsigned)bits(raw, 27, 6);
        unsigned c  = (unsigned)bits(raw, 12, 1);
        unsigned p1 = (unsigned)bits(raw, 6, 6);
        int cmp4 = (x2 == 1 || x2 == 3);
        int imm_form = (x2 >= 2);
        int res, ctype = 0;
        int64_t a;
        uint64_t au;
        uint64_t b = gr_read(m, r3, &n3);
        uint8_t nat = n3;

        if (imm_form) {                        /* A8 */
            a = sext((bits(raw, 36, 1) << 7) | bits(raw, 13, 7), 8);
            au = (uint64_t)a;
        } else if (tb) {                       /* A7: r2 must be r0 */
            a = 0; au = 0;
        } else {                               /* A6 */
            au = gr_read(m, r2, &n2);
            a = (int64_t)au;
            nat |= n2;
        }
        (void)nat;   /* NaT on compare → both preds 0; approximate: ignore */

        int64_t sb = (int64_t)b;
        uint64_t ub = b;
        if (cmp4) {
            a  = (int32_t)a;  sb = (int32_t)sb;
            au = (uint32_t)au; ub = (uint32_t)ub;
        }

        if (!imm_form && tb) {
            /* A7 matrix: rel from (major, ta, c); all parallel types */
            int rel;
            if (!ta && !c)      rel = (0 > sb);          /* gt  */
            else if (!ta && c)  rel = (0 <= sb);         /* le  */
            else if (ta && !c)  rel = (0 >= sb);         /* ge  */
            else                rel = (0 < sb);          /* lt  */
            ctype = (major == 0xC) ? 2 : (major == 0xD) ? 3 : 4;
            set_preds(m, p1, p2, qp, rel, ctype);
            return 1;
        }

        if (ta) {
            /* eq.and / eq.or / eq.or.andcm (c=1 → ne) */
            res = (au == ub) == (c ? 0 : 1);
            /* careful: c selects ne */
            res = c ? (au != ub) : (au == ub);
            ctype = (major == 0xC) ? 2 : (major == 0xD) ? 3 : 4;
            set_preds(m, p1, p2, qp, res, ctype);
            return 1;
        }

        switch (major) {
        case 0xC: res = a < sb; break;                   /* lt (signed) */
        case 0xD: res = au < ub; break;                  /* ltu */
        default:  res = au == ub; break;                 /* eq */
        }
        set_preds(m, p1, p2, qp, res, c ? 1 : 0);
        return 1;
    }

    return 0;   /* not an A-unit op */
}

/* ── M-unit loads/stores ─────────────────────────────────────────────────── */

static MercedStatus exec_mem(Merced *m, uint64_t raw, int qp) {
    unsigned major = (unsigned)bits(raw, 37, 4);
    unsigned x6 = (unsigned)bits(raw, 30, 6);
    unsigned mbit = (unsigned)bits(raw, 36, 1);
    unsigned xbit = (unsigned)bits(raw, 27, 1);
    unsigned r1 = (unsigned)bits(raw, 6, 7);
    unsigned r2 = (unsigned)bits(raw, 13, 7);
    unsigned r3 = (unsigned)bits(raw, 20, 7);
    uint8_t n2, n3;
    MercedStatus st = MERCED_OK;

    if (major == 4 && xbit && !mbit) {
        /* semaphores: cmpxchg (00-07), xchg (08-0B), fetchadd (12/13/16/17);
         * getf (1C-1F) shares this x=1 encoding space */
        if (!qp) return MERCED_OK;
        if (x6 >= 0x1C && x6 <= 0x1F)
            goto getf;
        warn_once(m, WARN_SEMAPHORE, "semaphore ops executed non-atomically");
        uint64_t va = gr_read(m, r3, &n3), pa;
        unsigned size = 1u << (x6 & 3);
        if (!va_translate(m, va, false, false, &pa, &st)) return st;
        if (x6 <= 0x07) {                       /* cmpxchg.acq/.rel */
            uint64_t old = phys_read(m, pa, size);
            uint64_t ccv = m->ar[AR_CCV];
            if (size < 8) ccv &= (1ull << (size * 8)) - 1;
            if (old == ccv)
                phys_write(m, pa, gr_read(m, r2, &n2), size);
            gr_write(m, r1, old, 0);
            return MERCED_OK;
        }
        if (x6 >= 0x08 && x6 <= 0x0B) {         /* xchg */
            uint64_t old = phys_read(m, pa, size);
            phys_write(m, pa, gr_read(m, r2, &n2), size);
            gr_write(m, r1, old, 0);
            return MERCED_OK;
        }
        if (x6 == 0x12 || x6 == 0x13 || x6 == 0x16 || x6 == 0x17) {
            /* fetchadd4/8.acq/.rel, inc3 immediate */
            unsigned i2b = (unsigned)bits(raw, 13, 2);
            int s = (int)bits(raw, 15, 1);
            int64_t inc = (i2b == 3) ? 1 : (1 << (4 - i2b));
            if (s) inc = -inc;
            uint64_t old = phys_read(m, pa, size);
            phys_write(m, pa, old + (uint64_t)inc, size);
            gr_write(m, r1, old, 0);
            return MERCED_OK;
        }
        return mhalt(m, "unimpl M semaphore x6=0x%02X", x6);
    }

    if (major == 4 && xbit && mbit) {           /* (reserved) */
    getf:
        if (!qp) return MERCED_OK;
        MercedFpReg f = fr_read(m, (unsigned)bits(raw, 13, 7));
        uint64_t v;
        switch (x6) {
        case 0x1C: v = f.sig; break;                              /* getf.sig */
        case 0x1D: v = ((uint64_t)f.sign << 17) | f.exp; break;   /* getf.exp */
        case 0x1E: {                                              /* getf.s */
            double d = fp2d(f); float fl = (float)d; uint32_t u;
            memcpy(&u, &fl, 4); v = u; break;
        }
        case 0x1F: {                                              /* getf.d */
            double d = fp2d(f); uint64_t u; memcpy(&u, &d, 8); v = u; break;
        }
        default: return mhalt(m, "unimpl getf x6=0x%02X", x6);
        }
        gr_write(m, r1, v, f.nat);
        return MERCED_OK;
    }

    int is_imm_form = (major == 5);
    int64_t imm9 = 0;
    if (is_imm_form) {
        int st_form = (x6 >= 0x30);
        uint64_t lo7 = st_form ? bits(raw, 6, 7) : bits(raw, 13, 7);
        imm9 = sext((bits(raw, 36, 1) << 8) | (bits(raw, 27, 1) << 7) | lo7, 9);
    }

    if ((major == 4 && !xbit) || major == 5) {
        unsigned type = x6 >> 2;   /* 0=ld 1=.s 2=.a 3=.sa 4=bias 5=acq 6=fill.. */
        unsigned size = 1u << (x6 & 3);

        if (x6 <= 0x17 || x6 == 0x1B || (x6 >= 0x20 && x6 <= 0x2B)) {  /* loads */
            if (!qp) return MERCED_OK;
            uint64_t va = gr_read(m, r3, &n3), pa;
            int spec = (type == 1 || type == 3);   /* .s / .sa */
            if (x6 == 0x1B) size = 8;              /* ld8.fill */
            if (!va_translate(m, va, false, spec, &pa, &st)) {
                if (st != MERCED_OK) return st;
                gr_write(m, r1, 0, 1);             /* NaT on deferred spec load */
            } else {
                uint64_t v = phys_read(m, pa, size);
                uint8_t nat = 0;
                if (x6 == 0x1B)                    /* ld8.fill: NaT from UNAT */
                    nat = (uint8_t)((m->ar[AR_UNAT] >> ((va >> 3) & 0x3F)) & 1);
                gr_write(m, r1, v, nat);
            }
            /* base update */
            if (major == 5) {
                uint64_t base = gr_read(m, r3, &n3);
                gr_write(m, r3, base + (uint64_t)imm9, n3);
            } else if (mbit) {
                uint64_t base = gr_read(m, r3, &n3);
                uint64_t inc = gr_read(m, r2, &n2);
                gr_write(m, r3, base + inc, n3 | n2);
            }
            return MERCED_OK;
        }

        if ((x6 >= 0x30 && x6 <= 0x37) || x6 == 0x3B) {   /* stores */
            if (!qp) return MERCED_OK;
            uint64_t va = gr_read(m, r3, &n3), pa;
            if (x6 == 0x3B) size = 8;              /* st8.spill */
            if (!va_translate(m, va, false, false, &pa, &st)) return st;
            uint64_t v = gr_read(m, r2, &n2);
            if (x6 == 0x3B) {
                unsigned bit = (unsigned)((va >> 3) & 0x3F);
                if (n2) m->ar[AR_UNAT] |= 1ull << bit;
                else    m->ar[AR_UNAT] &= ~(1ull << bit);
            }
            phys_write(m, pa, v, size);
            if (major == 5)
                gr_write(m, r3, va + (uint64_t)imm9, n3);
            return MERCED_OK;
        }
        return mhalt(m, "unimpl M ld/st x6=0x%02X (major %u)", x6, major);
    }

    if (major == 6 || major == 7) {
        /* FP load/store + lfetch + setf */
        if (major == 6 && xbit && mbit == 0) {     /* M18 setf */
            if (!qp) return MERCED_OK;
            uint64_t v = gr_read(m, r2, &n2);
            MercedFpReg f = {0, 0, 0, 0};
            switch (x6) {
            case 0x1C: f.sig = v; f.exp = 0x1003E; break;         /* setf.sig */
            case 0x1D: f.exp = (uint32_t)(v & 0x1FFFF);           /* setf.exp */
                       f.sign = (uint8_t)((v >> 17) & 1);
                       f.sig = 0x8000000000000000ull; break;
            case 0x1E: { float fl; uint32_t u = (uint32_t)v;      /* setf.s */
                         memcpy(&fl, &u, 4); f = d2fp((double)fl); break; }
            case 0x1F: { double d; uint64_t u = v;                /* setf.d */
                         memcpy(&d, &u, 8); f = d2fp(d); break; }
            default: return mhalt(m, "unimpl setf x6=0x%02X", x6);
            }
            f.nat = n2;
            fr_write(m, (unsigned)bits(raw, 6, 7), f);
            return MERCED_OK;
        }
        if (x6 >= 0x2C && x6 <= 0x2F) {            /* lfetch: treat as nop */
            if (qp && major == 7) {                /* imm base update form */
                uint64_t v = gr_read(m, r3, &n3);
                int64_t i9 = sext((bits(raw, 36, 1) << 8) |
                                  (bits(raw, 27, 1) << 7) | bits(raw, 13, 7), 9);
                gr_write(m, r3, v + (uint64_t)i9, n3);
            } else if (qp && mbit) {
                uint64_t base = gr_read(m, r3, &n3);
                uint64_t inc = gr_read(m, r2, &n2);
                gr_write(m, r3, base + inc, n3 | n2);
            }
            return MERCED_OK;
        }
        /* FP loads: x6 0x00-0x0F ldfe/ldf8/ldfs/ldfd(+spec), 0x1B ldf.fill;
         * stores: 0x30-0x33 stfe/stf8/stfs/stfd, 0x3B stf.spill */
        int is_st = (x6 >= 0x30);
        if (!qp) return MERCED_OK;
        uint64_t va = gr_read(m, r3, &n3), pa;
        if (!va_translate(m, va, false, false, &pa, &st)) return st;
        unsigned fmt = x6 & 3;   /* 0=e 1=8 2=s 3=d */
        if (!is_st && (x6 <= 0x0F || x6 == 0x1B)) {
            MercedFpReg f = {0, 0, 0, 0};
            if (x6 == 0x1B) {                       /* ldf.fill: spill format */
                f.sig = phys_read(m, pa, 8);
                uint64_t se = phys_read(m, pa + 8, 8);
                f.exp = (uint32_t)(se & 0x1FFFF);
                f.sign = (uint8_t)((se >> 17) & 1);
            } else switch (fmt) {
            case 0:                                  /* ldfe: 82-bit in 16 bytes */
                f.sig = phys_read(m, pa, 8);
                { uint64_t se = phys_read(m, pa + 8, 8);
                  f.exp = (uint32_t)(se & 0x1FFFF);
                  f.sign = (uint8_t)((se >> 17) & 1); }
                break;
            case 1: f.sig = phys_read(m, pa, 8); f.exp = 0x1003E; break; /* ldf8 */
            case 2: { uint32_t u = (uint32_t)phys_read(m, pa, 4); float fl;
                      memcpy(&fl, &u, 4); f = d2fp((double)fl); break; }
            case 3: { uint64_t u = phys_read(m, pa, 8); double d;
                      memcpy(&d, &u, 8); f = d2fp(d); break; }
            }
            fr_write(m, (unsigned)bits(raw, 6, 7), f);
        } else if (is_st) {
            MercedFpReg f = fr_read(m, (unsigned)bits(raw, 13, 7));
            if (x6 == 0x3B) {                        /* stf.spill */
                phys_write(m, pa, f.sig, 8);
                phys_write(m, pa + 8, ((uint64_t)f.sign << 17) | f.exp, 8);
            } else switch (fmt) {
            case 0:
                phys_write(m, pa, f.sig, 8);
                phys_write(m, pa + 8, ((uint64_t)f.sign << 17) | f.exp, 8);
                break;
            case 1: phys_write(m, pa, f.sig, 8); break;
            case 2: { float fl = (float)fp2d(f); uint32_t u;
                      memcpy(&u, &fl, 4); phys_write(m, pa, u, 4); break; }
            case 3: { double d = fp2d(f); uint64_t u;
                      memcpy(&u, &d, 8); phys_write(m, pa, u, 8); break; }
            }
        } else {
            return mhalt(m, "unimpl M fp-mem x6=0x%02X major %u", x6, major);
        }
        /* base updates */
        if (major == 7) {
            int stf = is_st;
            uint64_t lo7 = stf ? bits(raw, 6, 7) : bits(raw, 13, 7);
            int64_t i9 = sext((bits(raw, 36, 1) << 8) |
                              (bits(raw, 27, 1) << 7) | lo7, 9);
            gr_write(m, r3, va + (uint64_t)i9, n3);
        } else if (mbit) {
            uint64_t inc = gr_read(m, r2, &n2);
            gr_write(m, r3, va + inc, n3 | n2);
        }
        return MERCED_OK;
    }

    return mhalt(m, "unimpl M memory major %u x6=0x%02X", major, x6);
}

/* ── M-unit system ops ───────────────────────────────────────────────────── */

static unsigned tlb_debug_events;
#define TLB_DEBUG_MAX 48

/* debug: log transitions of PSR.it / PSR.dt with their source */
static void psr_trans_log(Merced *m, uint64_t newpsr, const char *src) {
    static unsigned n;
    uint64_t chg = (m->psr ^ newpsr) & (PSR_IT | PSR_DT | PSR_RT);
    if (merced_dbg() && chg && n < 32) {
        n++;
        fprintf(stderr, "merced: PSR it=%u->%u dt=%u->%u rt=%u->%u via %s"
                " at ip=%016" PRIX64 "\n",
                !!(m->psr & PSR_IT), !!(newpsr & PSR_IT),
                !!(m->psr & PSR_DT), !!(newpsr & PSR_DT),
                !!(m->psr & PSR_RT), !!(newpsr & PSR_RT),
                src, m->ip);
    }
}

static void tlb_insert(Merced *m, MercedTlbEntry *e, uint64_t pte,
                       bool instruction) {
    uint64_t ifa = m->cr[CR_IFA];
    uint64_t itir = m->cr[CR_ITIR];
    unsigned ps = (unsigned)((itir >> 2) & 0x3F);
    unsigned vrn = (unsigned)(ifa >> 61);
    uint64_t page = (ps >= 64) ? 0 : ~((1ull << ps) - 1);
    e->valid = (uint8_t)(pte & 1);
    e->rid = (uint32_t)((m->rr[vrn] >> 8) & 0xFFFFFF);
    e->va_start = ifa & page;
    e->va_end = e->va_start + (ps >= 64 ? ~0ull : (1ull << ps) - 1);
    e->pfn_base = (pte & 0x0003FFFFFFFFF000ull) & page;
    e->ps = (uint8_t)ps;
    e->itir = itir;
    e->pte = pte;
    if (merced_dbg() && tlb_debug_events < TLB_DEBUG_MAX) {
        tlb_debug_events++;
        fprintf(stderr, "merced: insert %c ip=%016" PRIX64
                " va=%016" PRIX64 " pa=%016" PRIX64 " ps=%u rid=%06X"
                " pte=%016" PRIX64 " valid=%u\n",
                instruction ? 'I' : 'D', m->ip, e->va_start, e->pfn_base,
                ps, e->rid, pte, e->valid);
    }
}

static void tlb_purge(MercedTlbEntry *t, int n, uint32_t rid,
                      uint64_t va, uint64_t len) {
    for (int i = 0; i < n; i++) {
        if (t[i].valid && t[i].rid == rid &&
            t[i].va_start < va + len && va <= t[i].va_end) {
            t[i].valid = 0;
            if (merced_dbg() && tlb_debug_events < TLB_DEBUG_MAX) {
                tlb_debug_events++;
                fprintf(stderr, "merced: purge va=%016" PRIX64
                        " len=%016" PRIX64 " rid=%06X hit va=%016" PRIX64
                        "-%016" PRIX64 "\n",
                        va, len, rid, t[i].va_start, t[i].va_end);
            }
        }
    }
}

/* ── RSE backing store ───────────────────────────────────────────────────── */

/* A NaT-collection slot is interleaved after every 63 general registers in
 * the real backing store. These convert between a register count and its
 * byte span on that layout, assuming the zero point (rse_anchor_regs) is
 * always a fresh NaT-group boundary - true here because it's only ever
 * (re)established by an explicit `mov ar.bspstore=r`, which is also the only
 * way this model ties a register count to a real address in the first
 * place. */
static uint64_t bytes_for_regs(int64_t n) {
    if (n <= 0) return 0;
    return (uint64_t)(n + n / 63) * 8;
}

static int64_t regs_for_bytes(uint64_t bytes) {
    uint64_t groups = bytes / 512;   /* 64 slots/group (63 GR + 1 NaT) * 8 */
    uint64_t rem = bytes % 512;
    return (int64_t)(groups * 63 + rem / 8);
}

static uint64_t rse_addr(Merced *m, int64_t regs_pos) {
    return m->rse_anchor_addr + bytes_for_regs(regs_pos - m->rse_anchor_regs);
}

/* Maps an absolute register-units position (same axis as bof_total) to its
 * gr_stack slot. Valid as long as it's within MERCED_N_STACKED of the
 * current frame - i.e. as long as real hardware wouldn't itself have needed
 * to background-spill past the 96-register physical file (see the
 * merced.h simplifications note). */
static uint32_t rse_stack_slot(Merced *m, int64_t regs_pos) {
    int64_t rel = regs_pos - (int64_t)m->bof_total;
    int64_t idx = ((int64_t)m->bof + rel) % MERCED_N_STACKED;
    if (idx < 0) idx += MERCED_N_STACKED;
    return (uint32_t)idx;
}

/* flushrs: write every register from the last flushed position up to the
 * current ar.bsp (bof_total+sof) out to the backing store, then advance
 * ar.bspstore (rse_flushed_regs) to match. */
static MercedStatus rse_flush(Merced *m) {
    int64_t target = (int64_t)m->bof_total + (int64_t)CFM_SOF(m->cfm);
    for (int64_t p = m->rse_flushed_regs; p < target; p++) {
        uint32_t idx = rse_stack_slot(m, p);
        uint64_t pa;
        MercedStatus st;
        if (!va_translate(m, rse_addr(m, p), false, false, &pa, &st))
            return st;
        phys_write(m, pa, m->gr_stack[idx], 8);
    }
    m->rse_flushed_regs = target;
    return MERCED_OK;
}

/* loadrs: per ar.rsc.loadrs (a byte count), position ar.bspstore that many
 * bytes behind the current ar.bsp and re-read whatever falls in the newly
 * "unflushed" window back from the backing store - the values are already
 * resident in gr_stack (this model never evicts them), but re-reading keeps
 * behavior correct if something patched the backing store directly (e.g. an
 * OS context-switch path), which is exactly the scenario loadrs exists for.
 * If the target instead lands past the current flush point (asking to
 * advance ar.bspstore without anything having been flushed there), there's
 * nothing meaningful to load from memory, so just adopt it - matching this
 * model's lack of a real dirty/clean partition to fault on. */
static MercedStatus rse_load(Merced *m, uint64_t loadrs_bytes) {
    int64_t bsp_regs = (int64_t)m->bof_total + (int64_t)CFM_SOF(m->cfm);
    int64_t target = bsp_regs - regs_for_bytes(loadrs_bytes);
    if (target >= m->rse_flushed_regs) {
        m->rse_flushed_regs = target;
        return MERCED_OK;
    }
    for (int64_t p = target; p < m->rse_flushed_regs; p++) {
        uint32_t idx = rse_stack_slot(m, p);
        uint64_t pa;
        MercedStatus st;
        if (!va_translate(m, rse_addr(m, p), false, false, &pa, &st))
            return st;
        m->gr_stack[idx] = phys_read(m, pa, 8);
        m->nat_stack[idx] = 0;   /* NaT collection words not modeled */
    }
    m->rse_flushed_regs = target;
    return MERCED_OK;
}

static MercedStatus exec_m_sys(Merced *m, uint64_t raw, int qp) {
    unsigned major = (unsigned)bits(raw, 37, 4);
    unsigned x3 = (unsigned)bits(raw, 33, 3);
    unsigned r1 = (unsigned)bits(raw, 6, 7);
    unsigned r2 = (unsigned)bits(raw, 13, 7);
    unsigned r3 = (unsigned)bits(raw, 20, 7);
    uint8_t n2, n3;

    if (major == 0) {
        unsigned x2 = (unsigned)bits(raw, 31, 2);
        unsigned x4 = (unsigned)bits(raw, 27, 4);
        if (x3 == 0) {
            if (x4 >= 4 && x4 <= 7) {              /* M44 sum/rum/ssm/rsm */
                if (!qp) return MERCED_OK;
                uint64_t imm = (bits(raw, 36, 1) << 23) | (bits(raw, 31, 2) << 21) |
                               bits(raw, 6, 21);
                switch (x4) {
                case 4: m->psr |= (imm & 0x3F); break;          /* sum */
                case 5: m->psr &= ~(imm & 0x3F); break;         /* rum */
                case 6: psr_trans_log(m, m->psr | imm, "ssm");
                        m->psr |= imm; break;                   /* ssm */
                default: psr_trans_log(m, m->psr & ~imm, "rsm");
                         m->psr &= ~imm; break;                 /* rsm */
                }
                return MERCED_OK;
            }
            switch ((x2 << 4) | x4) {
            case 0x00: {                            /* break.m */
                uint64_t imm = (bits(raw, 36, 1) << 20) | bits(raw, 6, 20);
                if (!qp) return MERCED_OK;
                m->cr[CR_IIM] = imm;
                snprintf(m->halt_msg, sizeof(m->halt_msg),
                         "break.m 0x%" PRIX64, imm);
                m->halt_ip = m->ip;
                return MERCED_HALT_BREAK;
            }
            case 0x01: return MERCED_OK;            /* nop.m / hint.m */
            case 0x10: return MERCED_OK;            /* invala */
            case 0x12: case 0x13: return MERCED_OK; /* invala.e */
            case 0x20: return MERCED_OK;            /* fwb */
            case 0x22: case 0x23: return MERCED_OK; /* mf / mf.a */
            case 0x28: {                            /* M30 mov.m ar3=imm8 */
                if (!qp) return MERCED_OK;
                unsigned ar3 = (unsigned)bits(raw, 20, 7);
                m->ar[ar3] = (uint64_t)sext((bits(raw, 36, 1) << 7) |
                                            bits(raw, 13, 7), 8);
                return MERCED_OK;
            }
            case 0x30: case 0x31: case 0x33: return MERCED_OK; /* srlz.d/i, sync.i */
            case 0x0C: return rse_flush(m);         /* flushrs */
            case 0x0A:                              /* loadrs */
                return rse_load(m, (m->ar[AR_RSC] >> 16) & 0x3FFF);
            }
            return mhalt(m, "unimpl M-sys major 0 x2=%u x4=0x%X", x2, x4);
        }
        if (x3 >= 4) return MERCED_OK;              /* M22/M23 chk.a: no ALAT → never fail */
        if (x3 == 1 || x3 == 2 || x3 == 3) {
            /* M20 chk.s.m r2,target25 (x3=1); 2/3 are fp/spec variants */
            if (!qp) return MERCED_OK;
            gr_read(m, r2, &n2);
            if (x3 == 1 && n2) {
                int64_t disp = sext((bits(raw, 36, 1) << 20) |
                                    (bits(raw, 20, 13) << 7) | bits(raw, 6, 7), 21) << 4;
                m->ip = (m->ip & ~0xFull) + (uint64_t)disp;
                m->taken = 1;
            }
            return MERCED_OK;
        }
        return mhalt(m, "unimpl M-sys major 0 x3=%u", x3);
    }

    /* major 1 */
    if (x3 == 6) {                                  /* alloc */
        unsigned sof = (unsigned)bits(raw, 13, 7);
        unsigned sol = (unsigned)bits(raw, 20, 7);
        unsigned sor = (unsigned)bits(raw, 27, 4);
        if (sof > MERCED_N_STACKED)
            return mhalt(m, "alloc sof=%u too large", sof);
        gr_write(m, r1, m->ar[AR_PFS], 0);
        /* alloc replaces sof/sol/sor but preserves the three rotation
         * bases.  Firmware routinely enters leaf arithmetic helpers with
         * a non-zero rrb.pr and relies on alloc leaving it intact. */
        m->cfm = (m->cfm & ~0x3FFFFull) |
                 sof | ((uint64_t)sol << 7) | ((uint64_t)sor << 14);
        return MERCED_OK;
    }
    if (x3 != 0)
        return mhalt(m, "unimpl M-sys major 1 x3=%u", x3);

    unsigned x6 = (unsigned)bits(raw, 27, 6);
    if (!qp) return MERCED_OK;
    switch (x6) {
    case 0x00: m->rr[gr_read(m, r3, &n3) >> 61 & 7] = gr_read(m, r2, &n2);
               /* rr index is GR[r3] bits 63:61 */
               return MERCED_OK;
    case 0x01: m->dbr[gr_read(m, r3, &n3) & 15] = gr_read(m, r2, &n2); return MERCED_OK;
    case 0x02: m->ibr[gr_read(m, r3, &n3) & 15] = gr_read(m, r2, &n2); return MERCED_OK;
    case 0x03: m->pkr[gr_read(m, r3, &n3) & 15] = gr_read(m, r2, &n2); return MERCED_OK;
    case 0x04: m->pmc[gr_read(m, r3, &n3) & 31] = gr_read(m, r2, &n2); return MERCED_OK;
    case 0x05: m->pmd[gr_read(m, r3, &n3) & 31] = gr_read(m, r2, &n2); return MERCED_OK;
    case 0x06: {
        unsigned idx = (unsigned)(gr_read(m, r3, &n3) & 4095);
        m->msr[idx] = gr_read(m, r2, &n2);
        m->msr_written[idx] = 1;
        m->msr_polls[idx] = 0;
        m->msr_toggle[idx] = 0;
        return MERCED_OK;
    }
    case 0x09: case 0x0A: case 0x0B: {              /* ptc.l/g/ga */
        uint64_t va = gr_read(m, r3, &n3);
        uint64_t ps = (gr_read(m, r2, &n2) >> 2) & 0x3F;
        uint32_t rid = (uint32_t)((m->rr[va >> 61] >> 8) & 0xFFFFFF);
        uint64_t len = ps >= 64 ? ~0ull : 1ull << ps;
        tlb_purge(m->itc, MERCED_N_TC, rid, va, len);
        tlb_purge(m->dtc, MERCED_N_TC, rid, va, len);
        return MERCED_OK;
    }
    case 0x0C: case 0x0D: {                         /* ptr.d / ptr.i */
        uint64_t va = gr_read(m, r3, &n3);
        uint64_t ps = (gr_read(m, r2, &n2) >> 2) & 0x3F;
        uint32_t rid = (uint32_t)((m->rr[va >> 61] >> 8) & 0xFFFFFF);
        uint64_t len = ps >= 64 ? ~0ull : 1ull << ps;
        if (x6 == 0x0C) {
            tlb_purge(m->dtr, MERCED_N_TR, rid, va, len);
            tlb_purge(m->dtc, MERCED_N_TC, rid, va, len);
        } else {
            tlb_purge(m->itr, MERCED_N_TR, rid, va, len);
            tlb_purge(m->itc, MERCED_N_TC, rid, va, len);
        }
        return MERCED_OK;
    }
    case 0x0E:                                      /* itr.d dtr[r3]=r2 */
        tlb_insert(m, &m->dtr[gr_read(m, r3, &n3) & (MERCED_N_TR - 1)],
                   gr_read(m, r2, &n2), false);
        return MERCED_OK;
    case 0x0F:                                      /* itr.i itr[r3]=r2 */
        tlb_insert(m, &m->itr[gr_read(m, r3, &n3) & (MERCED_N_TR - 1)],
                   gr_read(m, r2, &n2), true);
        return MERCED_OK;
    case 0x10: gr_write(m, r1, m->rr[gr_read(m, r3, &n3) >> 61 & 7], 0); return MERCED_OK;
    case 0x11: gr_write(m, r1, m->dbr[gr_read(m, r3, &n3) & 15], 0); return MERCED_OK;
    case 0x12: gr_write(m, r1, m->ibr[gr_read(m, r3, &n3) & 15], 0); return MERCED_OK;
    case 0x13: gr_write(m, r1, m->pkr[gr_read(m, r3, &n3) & 15], 0); return MERCED_OK;
    case 0x14: gr_write(m, r1, m->pmc[gr_read(m, r3, &n3) & 31], 0); return MERCED_OK;
    case 0x15: gr_write(m, r1, m->pmd[gr_read(m, r3, &n3) & 31], 0); return MERCED_OK;
    case 0x16: {
        unsigned idx = (unsigned)(gr_read(m, r3, &n3) & 4095);
        uint64_t v;
        if (m->msr_written[idx]) {
            v = m->msr[idx];
            /* command/status handshake: PAL writes a command, then polls
             * the same register for completion. After many reads with no
             * intervening write, alternate the complement in so a bit-wait
             * of either polarity terminates. */
            if (m->msr_polls[idx] < 0xFFFF)
                m->msr_polls[idx]++;
            if (m->msr_polls[idx] > 64) {
                m->msr_toggle[idx] ^= 1;
                if (m->msr_toggle[idx]) v = ~v;
            }
        } else {
            /* Unwritten status MSR: quiet (0) for one-shot reads - the
             * SALE-entry wake-reason probes must see no pending events -
             * but alternate once something polls it repeatedly so
             * wait-for-set handshakes still terminate. */
            if (m->msr_polls[idx] < 0xFFFF)
                m->msr_polls[idx]++;
            if (m->msr_polls[idx] > 16)
                m->msr_toggle[idx] ^= 1;
            v = m->msr_toggle[idx] ? ~0ull : 0;
        }
        gr_write(m, r1, v, 0);
        return MERCED_OK;
    }
    case 0x17: gr_write(m, r1, m->cpuid[gr_read(m, r3, &n3) & 7], 0); return MERCED_OK;
    case 0x18: case 0x19:                           /* probe imm2 */
        warn_once(m, WARN_PROBE, "probe always succeeds");
        gr_write(m, r1, 1, 0);
        return MERCED_OK;
    case 0x1A: {                                    /* thash */
        uint64_t va = gr_read(m, r3, &n3);
        unsigned vrn = (unsigned)(va >> 61);
        uint64_t pta = m->cr[CR_PTA];
        unsigned ps = (unsigned)((m->rr[vrn] >> 2) & 0x3F);
        unsigned sz = (unsigned)((pta >> 2) & 0x3F);
        uint64_t mask = (sz >= 64) ? ~0ull : ((1ull << sz) - 1);
        uint64_t off = ((va & 0x1FFFFFFFFFFFFFFFull) >> ps) << 3;
        gr_write(m, r1, ((uint64_t)vrn << 61) |
                        ((pta & ~0x7FFull & ~mask) | (off & mask & ~0x7ull)), 0);
        return MERCED_OK;
    }
    case 0x1B: {                                    /* ttag */
        uint64_t va = gr_read(m, r3, &n3);
        unsigned vrn = (unsigned)(va >> 61);
        uint64_t rid = (m->rr[vrn] >> 8) & 0xFFFFFF;
        unsigned ps = (unsigned)((m->rr[vrn] >> 2) & 0x3F);
        gr_write(m, r1, ((va >> ps) ^ (rid << 39)) & 0x7FFFFFFFFFFFFFFFull, 0);
        return MERCED_OK;
    }
    case 0x1E: {                                    /* tpa */
        uint64_t va = gr_read(m, r3, &n3), pa;
        MercedStatus st;
        if (!va_translate(m, va, false, false, &pa, &st)) return st;
        gr_write(m, r1, pa, 0);
        return MERCED_OK;
    }
    case 0x1F: gr_write(m, r1, 0, 0); return MERCED_OK;   /* tak: key 0 */
    case 0x21: gr_write(m, r1, m->psr & 0x3F, 0); return MERCED_OK;    /* psr.um */
    case 0x22: {                                    /* mov.m r1=ar3 */
        unsigned ar3 = (unsigned)bits(raw, 20, 7);
        uint64_t v = m->ar[ar3];
        if (ar3 == AR_BSP)
            v = rse_addr(m, (int64_t)m->bof_total + (int64_t)CFM_SOF(m->cfm));
        else if (ar3 == AR_BSPSTORE)
            v = rse_addr(m, m->rse_flushed_regs);
        gr_write(m, r1, v, 0);
        return MERCED_OK;
    }
    case 0x24: {                                    /* mov r1=cr3 */
        unsigned cr3 = (unsigned)bits(raw, 20, 7);
        uint64_t v = m->cr[cr3];
        if (cr3 == CR_IVR) {
            /* Reading ivr reports and acknowledges the pending vector. */
            if (m->external_pending) {
                v = m->external_vector;
                m->external_pending = 0;
            } else if (m->timer_pending) {
                v = m->cr[CR_ITV] & 0xFFull;
                m->timer_pending = 0;
            } else {
                v = 15;
            }
        }
        gr_write(m, r1, v, 0);
        return MERCED_OK;
    }
    case 0x25: gr_write(m, r1, m->psr, 0); return MERCED_OK;   /* mov r1=psr */
    case 0x29: m->psr = (m->psr & ~0x3Full) | (gr_read(m, r2, &n2) & 0x3F);
               return MERCED_OK;                    /* mov psr.um=r2 */
    case 0x2A: {                                    /* mov.m ar3=r2 */
        unsigned ar3 = (unsigned)bits(raw, 20, 7);
        m->ar[ar3] = gr_read(m, r2, &n2);
        if (ar3 == AR_BSPSTORE) {
            /* Establishes a fresh address<->register-count correspondence:
             * right after this write, ar.bsp == ar.bspstore == the written
             * value (zero dirty registers), matching architectural rules
             * for writing ar.bspstore. */
            m->rse_anchor_addr = m->ar[AR_BSPSTORE];
            m->rse_anchor_regs = (int64_t)m->bof_total + (int64_t)CFM_SOF(m->cfm);
            m->rse_flushed_regs = m->rse_anchor_regs;
            m->ar[AR_BSP] = m->ar[AR_BSPSTORE];
        }
        return MERCED_OK;
    }
    case 0x2C: case 0x3C: {                         /* mov cr3=r2 */
        unsigned cr3 = (unsigned)bits(raw, 20, 7);
        m->cr[cr3] = gr_read(m, r2, &n2);
        if (cr3 == CR_IVA) {
            static unsigned n;
            if (merced_dbg() && n < 16) {
                n++;
                fprintf(stderr, "merced: cr.iva <- %016" PRIX64
                        " at ip=%016" PRIX64 "\n", m->cr[cr3], m->ip);
            }
        }
        return MERCED_OK;
    }
    case 0x2D: {                                    /* mov psr.l=r2 */
        uint64_t np = (m->psr & ~0xFFFFFFFFull) | (uint32_t)gr_read(m, r2, &n2);
        psr_trans_log(m, np, "mov psr.l");
        m->psr = np;
        return MERCED_OK;
    }
    case 0x2E:                                      /* itc.d */
        tlb_insert(m, &m->dtc[m->dtc_next++ % MERCED_N_TC],
                   gr_read(m, r2, &n2), false);
        return MERCED_OK;
    case 0x2F:                                      /* itc.i */
        tlb_insert(m, &m->itc[m->itc_next++ % MERCED_N_TC],
                   gr_read(m, r2, &n2), true);
        return MERCED_OK;
    case 0x30: return MERCED_OK;                    /* fc / fc.i */
    case 0x31: case 0x32: case 0x33: return MERCED_OK;   /* probe.fault */
    case 0x34:                                      /* ptc.e */
        memset(m->itc, 0, sizeof(m->itc));
        memset(m->dtc, 0, sizeof(m->dtc));
        return MERCED_OK;
    case 0x38: case 0x39:                           /* probe.r/w reg */
        warn_once(m, WARN_PROBE, "probe always succeeds");
        gr_write(m, r1, 1, 0);
        return MERCED_OK;
    }
    return mhalt(m, "unimpl M-sys major 1 x6=0x%02X", x6);
}

/* ── I-unit ──────────────────────────────────────────────────────────────── */

static MercedStatus exec_i(Merced *m, uint64_t raw, int qp) {
    unsigned major = (unsigned)bits(raw, 37, 4);
    unsigned r1 = (unsigned)bits(raw, 6, 7);
    unsigned r2 = (unsigned)bits(raw, 13, 7);
    unsigned r3 = (unsigned)bits(raw, 20, 7);
    uint8_t n2, n3;
    MercedStatus st;

    if (exec_alu(m, raw, qp, &st)) return st;
    if (st != MERCED_OK) return st;

    switch (major) {
    case 0: {
        unsigned x3 = (unsigned)bits(raw, 33, 3);
        if (x3 == 0) {
            unsigned x6 = (unsigned)bits(raw, 27, 6);
            switch (x6) {
            case 0x00: {                            /* break.i */
                uint64_t imm = (bits(raw, 36, 1) << 20) | bits(raw, 6, 20);
                if (!qp) return MERCED_OK;
                m->cr[CR_IIM] = imm;
                snprintf(m->halt_msg, sizeof(m->halt_msg),
                         "break.i 0x%" PRIX64, imm);
                m->halt_ip = m->ip;
                return MERCED_HALT_BREAK;
            }
            case 0x01: return MERCED_OK;            /* nop.i / hint.i */
            case 0x0A: {                            /* mov.i ar3=imm8 */
                if (!qp) return MERCED_OK;
                unsigned ar3 = (unsigned)bits(raw, 20, 7);
                m->ar[ar3] = (uint64_t)sext((bits(raw, 36, 1) << 7) |
                                            bits(raw, 13, 7), 8);
                return MERCED_OK;
            }
            case 0x10: case 0x11: case 0x12:
            case 0x14: case 0x15: case 0x16: {       /* zxt/sext */
                if (!qp) return MERCED_OK;
                uint64_t v = gr_read(m, r3, &n3);
                switch (x6) {
                case 0x10: v = (uint8_t)v; break;
                case 0x11: v = (uint16_t)v; break;
                case 0x12: v = (uint32_t)v; break;
                case 0x14: v = (uint64_t)(int64_t)(int8_t)v; break;
                case 0x15: v = (uint64_t)(int64_t)(int16_t)v; break;
                default:   v = (uint64_t)(int64_t)(int32_t)v; break;
                }
                gr_write(m, r1, v, n3);
                return MERCED_OK;
            }
            case 0x18: case 0x19: case 0x1C: case 0x1D: {   /* czx */
                if (!qp) return MERCED_OK;
                uint64_t v = gr_read(m, r3, &n3);
                int two = (x6 & 1);
                int left = (x6 & 4) == 0;   /* .l scans from MSB */
                unsigned n = two ? 4 : 8, res = n;
                for (unsigned k = 0; k < n; k++) {
                    unsigned byte = left ? (n - 1 - k) : k;
                    uint64_t el = two ? (v >> (byte * 16)) & 0xFFFF
                                      : (v >> (byte * 8)) & 0xFF;
                    if (el == 0) { res = k; break; }
                }
                gr_write(m, r1, res, n3);
                return MERCED_OK;
            }
            case 0x30: if (qp) gr_write(m, r1, m->ip & ~0xFull, 0); return MERCED_OK;
            case 0x31: if (qp) gr_write(m, r1, m->br[bits(raw, 13, 3)], 0); return MERCED_OK;
            case 0x32: {                            /* mov.i r1=ar3 */
                if (!qp) return MERCED_OK;
                unsigned ar3 = (unsigned)bits(raw, 20, 7);
                gr_write(m, r1, m->ar[ar3], 0);
                return MERCED_OK;
            }
            case 0x33: {                            /* mov r1=pr */
                if (!qp) return MERCED_OK;
                uint64_t v = 0;
                for (unsigned i = 0; i < 64; i++)
                    if (pr_read(m, i)) v |= 1ull << i;
                gr_write(m, r1, v, 0);
                return MERCED_OK;
            }
            case 0x2A: {                            /* mov.i ar3=r2 */
                if (!qp) return MERCED_OK;
                unsigned ar3 = (unsigned)bits(raw, 20, 7);
                m->ar[ar3] = gr_read(m, r2, &n2);
                return MERCED_OK;
            }
            }
            return mhalt(m, "unimpl I-unit major 0 x6=0x%02X", x6);
        }
        if (x3 == 7) {                              /* mov b1=r2 */
            if (qp) m->br[bits(raw, 6, 3)] = gr_read(m, r2, &n2);
            return MERCED_OK;
        }
        if (x3 == 3) {                              /* mov pr=r2,mask17 */
            if (!qp) return MERCED_OK;
            uint64_t mask = (uint64_t)sext((bits(raw, 36, 1) << 16) |
                                           (bits(raw, 24, 8) << 8) |
                                           (bits(raw, 6, 7) << 1), 17);
            uint64_t v = gr_read(m, r2, &n2);
            for (unsigned i = 1; i < 64; i++)
                if ((mask >> i) & 1) pr_write(m, i, (int)((v >> i) & 1));
            return MERCED_OK;
        }
        if (x3 == 2) {                              /* mov pr.rot=imm44 */
            if (!qp) return MERCED_OK;
            uint64_t imm = (uint64_t)sext((bits(raw, 36, 1) << 43) |
                                          (bits(raw, 6, 27) << 16), 44);
            for (unsigned i = 16; i < 64; i++)
                pr_write(m, i, (int)((imm >> i) & 1));
            return MERCED_OK;
        }
        if (x3 == 1) {                              /* chk.s.i */
            if (!qp) return MERCED_OK;
            gr_read(m, r2, &n2);
            if (n2) {
                int64_t disp = sext((bits(raw, 36, 1) << 20) |
                                    (bits(raw, 20, 13) << 7) | bits(raw, 6, 7), 21) << 4;
                m->ip = (m->ip & ~0xFull) + (uint64_t)disp;
                m->taken = 1;
            }
            return MERCED_OK;
        }
        return mhalt(m, "unimpl I-unit major 0 x3=%u", x3);
    }

    case 4: {                                       /* I15 dep r1=r2,r3,pos6,len4 */
        if (!qp) return MERCED_OK;
        /* I15's complemented position is split around the opcode fields;
         * it is not raw bits 36:31. */
        unsigned cpos = (unsigned)bits(raw, 31, 2) |
                        ((unsigned)bits(raw, 33, 1) << 2) |
                        ((unsigned)bits(raw, 34, 2) << 3) |
                        ((unsigned)bits(raw, 36, 1) << 5);
        unsigned pos = 63 - cpos;
        unsigned len = (unsigned)bits(raw, 27, 4) + 1;
        uint64_t a = gr_read(m, r2, &n2), b = gr_read(m, r3, &n3);
        uint64_t mask = (len >= 64) ? ~0ull : ((1ull << len) - 1);
        if (pos < 64) {
            uint64_t field = (a & mask) << pos;
            uint64_t hole = mask << pos;
            gr_write(m, r1, (b & ~hole) | field, n2 | n3);
        }
        return MERCED_OK;
    }

    case 5: {
        unsigned x2 = (unsigned)bits(raw, 34, 2);
        unsigned x  = (unsigned)bits(raw, 33, 1);
        if (x2 == 0) {                              /* tbit / tnat */
            unsigned tb = (unsigned)bits(raw, 36, 1);
            unsigned ta = x;   /* naming: ta at 33 */
            unsigned p2 = (unsigned)bits(raw, 27, 6);
            unsigned y  = (unsigned)bits(raw, 13, 1);
            unsigned c  = (unsigned)bits(raw, 12, 1);
            unsigned p1 = (unsigned)bits(raw, 6, 6);
            uint64_t v = gr_read(m, r3, &n3);
            int bitval;
            if (y) bitval = n3;                     /* tnat */
            else   bitval = (int)((v >> bits(raw, 14, 6)) & 1);
            int res;
            int ctype;
            if (!ta && !tb) { res = !bitval ^ 0; ctype = c ? 1 : 0; res = !bitval; }
            else {
                /* and/or/or.andcm families; c selects z vs nz */
                res = c ? bitval : !bitval;
                if (!ta && tb) ctype = 2;
                else if (ta && !tb) ctype = 3;
                else ctype = 4;
            }
            set_preds(m, p1, p2, qp, res, ctype);
            return MERCED_OK;
        }
        if (x2 == 1 && !x) {                        /* extr.u / extr */
            if (!qp) return MERCED_OK;
            unsigned pos = (unsigned)bits(raw, 14, 6);
            unsigned len = (unsigned)bits(raw, 27, 6) + 1;
            unsigned sgn = (unsigned)bits(raw, 13, 1);
            uint64_t v = gr_read(m, r3, &n3) >> pos;
            if (len < 64) {
                v &= (1ull << len) - 1;
                if (sgn) v = (uint64_t)sext(v, len);
            }
            gr_write(m, r1, v, n3);
            return MERCED_OK;
        }
        if (x2 == 1 && x) {                         /* dep.z reg/imm */
            if (!qp) return MERCED_OK;
            unsigned y26 = (unsigned)bits(raw, 26, 1);
            unsigned pos = 63 - (unsigned)bits(raw, 20, 6);
            unsigned len = (unsigned)bits(raw, 27, 6) + 1;
            uint64_t a;
            uint8_t nat = 0;
            if (y26) a = (uint64_t)sext((bits(raw, 36, 1) << 7) | bits(raw, 13, 7), 8);
            else { a = gr_read(m, r2, &n2); nat = n2; }
            uint64_t mask = (len >= 64) ? ~0ull : ((1ull << len) - 1);
            gr_write(m, r1, (a & mask) << pos, nat);
            return MERCED_OK;
        }
        if (x2 == 3 && !x) {                        /* shrp */
            if (!qp) return MERCED_OK;
            unsigned count = (unsigned)bits(raw, 27, 6);
            uint64_t hi = gr_read(m, r2, &n2), lo = gr_read(m, r3, &n3);
            uint64_t res = count ? ((lo >> count) | (hi << (64 - count))) : lo;
            gr_write(m, r1, res, n2 | n3);
            return MERCED_OK;
        }
        if (x2 == 3 && x) {                         /* I14 dep imm1 */
            if (!qp) return MERCED_OK;
            unsigned pos = 63 - (unsigned)bits(raw, 14, 6);
            unsigned len = (unsigned)bits(raw, 27, 6) + 1;
            uint64_t a = bits(raw, 36, 1) ? ~0ull : 0;
            uint64_t b = gr_read(m, r3, &n3);
            uint64_t mask = (len >= 64) ? ~0ull : ((1ull << len) - 1);
            if (pos < 64) {
                uint64_t hole = mask << pos;
                gr_write(m, r1, (b & ~hole) | ((a & mask) << pos), n3);
            }
            return MERCED_OK;
        }
        return mhalt(m, "unimpl I-unit major 5 x2=%u x=%u", x2, x);
    }

    case 7: {
        unsigned za = (unsigned)bits(raw, 36, 1);
        unsigned x2a = (unsigned)bits(raw, 34, 2);
        unsigned zb = (unsigned)bits(raw, 33, 1);
        unsigned ve = (unsigned)bits(raw, 32, 1);
        unsigned x2c = (unsigned)bits(raw, 30, 2);
        unsigned x2b = (unsigned)bits(raw, 28, 2);
        if (ve) return mhalt(m, "I-unit major 7 ve=1");
        if (za == 1 && zb == 1) {
            if (!qp) return MERCED_OK;
            uint64_t a, b, res;
            if (x2a == 0 && x2b == 0 && x2c == 1) {         /* shl reg */
                a = gr_read(m, r2, &n2); b = gr_read(m, r3, &n3);
                res = (b >= 64) ? 0 : a << b;
                gr_write(m, r1, res, n2 | n3);
                return MERCED_OK;
            }
            if (x2a == 0 && x2c == 0 && (x2b == 0 || x2b == 2)) {  /* shr.u / shr */
                a = gr_read(m, r3, &n3); b = gr_read(m, r2, &n2);
                if (x2b == 0) res = (b >= 64) ? 0 : a >> b;
                else res = (b >= 64) ? (uint64_t)((int64_t)a >> 63)
                                     : (uint64_t)((int64_t)a >> b);
                gr_write(m, r1, res, n2 | n3);
                return MERCED_OK;
            }
        }
        if (za == 0 && zb == 1 && x2a == 1 && x2b == 1 && x2c == 2) {  /* popcnt */
            if (!qp) return MERCED_OK;
            uint64_t v = gr_read(m, r3, &n3);
            unsigned c = 0;
            while (v) { c += (unsigned)(v & 1); v >>= 1; }
            gr_write(m, r1, c, n3);
            return MERCED_OK;
        }
        if (x2a == 2) {                             /* I2 parallel (mix/unpack/pack/pmin/pmax/psad) */
            if (!qp) return MERCED_OK;
            uint64_t s1 = gr_read(m, r2, &n2), s2 = gr_read(m, r3, &n3), res = 0;
            uint8_t nat = n2 | n3;
            unsigned esz = za ? 4 : zb ? 2 : 1;     /* element bytes */
            unsigned n = 8 / esz;                    /* elements per reg */
            uint64_t emask = (esz >= 8) ? ~0ull : ((1ull << (esz * 8)) - 1);
            #define ELEM(v, i) (((v) >> ((i) * esz * 8)) & emask)
            #define PUT(i, val) (res |= ((uint64_t)(val) & emask) << ((i) * esz * 8))
            if (x2c == 2 && x2b != 3) {              /* mix.r (x2b=0) / mix.l (x2b=2) */
                unsigned off = (x2b == 0) ? 1 : 0;   /* r takes odd elems, l even */
                for (unsigned k = 0; k < n / 2; k++) {
                    PUT(2 * k,     ELEM(s1, 2 * k + off));
                    PUT(2 * k + 1, ELEM(s2, 2 * k + off));
                }
            } else if (x2c == 1 && (x2b == 0 || x2b == 2)) {  /* unpack.h/.l */
                unsigned base = (x2b == 0) ? 0 : n / 2;
                for (unsigned k = 0; k < n / 2; k++) {
                    PUT(2 * k,     ELEM(s1, base + k));
                    PUT(2 * k + 1, ELEM(s2, base + k));
                }
            } else if (x2c == 0 && (x2b == 0 || x2b == 2)) {  /* pack (sat, 2*esz->esz) */
                /* pack2: hw->byte; pack4: word->hw. za=1 => pack4, else pack2.
                 * x2b=0 unsigned-sat (pack2.uss), x2b=2 signed-sat. */
                unsigned dbytes = esz;               /* dest element size */
                unsigned sbytes = esz * 2;           /* src element size */
                unsigned sn = 8 / sbytes;            /* src elems per reg */
                uint64_t smask = (1ull << (sbytes * 8)) - 1;
                for (unsigned k = 0; k < sn; k++) {
                    for (int src = 0; src < 2; src++) {
                        uint64_t sv = src ? s1 : s2;
                        int64_t e = (int64_t)((sv >> (k * sbytes * 8)) & smask);
                        e = sext((uint64_t)e, sbytes * 8);
                        int64_t lim_hi = (1ll << (dbytes * 8 - 1)) - 1;
                        int64_t lim_lo, out;
                        if (x2b == 0) { lim_lo = 0; lim_hi = (1ll << (dbytes * 8)) - 1; }
                        else lim_lo = -(1ll << (dbytes * 8 - 1));
                        out = e < lim_lo ? lim_lo : e > lim_hi ? lim_hi : e;
                        unsigned di = k + (src ? sn : 0);
                        PUT(di, (uint64_t)out);
                    }
                }
            } else if (x2b == 1 && (x2c == 0 || x2c == 1)) {  /* pmin.u/pmax.u (1) or signed (2) */
                for (unsigned k = 0; k < n; k++) {
                    uint64_t a = ELEM(s1, k), b = ELEM(s2, k);
                    uint64_t r = (x2c == 0) ? (a < b ? a : b) : (a > b ? a : b);
                    PUT(k, r);
                }
            } else if (x2b == 3 && (x2c == 0 || x2c == 1)) {  /* pmin2/pmax2 signed */
                for (unsigned k = 0; k < n; k++) {
                    int64_t a = sext(ELEM(s1, k), esz * 8);
                    int64_t b = sext(ELEM(s2, k), esz * 8);
                    int64_t r = (x2c == 0) ? (a < b ? a : b) : (a > b ? a : b);
                    PUT(k, (uint64_t)r);
                }
            } else if (x2b == 3 && x2c == 2) {       /* psad1: sum of abs byte diffs */
                uint64_t acc = 0;
                for (unsigned k = 0; k < n; k++) {
                    int a = (int)ELEM(s1, k), b = (int)ELEM(s2, k);
                    acc += (uint64_t)(a > b ? a - b : b - a);
                }
                res = acc;
            } else {
                return mhalt(m, "unimpl I2 x2b=%u x2c=%u esz=%u", x2b, x2c, esz);
            }
            #undef ELEM
            #undef PUT
            gr_write(m, r1, res, nat);
            return MERCED_OK;
        }
        if (x2a == 3 && x2b == 2 && x2c == 2 && za == 0) {  /* I3 mux1 / I4 mux2 */
            if (!qp) return MERCED_OK;
            uint64_t a = gr_read(m, r2, &n2), res = 0;
            uint8_t sb[8];
            for (int i = 0; i < 8; i++) sb[i] = (uint8_t)(a >> (i * 8));
            if (zb == 0) {                              /* mux1: byte permute */
                unsigned mbt = (unsigned)bits(raw, 20, 4);
                static const int perm[16][8] = {
                    [0x0] = {7,7,7,7,7,7,7,7},          /* @brcst (byte 7) */
                    [0x8] = {0,4,2,6,1,5,3,7},          /* @mix */
                    [0x9] = {0,4,1,5,2,6,3,7},          /* @shuf */
                    [0xA] = {0,2,4,6,1,3,5,7},          /* @alt */
                    [0xB] = {7,6,5,4,3,2,1,0},          /* @rev */
                };
                if (mbt != 0 && mbt != 8 && mbt != 9 && mbt != 0xA && mbt != 0xB)
                    return mhalt(m, "mux1 reserved mbtype %u", mbt);
                for (int i = 0; i < 8; i++)
                    res |= (uint64_t)sb[perm[mbt][i]] << (i * 8);
            } else {                                    /* mux2: 16-bit permute */
                unsigned mht = (unsigned)bits(raw, 20, 8);
                uint16_t hw[4];
                for (int i = 0; i < 4; i++) hw[i] = (uint16_t)(a >> (i * 16));
                for (int i = 0; i < 4; i++)
                    res |= (uint64_t)hw[(mht >> (i * 2)) & 3] << (i * 16);
            }
            gr_write(m, r1, res, n2);
            return MERCED_OK;
        }
        return mhalt(m, "unimpl I-unit major 7 za=%u zb=%u x2a=%u x2b=%u x2c=%u",
                     za, zb, x2a, x2b, x2c);
    }
    }
    return mhalt(m, "unimpl I-unit major 0x%X", major);
}

/* ── M-unit dispatcher ───────────────────────────────────────────────────── */

static MercedStatus exec_m(Merced *m, uint64_t raw, int qp) {
    unsigned major = (unsigned)bits(raw, 37, 4);
    MercedStatus st;
    if (exec_alu(m, raw, qp, &st)) return st;
    if (st != MERCED_OK) return st;
    if (major <= 1) return exec_m_sys(m, raw, qp);
    if (major >= 4 && major <= 7) return exec_mem(m, raw, qp);
    return mhalt(m, "unimpl M-unit major 0x%X", major);
}

/* ── B-unit ──────────────────────────────────────────────────────────────── */

static MercedStatus exec_b(Merced *m, uint64_t raw, int qp) {
    unsigned major = (unsigned)bits(raw, 37, 4);
    unsigned btype = (unsigned)bits(raw, 6, 3);

    switch (major) {
    case 0: {
        unsigned x6 = (unsigned)bits(raw, 27, 6);
        switch (x6) {
        case 0x00: {                                /* break.b */
            uint64_t imm = (bits(raw, 36, 1) << 20) | bits(raw, 6, 20);
            if (!qp) return MERCED_OK;
            m->cr[CR_IIM] = imm;
            snprintf(m->halt_msg, sizeof(m->halt_msg), "break.b 0x%" PRIX64, imm);
            m->halt_ip = m->ip;
            return MERCED_HALT_BREAK;
        }
        case 0x02: {                                /* cover */
            uint64_t old = m->cfm;
            m->bof = (m->bof + CFM_SOF(old)) % MERCED_N_STACKED;
            m->bof_total += CFM_SOF(old);
            m->cfm = 0;
            if (!(m->psr & PSR_IC))
                m->cr[CR_IFS] = (old & CFM_MASK) | (1ull << 63);
            return MERCED_OK;
        }
        case 0x04: m->cfm &= 0x3FFFFull; return MERCED_OK;          /* clrrrb */
        case 0x05: m->cfm &= ~(0x3Full << 32); return MERCED_OK;    /* clrrrb.pr */
        case 0x08: {                                /* rfi */
            uint64_t ipsr = m->cr[CR_IPSR];
            uint64_t iip = m->cr[CR_IIP];
            psr_trans_log(m, ipsr, "rfi");
            m->psr = ipsr & ~(3ull << PSR_RI_SHIFT);
            if (ipsr & PSR_IS)
                m->ip = (uint32_t)iip;
            else
                m->ip = (iip & ~0xFull) | ((ipsr >> PSR_RI_SHIFT) & 3);
            if (m->cr[CR_IFS] >> 63) {
                uint64_t new_cfm = m->cr[CR_IFS] & CFM_MASK;
                /* undo cover's frame advance */
                m->bof = (m->bof + MERCED_N_STACKED - CFM_SOF(new_cfm))
                         % MERCED_N_STACKED;
                m->bof_total -= CFM_SOF(new_cfm);
                m->cfm = new_cfm;
            }
            m->taken = 1;
            return MERCED_OK;
        }
        case 0x0C: m->psr &= ~PSR_BN; return MERCED_OK;             /* bsw.0 */
        case 0x0D: m->psr |= PSR_BN; return MERCED_OK;              /* bsw.1 */
        case 0x10: return MERCED_OK;                                /* epc */
        case 0x20: {                                /* br.cond/br.ia b2 */
            if (!qp) return MERCED_OK;
            if (btype == 1) {
                unsigned b = (unsigned)bits(raw, 13, 3);
                if (m->psr & PSR_DI)
                    return mhalt(m, "br.ia with disabled ISA transitions");
                /* Itanium SDM vol. 2, 9.1.2: the target has byte rather
                 * than bundle granularity and is restricted to BR{31:0}.
                 * IA-32 maps CSD/SSD into GR25/GR26 while executing. */
                m->ip = (uint32_t)m->br[b];
                m->psr |= PSR_IS;
                m->psr &= ~(PSR_DA | PSR_DD | PSR_IA | PSR_ED |
                            (3ull << PSR_RI_SHIFT));
                m->cfm = 0;
                gr_write(m, 25, m->ar[25], 0);       /* ar.csd -> CSD */
                gr_write(m, 26, m->ar[26], 0);       /* ar.ssd -> SSD */
                m->taken = 1;
                return MERCED_OK;
            }
            m->ip = m->br[bits(raw, 13, 3)] & ~0xFull;
            m->taken = 1;
            return MERCED_OK;
        }
        case 0x21:                                  /* br.ret b2 */
            if (!qp) return MERCED_OK;
            do_ret(m, m->br[bits(raw, 13, 3)] & ~0xFull);
            return MERCED_OK;
        }
        return mhalt(m, "unimpl B-unit major 0 x6=0x%02X", x6);
    }
    case 1:                                         /* br.call b1=b2 */
        if (!qp) return MERCED_OK;
        do_call(m, (unsigned)bits(raw, 6, 3), m->br[bits(raw, 13, 3)] & ~0xFull);
        return MERCED_OK;
    case 2: {
        unsigned x6 = (unsigned)bits(raw, 27, 6);
        if (x6 == 0x00 || x6 == 0x01) return MERCED_OK;   /* nop.b / hint.b */
        if (x6 == 0x10 || x6 == 0x11) return MERCED_OK;   /* brp */
        return mhalt(m, "unimpl B-unit major 2 x6=0x%02X", x6);
    }
    case 4: {                                       /* br.cond/wexit/wtop/cloop/cexit/ctop */
        int64_t disp = sext((bits(raw, 36, 1) << 20) | bits(raw, 13, 20), 21) << 4;
        uint64_t target = (m->ip & ~0xFull) + (uint64_t)disp;
        switch (btype) {
        case 0:                                     /* br.cond */
            if (qp) { m->ip = target; m->taken = 1; }
            return MERCED_OK;
        case 5:                                     /* br.cloop */
            if (m->ar[AR_LC] != 0) { m->ar[AR_LC]--; m->ip = target; m->taken = 1; }
            return MERCED_OK;
        case 6:                                     /* br.cexit */
            if (do_ctop(m, 0)) { m->ip = target; m->taken = 1; }
            return MERCED_OK;
        case 7:                                     /* br.ctop */
            if (do_ctop(m, 1)) { m->ip = target; m->taken = 1; }
            return MERCED_OK;
        case 2:                                     /* br.wexit */
            if (do_wtop(m, qp, 0)) { m->ip = target; m->taken = 1; }
            return MERCED_OK;
        case 3:                                     /* br.wtop */
            if (do_wtop(m, qp, 1)) { m->ip = target; m->taken = 1; }
            return MERCED_OK;
        }
        return mhalt(m, "unimpl B-unit major 4 btype=%u", btype);
    }
    case 5: {                                       /* br.call target25 */
        if (!qp) return MERCED_OK;
        int64_t disp = sext((bits(raw, 36, 1) << 20) | bits(raw, 13, 20), 21) << 4;
        do_call(m, (unsigned)bits(raw, 6, 3), (m->ip & ~0xFull) + (uint64_t)disp);
        return MERCED_OK;
    }
    case 7: return MERCED_OK;                       /* brp target25 */
    }
    return mhalt(m, "unimpl B-unit major 0x%X", major);
}

/* ── X-unit (uses L slot payload) ────────────────────────────────────────── */

static MercedStatus exec_x(Merced *m, uint64_t raw, uint64_t lraw, int qp) {
    unsigned major = (unsigned)bits(raw, 37, 4);

    if (major == 6) {                               /* movl */
        if (!qp) return MERCED_OK;
        uint64_t imm = (bits(raw, 36, 1) << 63) | (lraw << 22) |
                       (bits(raw, 21, 1) << 21) | (bits(raw, 22, 5) << 16) |
                       (bits(raw, 27, 9) << 7) | bits(raw, 13, 7);
        gr_write(m, (unsigned)bits(raw, 6, 7), imm, 0);
        return MERCED_OK;
    }
    if (major == 0xC || major == 0xD) {             /* brl.cond / brl.call */
        if (!qp) return MERCED_OK;
        uint64_t i = bits(raw, 36, 1);
        uint64_t imm39 = bits(lraw, 2, 39);
        uint64_t imm20b = bits(raw, 13, 20);
        int64_t disp = sext((i << 59) | (imm39 << 20) | imm20b, 60) << 4;
        uint64_t target = (m->ip & ~0xFull) + (uint64_t)disp;
        if (major == 0xD) {
            do_call(m, (unsigned)bits(raw, 6, 3), target);
        } else {
            m->ip = target;
            m->taken = 1;
        }
        return MERCED_OK;
    }
    if (major == 0) {
        unsigned x3 = (unsigned)bits(raw, 33, 3);
        unsigned x6 = (unsigned)bits(raw, 27, 6);
        if (x3 == 0 && x6 == 0x01) return MERCED_OK;   /* nop.x */
        if (x3 == 0 && x6 == 0x00) {                   /* break.x */
            uint64_t imm = (bits(raw, 36, 1) << 20) | bits(raw, 6, 20);
            if (!qp) return MERCED_OK;
            m->cr[CR_IIM] = imm;
            snprintf(m->halt_msg, sizeof(m->halt_msg), "break.x 0x%" PRIX64, imm);
            m->halt_ip = m->ip;
            return MERCED_HALT_BREAK;
        }
    }
    return mhalt(m, "unimpl X-unit major 0x%X", major);
}

/* ── F-unit (minimal) ────────────────────────────────────────────────────── */

static MercedStatus exec_f(Merced *m, uint64_t raw, int qp) {
    unsigned major = (unsigned)bits(raw, 37, 4);
    unsigned f1 = (unsigned)bits(raw, 6, 7);
    unsigned f2 = (unsigned)bits(raw, 13, 7);
    unsigned f3 = (unsigned)bits(raw, 20, 7);
    unsigned f4 = (unsigned)bits(raw, 27, 7);

    if (major == 0 || major == 1) {
        unsigned x = (unsigned)bits(raw, 33, 1);
        unsigned x6 = (unsigned)bits(raw, 27, 6);
        if (!x) {
            switch (x6) {
            case 0x00: {                            /* break.f */
                if (!qp) return MERCED_OK;
                snprintf(m->halt_msg, sizeof(m->halt_msg), "break.f");
                m->halt_ip = m->ip;
                return MERCED_HALT_BREAK;
            }
            case 0x01: return MERCED_OK;            /* nop.f / hint.f */
            case 0x04: {                            /* fsetc */
                if (!qp) return MERCED_OK;
                uint64_t amask = bits(raw, 13, 7), omask = bits(raw, 20, 7);
                uint64_t sf0 = m->ar[AR_FPSR] >> 6 & 0x1FFF;
                sf0 = (sf0 & (amask | ~0x7Full)) | omask;
                m->ar[AR_FPSR] = (m->ar[AR_FPSR] & ~(0x1FFFull << 6)) | (sf0 << 6);
                return MERCED_OK;
            }
            case 0x05:                              /* fclrf */
                if (qp) m->ar[AR_FPSR] &= ~(0x3Full << 13);
                return MERCED_OK;
            case 0x08: return MERCED_OK;            /* fchkf */
            case 0x10: case 0x11: case 0x12: {      /* fmerge.s/ns/se */
                if (!qp) return MERCED_OK;
                MercedFpReg a = fr_read(m, f2), b = fr_read(m, f3), r = b;
                if (x6 == 0x10) r.sign = a.sign;
                else if (x6 == 0x11) r.sign = !a.sign;
                else { r.sign = a.sign; r.exp = a.exp; }
                fr_write(m, f1, r);
                return MERCED_OK;
            }
            case 0x18: case 0x19: case 0x1A: case 0x1B: {
                /* fcvt.fx / fcvt.fxu / fcvt.fx.trunc / fcvt.fxu.trunc */
                if (!qp) return MERCED_OK;
                MercedFpReg a = fr_read(m, f2);
                MercedFpReg r = {0, 0x1003E, 0, a.nat};
                long double d = fp2d(a);
                /* only truncating rounding modeled; sf rounding control is
                 * a refinement for later */
                if (x6 & 1)
                    r.sig = (d <= 0) ? 0 : (uint64_t)d;
                else
                    r.sig = (uint64_t)(int64_t)d;
                fr_write(m, f1, r);
                return MERCED_OK;
            }
            case 0x1C: {                            /* fcvt.xf (int → fp) */
                if (!qp) return MERCED_OK;
                MercedFpReg a = fr_read(m, f2), r;
                r = d2fp((double)(int64_t)a.sig);
                r.nat = a.nat;
                fr_write(m, f1, r);
                return MERCED_OK;
            }
            case 0x3A: case 0x3B: case 0x3C: case 0x3D: {
                /* fmix.r/.l, fsxt.r/.l - treat FP regs as 64-bit integer
                 * data (two 32-bit words) and shuffle the halves. */
                if (!qp) return MERCED_OK;
                MercedFpReg a = fr_read(m, f2), b = fr_read(m, f3);
                MercedFpReg r = {0, 0x1003E, 0, (uint8_t)(a.nat | b.nat)};
                uint32_t a_lo = (uint32_t)a.sig, a_hi = (uint32_t)(a.sig >> 32);
                uint32_t b_lo = (uint32_t)b.sig, b_hi = (uint32_t)(b.sig >> 32);
                uint32_t lo, hi;
                switch (x6) {
                case 0x3A: lo = a_hi; hi = b_hi; break;               /* fmix.r */
                case 0x3B: lo = a_lo; hi = b_lo; break;               /* fmix.l */
                case 0x3C: lo = (a_hi & 1) ? ~0u : 0u; hi = b_hi; break; /* fsxt.r */
                default:   lo = (uint32_t)((int32_t)a_lo >> 31); hi = b_lo; break; /* fsxt.l */
                }
                r.sig = (uint64_t)lo | ((uint64_t)hi << 32);
                fr_write(m, f1, r);
                return MERCED_OK;
            }
            }
            return mhalt(m, "unimpl F-unit major %u x6=0x%02X", major, x6);
        }
        /* x=1: fma-family (F1) handled below via major 8-D; frcpa (F6) here */
        unsigned q = (unsigned)bits(raw, 36, 1);
        if (!q) {                                   /* frcpa */
            if (!qp) return MERCED_OK;
            warn_once(m, WARN_FP_APPROX, "FP ops use double-precision approximation");
            unsigned p2 = (unsigned)bits(raw, 27, 6);
            MercedFpReg a = fr_read(m, f2);
            MercedFpReg b = fr_read(m, f3);
            long double num = fp2d(a), den = fp2d(b);
            /* frcpa approximates 1/den only - num (f2) is a source purely for
             * the architected NaN/zero/inf special-case checks below, never
             * multiplied in. Compiler-generated Newton-Raphson division
             * sequences rely on this: they multiply the reciprocal by the
             * numerator themselves in a later fma. Returning num/den here
             * (as an earlier version of this code did) silently corrupts
             * every runtime integer/float division that goes through such a
             * sequence, since the caller's next step becomes
             * num*(num/den) = num^2/den instead of num/den. Confirmed
             * against reference/ski/src/float.c's frcpa()/ieee_recip(). */
            if (num != 0.0 && den != 0.0) {
                /* Architecturally this should be fp_recip_estimate(b) (the
                 * real 8-bit table) - see the long comment on that function
                 * for why a full-precision reciprocal is used here instead
                 * for now. */
                fr_write(m, f1, d2fp(1.0L / den));
                pr_write(m, p2, 1);
            } else {
                pr_write(m, p2, 0);
            }
            return MERCED_OK;
        }
        return mhalt(m, "unimpl F-unit major %u x=1 q=1 (frsqrta?)", major);
    }

    if (major >= 8 && major <= 0xD) {               /* F1 fma family */
        if (!qp) return MERCED_OK;
        unsigned xbit = (unsigned)bits(raw, 36, 1);
        if (xbit && (major & 1)) {
            /* fpma (parallel) - unimplemented */
            return mhalt(m, "unimpl F-unit fpma/parallel major %u", major);
        }
        warn_once(m, WARN_FP_APPROX, "FP ops use double-precision approximation");
        long double a = fp2d(fr_read(m, f3));
        long double bmul = fp2d(fr_read(m, f4));
        long double c = fp2d(fr_read(m, f2));
        long double r;
        switch (major) {
        case 8: case 9:  r = fmal(a, bmul, c); break;    /* fma */
        case 0xA: case 0xB: r = fmal(a, bmul, -c); break;/* fms */
        default: r = fmal(-a, bmul, c); break;           /* fnma */
        }
        /* Even major, x=0 is dynamic precision selected by the instruction's
         * FPSR status field; even major, x=1 is explicitly single precision.
         * Odd major, x=0 is double precision (odd/x=1 is parallel). */
        unsigned precision;
        if (major & 1) {
            precision = 2;
        } else if (xbit) {
            precision = 1;
        } else {
            unsigned sf = (unsigned)bits(raw, 34, 2);
            /* FPSR.sfN.pc occupies bits 8:9 of sf0, with each subsequent
             * status field starting 13 bits higher. */
            unsigned pc = (unsigned)((m->ar[AR_FPSR] >> (8 + 13 * sf)) & 3);
            precision = (pc == 0) ? 1 : (pc == 2 ? 2 : 0);
        }
        fr_write(m, f1, fp_result_static(r, precision));
        return MERCED_OK;
    }

    if (major == 0xE) {
        unsigned xbit = (unsigned)bits(raw, 36, 1);
        unsigned x2 = (unsigned)bits(raw, 34, 2);
        if (xbit) {                                 /* xma.l/.h/.hu */
            if (!qp) return MERCED_OK;
            uint64_t a = fr_read(m, f3).sig;
            uint64_t b = fr_read(m, f4).sig;
            uint64_t c = fr_read(m, f2).sig;
            MercedFpReg r = {0, 0x1003E, 0, 0};
            if (x2 == 0) {                          /* xma.l */
                r.sig = a * b + c;
            } else {
                /* 128-bit high product */
                uint64_t ah = a >> 32, al = (uint32_t)a;
                uint64_t bh = b >> 32, bl = (uint32_t)b;
                uint64_t t0 = al * bl;
                uint64_t t1 = ah * bl + (t0 >> 32);
                uint64_t t2 = al * bh + (uint32_t)t1;
                uint64_t hi = ah * bh + (t1 >> 32) + (t2 >> 32);
                if (x2 == 3) {                      /* xma.h signed adjust */
                    if ((int64_t)a < 0) hi -= b;
                    if ((int64_t)b < 0) hi -= a;
                }
                uint64_t lo = a * b;
                uint64_t sum = lo + c;
                hi += (sum < lo);
                r.sig = hi;
            }
            if (merced_dbg()) {
                static uint64_t n;
                n++;
                fprintf(stderr, "XMA #%" PRIu64 " x2=%u f1=%u a=%016" PRIX64
                        " b=%016" PRIX64 " c=%016" PRIX64
                        " -> %016" PRIX64 " ip=%016" PRIX64 "\n",
                        n, x2, f1, a, b, c, r.sig, m->ip);
            }
            fr_write(m, f1, r);
            return MERCED_OK;
        }
        (void)x2;
        return mhalt(m, "unimpl F-unit major E x=0 (fselect?)");
    }

    if (major == 4) {                               /* F4 fcmp */
        if (!qp) return MERCED_OK;
        warn_once(m, WARN_FP_APPROX, "FP ops use double-precision approximation");
        unsigned rb = (unsigned)bits(raw, 36, 1);
        unsigned ra = (unsigned)bits(raw, 33, 1);
        unsigned ta = (unsigned)bits(raw, 12, 1);
        unsigned p1 = (unsigned)bits(raw, 6, 6);
        unsigned p2 = (unsigned)bits(raw, 27, 6);
        long double a = fp2d(fr_read(m, f2)), b = fp2d(fr_read(m, f3));
        int res;
        switch ((ra << 1) | rb) {
        case 0: res = a == b; break;
        case 1: res = a < b; break;
        case 2: res = a <= b; break;
        default: res = (a != a) || (b != b); break; /* unord */
        }
        set_preds(m, p1, p2, qp, res, ta ? 1 : 0);
        return MERCED_OK;
    }

    if (major == 5) {                               /* F5 fclass */
        if (!qp) return MERCED_OK;
        unsigned p1 = (unsigned)bits(raw, 6, 6);
        unsigned p2 = (unsigned)bits(raw, 27, 6);
        unsigned ta = (unsigned)bits(raw, 12, 1);
        uint64_t fclass = (bits(raw, 20, 7) << 2) | bits(raw, 33, 2);
        MercedFpReg a = fr_read(m, f2);
        int res = 0;
        /* coarse classification: nat / zero / pos / neg */
        if ((fclass & 0x100) && a.nat) res = 1;              /* @nat */
        if ((fclass & 0x004) && a.exp == 0 && a.sig == 0) res = 1;   /* zero */
        if ((fclass & 0x001) && !a.sign) res = 1;            /* @pos */
        if ((fclass & 0x002) && a.sign) res = 1;             /* @neg */
        set_preds(m, p1, p2, qp, res, ta ? 1 : 0);
        return MERCED_OK;
    }

    return mhalt(m, "unimpl F-unit major 0x%X", major);
}

/* ── Fetch/execute ───────────────────────────────────────────────────────── */

static const char bundle_units[32][4] = {
    /* 00 */ "MII", "MII", "MII", "MII", "MLX", "MLX", "??", "??",
    /* 08 */ "MMI", "MMI", "MMI", "MMI", "MFI", "MFI", "MMF", "MMF",
    /* 10 */ "MIB", "MIB", "MBB", "MBB", "??", "??", "BBB", "BBB",
    /* 18 */ "MMB", "MMB", "??", "??", "MFB", "MFB", "??", "??",
};

bool merced_ia32_read(Merced *m, uint64_t va, unsigned size,
                      bool ifetch, uint64_t *value) {
    uint64_t pa;
    MercedStatus st;
    if (!va_translate(m, va, ifetch, false, &pa, &st))
        return false;
    *value = ifetch ? phys_fetch(m, pa, size) : phys_read(m, pa, size);
    return true;
}

bool merced_ia32_write(Merced *m, uint64_t va, unsigned size,
                       uint64_t value) {
    uint64_t pa;
    MercedStatus st;
    if (!va_translate(m, va, false, false, &pa, &st))
        return false;
    phys_write(m, pa, value, size);
    return true;
}

uint64_t merced_ia32_gr_read(Merced *m, unsigned reg) {
    return gr_read(m, reg, NULL);
}

void merced_ia32_gr_write(Merced *m, unsigned reg, uint64_t value) {
    gr_write(m, reg, value, 0);
}

MercedStatus merced_step(Merced *m) {
    if (m->psr & PSR_IS)
        return merced_ia32_step(m);
    if (m->external_pending && (m->psr & PSR_I) && (m->psr & PSR_IC)) {
        MercedStatus ist = deliver_fault(m, VEC_EXTINT, 0, 0, false);
        m->ninsts++;
        return ist;
    }

    /* Deliver a latched interval-timer external interrupt at the next
     * instruction boundary where interrupts are actually enabled. Mirrors
     * real hardware: delivery clears psr.i (via deliver_fault below), so
     * this naturally can't re-fire until firmware explicitly re-enables
     * interrupts or acks the timer through a cr.ivr read. */
    if (m->timer_pending && !(m->cr[CR_ITV] & (1ull << 16)) &&
        (m->psr & PSR_I) && (m->psr & PSR_IC)) {
        MercedStatus ist = deliver_fault(m, VEC_EXTINT, 0, 0, false);
        m->ninsts++;
        return ist;
    }

    uint64_t bundle_va = m->ip & ~0xFull;
    unsigned slot = (unsigned)(m->ip & 0xF);
    uint64_t pa;
    MercedStatus st;

    /* EXPERIMENTAL, not a real fix: VA 0 is never legitimately executable.
     * Reaching it is the signature of an indirect call through a null IA-64
     * function descriptor (entry point and gp both read back as 0 from
     * zero-filled RAM). Real firmware is presumably supposed to null-check
     * before such a call; something upstream isn't, or the check we haven't
     * found yet is being bypassed. Rather than crash, treat it as if the
     * call immediately returned (br.ret b0), to see how much further boot
     * gets past this specific landmine. This is a diagnostic bisection aid,
     * not a fix for the real bug (see i2000 project memory, "third
     * investigation round" and later, 2026-07-18). */
    if (bundle_va == 0) {
        static unsigned hits;
        uint64_t return_ip = m->br[0] & ~0xFull;
        /* A null service descriptor is an unavailable PIM service.  Never
         * return a stale success/status value: callers use r8 to decide
         * whether dependent modules may be initialized. */
        gr_write(m, 8, ~0ull, 0);
        if (hits < 50) {
            hits++;
            fprintf(stderr, "merced: WORKAROUND null-descriptor call at "
                    "ip=%016" PRIX64 ", synthesizing br.ret b0=%016" PRIX64
                    " (hit #%u)\n", m->ip, m->br[0], hits);
        }
        /* The CDB query returning to 0x7FE86D90 has an explicit non-zero
         * error path.  A missing callback cannot mean success: leaving the
         * old r8 value at zero makes its caller consume untouched output
         * parameters (one is initialized to -1) and fault later. */
        if (return_ip == 0x7FE867B0ull) {
            /* CDB slot 0 initializes an opaque query handle through arg0. */
            uint64_t out = gr_read(m, 32, NULL), out_pa;
            if (va_translate(m, out, false, false, &out_pa, &st))
                phys_write(m, out_pa, 0, 8);
        } else if (return_ip == 0x7FE867E0ull) {
            /* CDB slot 1 returns an iterable result through arg3.  Supply a
             * valid empty result using the caller's adjacent count-output
             * storage (arg4), so its normal zero-count path is exercised. */
            uint64_t out = gr_read(m, 35, NULL);
            uint64_t empty = gr_read(m, 36, NULL);
            uint64_t out_pa, empty_pa;
            if (va_translate(m, empty, false, false, &empty_pa, &st) &&
                va_translate(m, out, false, false, &out_pa, &st)) {
                phys_write(m, empty_pa, 0, 8);
                phys_write(m, out_pa, empty, 8);
                gr_write(m, 8, 0, 0);
            }
        }
        do_ret(m, return_ip);
        m->ninsts++;
        return MERCED_OK;
    }

    if (slot > 2) return mhalt(m, "bad IP slot %u", slot);
    if (!va_translate(m, bundle_va, true, false, &pa, &st))
        return st;   /* ITLB miss delivered (or halt) */

    uint64_t lo = phys_fetch(m, pa, 8);
    uint64_t hi = phys_fetch(m, pa + 8, 8);
    unsigned tmpl = (unsigned)(lo & 0x1F);
    const char *units = bundle_units[tmpl];
    if (units[0] == '?')
        return mhalt(m, "reserved bundle template 0x%02X", tmpl);

    uint64_t slots[3];
    slots[0] = (lo >> 5) & 0x1FFFFFFFFFFull;
    slots[1] = ((lo >> 46) | (hi << 18)) & 0x1FFFFFFFFFFull;
    slots[2] = (hi >> 23) & 0x1FFFFFFFFFFull;

    /* SAL clears discovered RAM with two post-increment stores and a
     * br.cloop.  A 2 GiB clear otherwise costs over 400 million interpreted
     * slots, so preserve its architectural effects while filling direct
     * machine memory in one operation. */
    if (slot == 0 && m->bus.fill && units[0] == 'M' && units[1] == 'M' &&
        units[2] == 'B' &&
        bits(slots[0], 37, 4) == 5 && bits(slots[1], 37, 4) == 5 &&
        bits(slots[0], 30, 6) == 0x33 && bits(slots[1], 30, 6) == 0x33 &&
        bits(slots[0], 0, 6) == 0 && bits(slots[1], 0, 6) == 0 &&
        bits(slots[0], 13, 7) == 0 && bits(slots[1], 13, 7) == 0 &&
        bits(slots[0], 6, 7) == 16 && bits(slots[1], 6, 7) == 16 &&
        bits(slots[2], 37, 4) == 4 && bits(slots[2], 6, 3) == 5 &&
        bits(slots[2], 0, 6) == 0) {
        unsigned ra = (unsigned)bits(slots[0], 20, 7);
        unsigned rb = (unsigned)bits(slots[1], 20, 7);
        uint8_t na, nb;
        uint64_t va = gr_read(m, ra, &na), vb = gr_read(m, rb, &nb);
        int64_t disp = sext((bits(slots[2], 36, 1) << 20) |
                            bits(slots[2], 13, 20), 21) << 4;
        uint64_t iterations = m->ar[AR_LC] + 1;
        uint64_t len = iterations * 16;
        uint64_t fill_pa;
        if (!na && !nb && vb == va + 8 && disp == 0 &&
            iterations <= UINT64_MAX / 16 &&
            va_translate(m, va, false, false, &fill_pa, &st) &&
            m->bus.fill(m->bus.ud, fill_pa, 0, len)) {
            gr_write(m, ra, va + len, 0);
            gr_write(m, rb, vb + len, 0);
            m->ar[AR_LC] = 0;
            m->ninsts += iterations * 3;
            itc_advance(m, iterations * 3);
            m->ip = bundle_va + 16;
            return MERCED_OK;
        }
    }

    /* Firmware delay/calibration loops commonly consist solely of
     *
     *     nop.m; nop.i; br.cloop <same bundle>
     *
     * Executing hundreds of millions of architecturally empty iterations
     * only burns host time.  Complete the loop in one step while advancing
     * the architected instruction and interval-time counters by exactly the
     * number of slots that sequential execution would have consumed. */
    if (slot == 0 && units[0] == 'M' && units[1] == 'I' && units[2] == 'B' &&
        slots[0] == 0x00008000000ull && slots[1] == 0x00008000000ull &&
        bits(slots[2], 37, 4) == 4 && bits(slots[2], 6, 3) == 5 &&
        bits(slots[2], 0, 6) == 0) {
        int64_t disp = sext((bits(slots[2], 36, 1) << 20) |
                            bits(slots[2], 13, 20), 21) << 4;
        if (disp == 0) {
            uint64_t iterations = m->ar[AR_LC] + 1;
            uint64_t executed = iterations * 3;
            m->ar[AR_LC] = 0;
            m->ninsts += executed;
            itc_advance(m, executed);
            m->ip = bundle_va + 16;
            return MERCED_OK;
        }
    }

    /* Preserve the path into the conventional firmware dead loop instead of
     * executing it forever and overwriting the useful trace history. */
    if (slot == 0 && units[0] == 'M' && units[1] == 'I' && units[2] == 'B' &&
        slots[0] == 0x00008000000ull && slots[1] == 0x00008000000ull &&
        bits(slots[2], 37, 4) == 4 && bits(slots[2], 6, 3) == 0 &&
        bits(slots[2], 0, 6) == 0) {
        int64_t disp = sext((bits(slots[2], 36, 1) << 20) |
                            bits(slots[2], 13, 20), 21) << 4;
        if (disp == 0) {
            snprintf(m->halt_msg, sizeof(m->halt_msg),
                     "firmware dead loop at 0x%016" PRIX64, bundle_va);
            m->halt_ip = m->ip;
            return MERCED_HALT_DEADLOOP;
        }
    }

    char unit = units[slot];
    if (unit == 'L') {
        /* branching into the L slot of an MLX is illegal; treat the L+X
         * pair as executing at slot 1 */
        unit = 'X';
    }
    uint64_t raw = slots[slot];
    uint64_t lraw = 0;
    if (unit == 'X') {
        raw = slots[2];
        lraw = slots[1];
    }

    int qp = pr_read(m, (unsigned)bits(raw, 0, 6));
    m->taken = 0;

    /* SDV's PE/COFF relocation walker assumes every base-relocation block
     * has a non-zero SizeOfBlock.  A malformed/truncated image otherwise
     * computes next == current and loops forever.  Follow the routine's own
     * EFI_LOAD_ERROR (-3) epilogue instead of manufacturing a successful
     * result and later calling through an invalid function descriptor. */
    if ((bundle_va == 0x000000007FF2B260ull && slot == 2) ||
        (bundle_va == 0x000000007FF2B810ull && slot == 0)) {
        uint64_t sp = gr_read(m, 12, NULL);
        uint64_t block = phys_read(m, sp + 24, 8);
        if (phys_read(m, block + 4, 4) == 0) {
            m->ip = bundle_va == 0x000000007FF2B260ull
                        ? 0x000000007FF2B550ull
                        : 0x000000007FF2CE00ull;
            m->taken = 1;
            m->ninsts++;
            itc_advance(m, 1);
            return MERCED_OK;
        }
    }

    /* EXPERIMENTAL WORKAROUND: bios130.BIN spins here (0x7FE281D0-0x7FE281EC)
     * waiting for memory at r37 to become the literal 18 (an A8-type
     * cmp.eq p6,p7=18,r38 immediate compare, not a register compare - the
     * "18" in objdump's disassembly is a plain immediate, not r18). cr.itv
     * was deliberately programmed masked just before this, so it's not a
     * plain interval-timer wait; it looks like an unmodeled event/status
     * self-test waiting for a specific completion code. Rather than guess
     * the exact PAL/SAL semantics, supply the value directly and see what
     * the next blocker reveals. The whole routine (0x7FE28110) re-zeroes
     * the target and re-enters this wait on retry, so the iteration count
     * resets on every fresh entry rather than firing once ever. */
    {
        static unsigned rendezvous_n;
        if (bundle_va == 0x000000007FE28110ull && slot == 0) {
            rendezvous_n = 0;
        } else if (bundle_va == 0x000000007FE281D0ull && slot == 0) {
            if (++rendezvous_n == 200000) {
                uint64_t addr = gr_read(m, 37, NULL);
                phys_write(m, addr, 18, 8);
            }
        }
    }

    unsigned hist = m->trace_history_next++ % MERCED_TRACE_HISTORY;
    m->trace_history[hist].ip = bundle_va | slot;
    m->trace_history[hist].raw = raw;
    m->trace_history[hist].src2 = gr_read(m, (unsigned)bits(raw, 13, 7), NULL);
    m->trace_history[hist].src3 = gr_read(m, (unsigned)bits(raw, 20, 7), NULL);
    m->trace_history[hist].r25 = gr_read(m, 25, NULL);
    m->trace_history[hist].b0 = m->br[0];
    m->trace_history[hist].unit = (uint8_t)unit;
    m->trace_history[hist].qp = (uint8_t)qp;

    if (m->trace_n) {
        m->trace_n--;
        fprintf(stderr, "T %016" PRIX64 ".%u %c%s maj=%X raw=%011" PRIX64
                        " qp=p%u=%d r1=%u r2=%u r3=%u\n",
                bundle_va, slot, unit, qp ? " " : "-",
                (unsigned)bits(raw, 37, 4), raw,
                (unsigned)bits(raw, 0, 6), qp,
                (unsigned)bits(raw, 6, 7), (unsigned)bits(raw, 13, 7),
                (unsigned)bits(raw, 20, 7));
    }

    switch (unit) {
    case 'M': st = exec_m(m, raw, qp); break;
    case 'I': st = exec_i(m, raw, qp); break;
    case 'B': st = exec_b(m, raw, qp); break;
    case 'F': st = exec_f(m, raw, qp); break;
    case 'X': st = exec_x(m, raw, lraw, qp); break;
    default:  st = mhalt(m, "internal: unit %c", unit); break;
    }

    if (st != MERCED_OK) {
        /* enrich unimpl diagnostics with raw bits */
        if (st == MERCED_HALT_UNIMPL) {
            size_t len = strlen(m->halt_msg);
            snprintf(m->halt_msg + len, sizeof(m->halt_msg) - len,
                     " [%c-slot %u tmpl %s raw=0x%011" PRIX64 "]",
                     unit, slot, bundle_units[tmpl], raw);
        }
        return st;
    }

    m->ninsts++;
    itc_advance(m, 1);

    if (m->taken) {
        m->taken = 0;
        return MERCED_OK;                     /* branch/fault redirected IP */
    }

    /* advance within bundle: X consumes slots 1+2 */
    if (unit == 'X' || slot == 2)
        m->ip = bundle_va + 16;
    else
        m->ip = bundle_va | (slot + 1);
    return MERCED_OK;
}

/* ── Debug dump ──────────────────────────────────────────────────────────── */

void merced_dump_trace(const Merced *m, unsigned count, FILE *out) {
    unsigned available = m->trace_history_next < MERCED_TRACE_HISTORY
                       ? m->trace_history_next : MERCED_TRACE_HISTORY;
    if (count > available) count = available;
    unsigned first = m->trace_history_next - count;
    for (unsigned i = first; i < m->trace_history_next; i++) {
        unsigned h = i % MERCED_TRACE_HISTORY;
        fprintf(out, "T %016" PRIX64 ".%u %c%c raw=%011" PRIX64
                     " s2=%016" PRIX64 " s3=%016" PRIX64
                     " r25=%016" PRIX64 " b0=%016" PRIX64 "\n",
                (uint64_t)(m->trace_history[h].ip & ~(uint64_t)0xF),
                (unsigned)(m->trace_history[h].ip & 0xF),
                m->trace_history[h].unit,
                m->trace_history[h].qp ? ' ' : '-',
                m->trace_history[h].raw, m->trace_history[h].src2,
                m->trace_history[h].src3, m->trace_history[h].r25,
                m->trace_history[h].b0);
    }
}

void merced_dump_calls(const Merced *m, unsigned count, FILE *out) {
    unsigned available = m->call_history_next < MERCED_CALL_HISTORY
                       ? m->call_history_next : MERCED_CALL_HISTORY;
    if (count > available) count = available;
    unsigned first = m->call_history_next - count;
    for (unsigned i = first; i < m->call_history_next; i++) {
        unsigned h = i % MERCED_CALL_HISTORY;
        fprintf(out, "C %s %016" PRIX64 ".%u -> %016" PRIX64 "\n",
                m->call_history[h].is_return ? "ret " : "call",
                (uint64_t)(m->call_history[h].from & ~(uint64_t)0xF),
                (unsigned)(m->call_history[h].from & 0xF),
                m->call_history[h].to);
    }
}

void merced_dump_state(const Merced *m, char *buf, size_t len) {
    size_t o = 0;
    #define P(...) do { if (o < len) o += (size_t)snprintf(buf + o, len - o, __VA_ARGS__); } while (0)
    P("IP  %016" PRIX64 "  slot %u   ninsts %" PRIu64 "  faults %" PRIu64 "\n",
      m->ip & ~0xFull, (unsigned)(m->ip & 0xF), m->ninsts, m->nfaults);
    P("PSR %016" PRIX64 "  ic=%u i=%u it=%u dt=%u rt=%u bn=%u ac=%u\n",
      m->psr,
      (unsigned)((m->psr >> 13) & 1), (unsigned)((m->psr >> 14) & 1),
      (unsigned)((m->psr >> 36) & 1), (unsigned)((m->psr >> 17) & 1),
      (unsigned)((m->psr >> 27) & 1), (unsigned)((m->psr >> 44) & 1),
      (unsigned)((m->psr >> 3) & 1));
    P("CFM sof=%u sol=%u sor=%u rrb=%u/%u/%u  bof=%u  PR %016" PRIX64 "\n",
      CFM_SOF(m->cfm), CFM_SOL(m->cfm), CFM_SOR(m->cfm),
      CFM_RRB_GR(m->cfm), CFM_RRB_FR(m->cfm), CFM_RRB_PR(m->cfm),
      m->bof, m->pr);
    for (unsigned r = 0; r < 32; r += 4) {
        P("r%-3u %016" PRIX64 " %016" PRIX64 " %016" PRIX64 " %016" PRIX64 "\n",
          r,
          merced_gr(m, r), merced_gr(m, r + 1),
          merced_gr(m, r + 2), merced_gr(m, r + 3));
    }
    unsigned sof = CFM_SOF(m->cfm);
    for (unsigned r = 32; r < 32 + sof && r < 128; r += 4) {
        P("r%-3u %016" PRIX64 " %016" PRIX64 " %016" PRIX64 " %016" PRIX64 "\n",
          r,
          merced_gr(m, r), merced_gr(m, r + 1),
          merced_gr(m, r + 2), merced_gr(m, r + 3));
    }
    P("b0  %016" PRIX64 " %016" PRIX64 " %016" PRIX64 " %016" PRIX64 "\n",
      m->br[0], m->br[1], m->br[2], m->br[3]);
    P("b4  %016" PRIX64 " %016" PRIX64 " %016" PRIX64 " %016" PRIX64 "\n",
      m->br[4], m->br[5], m->br[6], m->br[7]);
    P("pfs %016" PRIX64 "  lc %" PRIu64 "  ec %" PRIu64 "  unat %016" PRIX64 "\n",
      m->ar[AR_PFS], m->ar[AR_LC], m->ar[AR_EC], m->ar[AR_UNAT]);
    P("iva %016" PRIX64 "  iip %016" PRIX64 "  ipsr %016" PRIX64 "  isr %016" PRIX64 "\n",
      m->cr[CR_IVA], m->cr[CR_IIP], m->cr[CR_IPSR], m->cr[CR_ISR]);
    P("ifa %016" PRIX64 "  itir %016" PRIX64 "  halt: %s\n",
      m->cr[CR_IFA], m->cr[CR_ITIR], m->halt_msg);
    #undef P
}
