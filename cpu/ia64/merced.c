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

/* Debug switches are process-startup configuration.  Looking them up through
 * the CRT from instruction/MMU hot paths is especially expensive on MinGW.
 * Cache by the string-literal address: call sites use literals, so the steady
 * state is a couple of pointer operations without hashing the string. */
typedef struct MercedEnvCacheEntry {
    const char *name;
    const char *value;
} MercedEnvCacheEntry;

static const char *merced_cached_getenv(const char *name) {
    enum { ENV_CACHE_SIZE = 256 };
    static MercedEnvCacheEntry cache[ENV_CACHE_SIZE];
    uintptr_t h = ((uintptr_t)name >> 4) ^ ((uintptr_t)name >> 13);
    unsigned slot = (unsigned)h & (ENV_CACHE_SIZE - 1);

    for (unsigned probe = 0; probe < ENV_CACHE_SIZE; probe++) {
        MercedEnvCacheEntry *entry =
            &cache[(slot + probe) & (ENV_CACHE_SIZE - 1)];
        if (entry->name == name)
            return entry->value;
        if (!entry->name) {
            entry->value = getenv(name);
            entry->name = name;
            return entry->value;
        }
    }
    return getenv(name); /* Defensive fallback; current call-site count < 256. */
}

/* All getenv uses below describe immutable startup/debug configuration.
 * Keep the spelling at call sites readable while ensuring none can regress
 * into a per-instruction CRT lookup. */
#define getenv(name) merced_cached_getenv(name)

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

/* One-off register/bundle debug hooks (gr_read/gr_write/merced_step/
 * phys_write). Resolved once, in watch_init() (near the rest of this file's
 * debug env vars) - these sit on paths called billions of times, so a
 * getenv() per call would cost more than the emulation itself. */
static bool r5_debug_on, r8_debug_on, r24_debug_on, r29_debug_on;
static bool r18_debug_on, r48_debug_on, zero_loop_debug_on, bad_store_debug_on;

/* ── PSR / CFM / AR / CR layout ──────────────────────────────────────────── */

#define PSR_BE   (1ull << 1)
#define PSR_UP   (1ull << 2)
#define PSR_AC   (1ull << 3)
#define PSR_MFL  (1ull << 4)
#define PSR_MFH  (1ull << 5)
#define PSR_IC   (1ull << 13)
#define PSR_I    (1ull << 14)
#define PSR_DT   (1ull << 17)
#define PSR_PP   (1ull << 21)
#define PSR_DI   (1ull << 22)
#define PSR_RT   (1ull << 27)
#define PSR_MC   (1ull << 35)
#define PSR_IS   (1ull << 34)
#define PSR_DA   (1ull << 38)
#define PSR_DD   (1ull << 39)
#define PSR_IT   (1ull << 36)
#define PSR_ED   (1ull << 43)
#define PSR_BN   (1ull << 44)
#define PSR_IA   (1ull << 45)
#define PSR_RI_SHIFT 41

#define PSR_PK   (1ull << 15)
#define PSR_CPL_SHIFT 32

#define ISR_X (1ull << 32)
#define ISR_W (1ull << 33)
#define ISR_R (1ull << 34)
#define ISR_NA (1ull << 35)
/* Set when the reference that faulted was control-speculative.  Software
 * dispatches on this to decide between retrying with PSR.ed and running its
 * real fault handler, so a delivered ld.s fault must report it. */
#define ISR_SP (1ull << 36)
#define ISR_RS (1ull << 37) /* register-stack-engine reference */
#define ISR_ED (1ull << 43)

/* PTE fields (SDM Vol.2 4.1, "Translation Insertion Format"). */
#define PTE_PRESENT   (1ull << 0)
#define PTE_ACCESSED  (1ull << 5)
#define PTE_DIRTY     (1ull << 6)
#define PTE_MA_SHIFT  2
#define PTE_MA_NATPAGE 7
#define PTE_PL_SHIFT  7
#define PTE_AR_SHIFT  9

/* Protection key register fields. */
#define PKR_VALID (1ull << 0)
#define PKR_WD    (1ull << 1)
#define PKR_RD    (1ull << 2)
#define PKR_XD    (1ull << 3)
#define PKR_KEY_SHIFT 8

/* Access types, matching the ISR bits the callers already pass around. */
#define PERM_R 1u
#define PERM_W 2u
#define PERM_X 4u

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
    VEC_INST_KEY_MISS = 0x1800, VEC_DATA_KEY_MISS = 0x1C00,
    VEC_DIRTY = 0x2000, VEC_IACCESS = 0x2400, VEC_DACCESS = 0x2800,
    VEC_BREAK = 0x2C00, VEC_EXTINT = 0x3000,
    VEC_PAGE_NOT_PRESENT = 0x5000, VEC_KEY_PERMISSION = 0x5100,
    VEC_INST_ACCESS_RIGHTS = 0x5200, VEC_DATA_ACCESS_RIGHTS = 0x5300,
    VEC_GENERAL = 0x5400,
    VEC_NAT = 0x5600, VEC_SPEC = 0x5700, VEC_UNALIGNED = 0x5A00,
    VEC_IA32_INTERCEPT = 0x6A00,
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

static uint64_t debug_bank1_r27_nat_set_ip;
static uint64_t debug_bank1_r27_nat_set_ninsts;
static bool debug_b520_fault_active;

static unsigned stacked_phys(const Merced *m, unsigned r) {
    unsigned idx = r - 32;
    unsigned sor8 = CFM_SOR(m->cfm) * 8;
    if (sor8 && idx < sor8)
        idx = (idx + CFM_RRB_GR(m->cfm)) % sor8;
    return (m->bof + idx) % MERCED_RSE_CAPACITY;
}

static uint64_t gr_read(Merced *m, unsigned r, uint8_t *nat) {
    if (nat) *nat = 0;
    if (r == 0) return 0;
    if (r < 16) { if (nat) *nat = m->nat_static[r]; return m->gr_static[r]; }
    if (r < 32) {
        if (r == 29 && r29_debug_on &&
            (m->ip & ~UINT64_C(0xF)) >= UINT64_C(0x7FF0FD90) &&
            (m->ip & ~UINT64_C(0xF)) <= UINT64_C(0x7FF0FEF0)) {
            fprintf(stderr, "merced: R29-READ ip=%016" PRIX64
                    " bn=%u bank0=%016" PRIX64 " bank1=%016" PRIX64
                    " ninsts=%" PRIu64 "\n", m->ip, !!(m->psr & PSR_BN),
                    m->gr_bank0[13], m->gr_static[29], m->ninsts);
            fflush(stderr);
        }
        if (m->psr & PSR_BN) { if (nat) *nat = m->nat_static[r]; return m->gr_static[r]; }
        if (nat) *nat = m->nat_bank0[r - 16];
        return m->gr_bank0[r - 16];
    }
    unsigned p = stacked_phys(m, r);
    if (nat) *nat = m->nat_stack[p];
    return m->gr_stack[p];
}

static uint8_t gr_nat(Merced *m, unsigned r) {
    uint8_t nat = 0;
    (void)gr_read(m, r, &nat);
    return nat;
}

static inline void alat_set_valid(Merced *m, unsigned i, bool v) {
    m->alat[i].valid = v;
    if (v) m->alat_valid_mask |= (uint32_t)1u << i;
    else   m->alat_valid_mask &= ~((uint32_t)1u << i);
}

static void alat_invalidate_reg(Merced *m, unsigned reg) {
    uint32_t bits_left = m->alat_valid_mask;
    while (bits_left) {
        unsigned i = (unsigned)__builtin_ctz(bits_left);
        bits_left &= bits_left - 1;
        if (m->alat[i].reg == reg)
            alat_set_valid(m, i, false);
    }
}

static void alat_invalidate_stacked(Merced *m) {
    uint32_t bits_left = m->alat_valid_mask;
    while (bits_left) {
        unsigned i = (unsigned)__builtin_ctz(bits_left);
        bits_left &= bits_left - 1;
        if (m->alat[i].reg >= 32)
            alat_set_valid(m, i, false);
    }
}

static void alat_invalidate_store(Merced *m, uint64_t pa, unsigned size) {
    uint64_t end = pa + size;
    uint32_t bits_left = m->alat_valid_mask;
    while (bits_left) {
        unsigned i = (unsigned)__builtin_ctz(bits_left);
        bits_left &= bits_left - 1;
        uint64_t aend = m->alat[i].phys_addr + m->alat[i].size;
        if (m->alat[i].phys_addr < end && pa < aend)
            alat_set_valid(m, i, false);
    }
}

static void alat_set(Merced *m, unsigned reg, uint64_t pa, unsigned size) {
    alat_invalidate_reg(m, reg);
    for (unsigned i = 0; i < 32; i++) {
        if (!m->alat[i].valid) {
            m->alat[i].phys_addr = pa;
            m->alat[i].size = (uint8_t)size;
            m->alat[i].reg = (uint8_t)reg;
            alat_set_valid(m, i, true);
            return;
        }
    }
}

static bool alat_check(Merced *m, unsigned reg, uint64_t pa, unsigned size,
                       bool clear) {
    uint32_t bits_left = m->alat_valid_mask;
    while (bits_left) {
        unsigned i = (unsigned)__builtin_ctz(bits_left);
        bits_left &= bits_left - 1;
        if (m->alat[i].reg != reg)
            continue;
        bool hit = m->alat[i].phys_addr == pa && m->alat[i].size == size;
        if (clear || !hit)
            alat_set_valid(m, i, false);
        return hit;
    }
    return false;
}

static bool alat_check_reg(Merced *m, unsigned reg, bool clear) {
    uint32_t bits_left = m->alat_valid_mask;
    while (bits_left) {
        unsigned i = (unsigned)__builtin_ctz(bits_left);
        bits_left &= bits_left - 1;
        if (m->alat[i].reg != reg)
            continue;
        if (clear)
            alat_set_valid(m, i, false);
        return true;
    }
    return false;
}

static void gr_write(Merced *m, unsigned r, uint64_t v, uint8_t nat) {
    if (r == 0) return;   /* writes to r0 fault on HW; ignore here */
    if (r == 24 && r24_debug_on &&
        (m->ip & ~UINT64_C(0xF)) >= UINT64_C(0x7FF0DF60) &&
        (m->ip & ~UINT64_C(0xF)) <= UINT64_C(0x7FF0E120)) {
        fprintf(stderr, "merced: R24-WRITE ip=%016" PRIX64 " value=%016"
                PRIX64 " nat=%u ninsts=%" PRIu64 "\n",
                m->ip, v, !!nat, m->ninsts);
        fflush(stderr);
    }
    if (r == 8 && r8_debug_on &&
        (m->ip & ~UINT64_C(0xF)) >= UINT64_C(0x7FE30000) &&
        (m->ip & ~UINT64_C(0xF)) <= UINT64_C(0x7FE40000)) {
        fprintf(stderr, "merced: R8-WRITE ip=%016" PRIX64 " value=%016"
                PRIX64 " nat=%u ninsts=%" PRIu64 "\n",
                m->ip, v, !!nat, m->ninsts);
        fflush(stderr);
    }
    if (r == 5 && r5_debug_on) {
        fprintf(stderr, "merced: R5-WRITE ip=%016" PRIX64 " value=%016"
                PRIX64 " nat=%u ninsts=%" PRIu64 "\n",
                m->ip, v, !!nat, m->ninsts);
        fflush(stderr);
    }
    if (r == 29 && r29_debug_on &&
        (m->ip & ~UINT64_C(0xF)) >= UINT64_C(0x7FE3EB00) &&
        (m->ip & ~UINT64_C(0xF)) <= UINT64_C(0x7FF0FEF0)) {
        fprintf(stderr, "merced: R29-WRITE ip=%016" PRIX64 " value=%016"
                PRIX64 " nat=%u bn=%u ninsts=%" PRIu64 "\n",
                m->ip, v, !!nat, !!(m->psr & PSR_BN), m->ninsts);
        fflush(stderr);
    }
    if (r == 19 && debug_b520_fault_active &&
        getenv("MERCED_DEBUG_R19_NAT")) {
        static unsigned n;
        if (n++ < 256)
            fprintf(stderr, "merced: R19-WRITE ip=%016" PRIX64
                    " value=%016" PRIX64 " nat=%u bn=%u cfm=%016" PRIX64
                    " ninsts=%" PRIu64 "\n", m->ip, v, !!nat,
                    !!(m->psr & PSR_BN), m->cfm, m->ninsts);
    }
    if (r == 8 && v == UINT64_C(0xE000010600000000) &&
        getenv("MERCED_DEBUG_ALLOC_BAD")) {
        static unsigned alloc_bad_count;
        if (alloc_bad_count++ < 8) {
            fprintf(stderr, "merced: ALLOC-BAD-WRITE ip=%016" PRIX64
                    " ninsts=%" PRIu64 " pr=%016" PRIX64
                    " cfm=%016" PRIX64 " bof=%u b0=%016" PRIX64 "\n",
                    m->ip, m->ninsts, m->pr, m->cfm, m->bof, m->br[0]);
            for (unsigned q = 8; q < 64; q += 4)
                fprintf(stderr, "merced: ABR r%-2u=%016" PRIX64
                        " r%-2u=%016" PRIX64 " r%-2u=%016" PRIX64
                        " r%-2u=%016" PRIX64 "\n",
                        q, gr_read(m, q, NULL),
                        q + 1, gr_read(m, q + 1, NULL),
                        q + 2, gr_read(m, q + 2, NULL),
                        q + 3, gr_read(m, q + 3, NULL));
            merced_dump_trace(m, MERCED_TRACE_HISTORY, stderr);
            fflush(stderr);
        }
    }
    if (v == UINT64_C(0xE000010600000000) &&
        (m->ip >> 60) == UINT64_C(0xE) &&
        getenv("MERCED_DEBUG_ORIGIN")) {
        static unsigned count;
        if (count++ < 20)
            fprintf(stderr, "merced: ORIGIN r%u=E000010600000000 ip=%016" PRIX64
                    " ninsts=%" PRIu64 " b0=%016" PRIX64 "\n",
                    r, m->ip, m->ninsts, m->br[0]);
    }
    /* Ordinary GR writes do not invalidate an advanced-load entry for that
     * register.  The ALAT tracks memory conflicts, not the current GR value:
     * ld.c.nc must preserve an intervening computed value when no conflicting
     * store occurred. */
    if (r < 16) { m->gr_static[r] = v; m->nat_static[r] = nat; return; }
    if (r < 32) {
        if (m->psr & PSR_BN) {
            if (r == 27 && nat) {
                debug_bank1_r27_nat_set_ip = m->ip;
                debug_bank1_r27_nat_set_ninsts = m->ninsts;
            }
            m->gr_static[r] = v; m->nat_static[r] = nat;
        }
        else {
            m->gr_bank0[r - 16] = v; m->nat_bank0[r - 16] = nat;
        }
        return;
    }
    unsigned p = stacked_phys(m, r);
    /* A returned frame may reuse a register position that was spilled by an
     * older frame.  Writing that position makes it dirty again; leaving the
     * flush boundary above it causes a later context switch to reload the
     * older backing-store value (NT exposed this as MiCreateMemoryEvent's
     * saved r36 changing from a kernel pointer to 0x6742). */
    unsigned logical = r - 32;
    unsigned sor8 = CFM_SOR(m->cfm) * 8;
    if (sor8 && logical < sor8)
        logical = (logical + CFM_RRB_GR(m->cfm)) % sor8;
    int64_t pos = (int64_t)m->bof_total + logical;
    if (pos < m->rse_flushed_regs)
        m->rse_flushed_regs = pos;
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

static uint64_t rsc_value_for_write(const Merced *m, uint64_t value) {
    unsigned requested_pl = (unsigned)((value >> 2) & 3);
    unsigned cpl = (unsigned)((m->psr >> 32) & 3);

    /* RSC.pl cannot grant the RSE more privilege than the current CPL. */
    if (requested_pl < cpl)
        value = (value & ~(UINT64_C(3) << 2)) | ((uint64_t)cpl << 2);
    return value;
}
static void pr_write(Merced *m, unsigned i, int v) {
    if (i == 0) return;
    unsigned p = pr_phys(m, i);
    if ((i == 1 || i == 14) &&
        m->ip >= UINT64_C(0xE00000008355B000) &&
        m->ip < UINT64_C(0xE00000008355B600) &&
        getenv("MERCED_DEBUG_B520_PREDS")) {
        static unsigned pred_n;
        if (pred_n++ < 512)
            fprintf(stderr, "merced: B520-PRED ip=%016" PRIX64
                    " p%u=%d old=%d raw-pr=%016" PRIX64
                    " r28=%016" PRIX64 "/nat%u ninsts=%" PRIu64 "\n",
                    m->ip, i, !!v,
                    !!(m->pr & (UINT64_C(1) << p)), m->pr,
                    gr_read(m, 28, NULL), gr_nat(m, 28), m->ninsts);
    }
    if (v && i >= 44 && !(m->pr & (UINT64_C(1) << p)) &&
        getenv("MERCED_DEBUG_HIGH_PR")) {
        static unsigned n;
        if (n++ < 256)
            fprintf(stderr, "merced: HIGH-PR ip=%016" PRIX64
                    " logical=p%u physical=p%u raw-pr=%016" PRIX64
                    " cfm=%016" PRIX64 " ninsts=%" PRIu64 "\n",
                    m->ip, i, p, m->pr, m->cfm, m->ninsts);
    }
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
    if (!isfinite(d)) {
        /* This model has no Inf/NaN register encoding (see fp2d), and the
         * normalize loop below never terminates for +Inf (d*=0.5 stays
         * Inf). Clamp to the largest finite magnitude instead of hanging -
         * consistent with the rest of this conversion being an approximate,
         * not IEEE-corner-exact, model. */
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "merced: note: FP result not finite (Inf/NaN), "
                            "clamping instead of hanging\n");
        }
        f.sig = UINT64_MAX;
        f.exp = 0x1FFFE;
        return f;
    }
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
 * fp_result_static() is used for FMA-family results.  frcpa must return the
 * architected table estimate, not a full-precision reciprocal: compiler
 * Newton-Raphson sequences deliberately refine the low-precision estimate
 * and can produce a non-converging quotient if handed an exact reciprocal. */
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
static uint64_t rfi_generation;
/* Set when the interruption currently in service was delivered via
 * VEC_EXTINT, cleared at the matching rfi (see deliver_fault() and the rfi
 * case in exec_b()). IA-64 doesn't stack interruption context - by the time
 * a guest reaches rfi it can only be returning from the single most recent
 * delivery - so a flag, not a counter, is enough to identify which rfi that
 * was. Without this, rfi_generation counted every rfi system-wide,
 * including the ones ending completely unrelated TLB-miss/fault handlers
 * that fire far more often than any interrupt; platform code waiting on it
 * to mean "the external ISR returned" was actually satisfied by the next
 * unrelated fault instead, in practice almost immediately. */
static bool ext_interrupt_in_service;

uint64_t merced_rfi_generation(void) { return rfi_generation; }

static const MercedTlbEntry *tlb_search(const MercedTlbEntry *t, int n,
                                        uint32_t rid, uint64_t va,
                                        uint32_t *hint) {
    const uint64_t payload_mask = (UINT64_C(1) << 61) - 1;
    uint64_t payload = va & payload_mask;

    if (*hint < (uint32_t)n) {
        const MercedTlbEntry *e = &t[*hint];
        uint64_t start = e->va_start & payload_mask;
        uint64_t end = e->va_end & payload_mask;
        if (e->valid && e->rid == rid && payload >= start && payload <= end)
            return e;
    }

    for (int i = 0; i < n; i++) {
        uint64_t start = t[i].va_start & payload_mask;
        uint64_t end = t[i].va_end & payload_mask;

        /* Region number selects the RR (and therefore the RID), but it is
         * not part of a TR/TC entry's VPN comparison.  Equal RID and payload
         * must match even when the reference uses another VRN. */
        if (t[i].valid && t[i].rid == rid &&
            payload >= start && payload <= end) {
            *hint = (uint32_t)i;
            return &t[i];
        }
    }
    return NULL;
}

/* Returns true and sets *pa on success; on failure delivers a fault (or
 * halts) and returns false with *st set. If force is true, always performs
 * the real region/TR/TC/VHPT lookup even when the corresponding psr.it/dt
 * bit is off - used by tpa, which is an explicit "what would this VA
 * translate to" query and must answer correctly regardless of whether
 * translation happens to be enabled for ordinary references right now
 * (real hardware's tpa doesn't take the physical-mode passthrough that
 * ld/st do). Ordinary references go through va_translate(), which is just
 * this with force=false. */
static bool va_translate_ex(Merced *m, uint64_t va, bool ifetch, bool spec,
                            uint64_t isr_access, bool force,
                            uint64_t *pa, MercedStatus *st);
static bool va_translate(Merced *m, uint64_t va, bool ifetch, bool spec,
                         uint64_t isr_access,
                         uint64_t *pa, MercedStatus *st);
/* MERCED_MMFAULT_LO/HI: once a fault has been delivered for the MERCED_WATCH_VA
 * range, log every data reference made while executing inside [lo,hi).  Aimed
 * at a guest fault handler, to discover which address it treats as the PTE. */
static uint64_t mmfault_lo, mmfault_hi;
static int mmfault_armed;
static uint64_t watch_pa_base, watch_pa_end;
static uint64_t watch_va_base, watch_va_end;
static bool target_trap_slot_armed;
static unsigned target_trap_slot_writes;
static uint64_t target_vhpt_entry_pa;
static unsigned target_vhpt_entry_writes;
static uint64_t phys_read(Merced *m, uint64_t pa, unsigned size);
static void tlb_insert(Merced *m, MercedTlbEntry *e, uint64_t pte,
                       bool instruction);
static bool firmware_identity_pa(Merced *m, uint64_t va, uint64_t *pa);
static void tlb_serialize_data(Merced *m);
static void tlb_serialize_instruction(Merced *m);

/* Hardware-style VHPT walk, tried on a TLB miss before falling back to the
 * software fault path - see the definition (after tlb_insert) for why this
 * is safe even if the hash math is subtly wrong. */
enum {
    VHPT_MISS = 0,
    VHPT_HIT = 1,
    VHPT_NOT_PRESENT = -1,
    VHPT_TRANSLATION = -2,
};
static int vhpt_walk(Merced *m, uint64_t va, bool ifetch);

/* The VHPT hash address for va - shared by vhpt_walk() (to locate the
 * collision-chain entry) and deliver_fault() (to populate cr.iha on a
 * miss). Matching reference/qemu-system-ia64's ia64_vhpt_hash_address():
 * both call sites use the exact same formula, so GEMU's own VHPT walker
 * and the value the OS's software walker is told to repair can never
 * diverge. Returns va unchanged if the walker is disabled (pta.ve=0 or
 * rr.ve=0), matching the reference's "config invalid" fallback. */
static uint64_t vhpt_hash_address(Merced *m, uint64_t va);

/* Advance ar.itc and edge-latch the interval-timer interrupt if it just
 * crossed cr.itm. cr.itm == 0 means "never armed" (SAL/EFI haven't
 * programmed a deadline yet), matching real reset state where the timer
 * isn't running until software sets it. */
static void itc_advance(Merced *m, uint64_t delta) {
    uint64_t old = m->ar[AR_ITC];
    m->ar[AR_ITC] += delta;
    uint64_t deadline = m->cr[CR_ITM];
    /* cr.itm is a one-shot match, not a level-triggered "ITC is beyond
     * ITM" condition.  Once cr.ivr acknowledges the interrupt it must stay
     * quiet until software programs and crosses a new deadline.  The old
     * level comparison reasserted the timer after every instruction and
     * trapped NT in its external-interrupt handler. */
    if (deadline != 0 &&
        (int64_t)(old - deadline) < 0 &&
        (int64_t)(m->ar[AR_ITC] - deadline) >= 0)
        m->timer_pending = 1;
}

void merced_set_external_itc(Merced *m, bool enabled) {
    m->external_itc = enabled;
}

void merced_advance_itc(Merced *m, uint64_t ticks) {
    itc_advance(m, ticks);
}

uint64_t merced_get_itc(const Merced *m) {
    return m->ar[AR_ITC];
}

static void ext_pending_set(Merced *m, uint8_t vector) {
    uint8_t bit = (uint8_t)(1u << (vector & 7));
    if (!(m->external_pending[vector >> 3] & bit))
        m->ext_pending_count++;
    m->external_pending[vector >> 3] |= bit;
}

static void ext_pending_clear(Merced *m, uint8_t vector) {
    uint8_t bit = (uint8_t)(1u << (vector & 7));
    if (m->external_pending[vector >> 3] & bit)
        m->ext_pending_count--;
    m->external_pending[vector >> 3] &= (uint8_t)~bit;
}

void merced_ack_external(Merced *m, uint8_t vector) {
    ext_pending_clear(m, vector);
}

static bool ext_pending_test(const Merced *m, uint8_t vector) {
    return (m->external_pending[vector >> 3] >> (vector & 7)) & 1;
}

void merced_raise_external(Merced *m, uint8_t vector) {
    ext_pending_set(m, vector);
}

void merced_set_tpr(Merced *m, uint64_t tpr) {
    m->cr[CR_TPR] = tpr;
}

/* An external interrupt is eligible only when its priority class exceeds
 * cr.tpr.mic.  NT represents IRQL in the high nibble of the vector and
 * writes that value directly to CR.TPR; ignoring it permits a clock vector
 * to re-enter its own queued-spinlock path and deadlock the sole processor.
 * CR.TPR.mmi (bit 16) masks all maskable interrupts.
 *
 * Vectors 0 and 2 are architecturally special (SDM Vol.2 5.6/5.7, confirmed
 * against reference/qemu-system-ia64-merced's sapic_vector_unmasked()):
 * vector 2 is the local SAPIC's NMI delivery mode - never masked, not even
 * by TPR.mmi. Vector 0 is ExtINT delivery mode - the legacy-8259-compatible
 * path a real chipset uses to signal the CPU when a classic PIC IRQ fires,
 * gated only by TPR.mmi, NOT by TPR.mic's priority class (real legacy PC
 * interrupts have no notion of IA-64 IRQL). Getting this wrong is exactly
 * what caused the i2000 SDV BIOS to hang forever: it raises tpr to 0xC0
 * around ATA identify and never lowers it again, which permanently masks
 * any normal (>=16) vector but must NOT mask ExtINT. */
static bool interrupt_unmasked(Merced *m, uint8_t vector) {
    uint64_t tpr = m->cr[CR_TPR];
    if (vector == 2)
        return true;
    if (vector == 0)
        return !(tpr & (UINT64_C(1) << 16));
    if (tpr & (UINT64_C(1) << 16))
        return false;
    return (vector & 0xF0u) > (tpr & 0xF0u);
}

/* The highest-priority pending external vector that also clears the
 * current cr.tpr, or -1.  Vectors 1 and 3-15 are architecturally reserved
 * (fault/trap vector space) and never legal external-interrupt vectors;
 * 0 (ExtINT) and 2 (NMI) are legal but outrank every normal (>=16) vector -
 * they're delivery *modes*, not priority-classed platform interrupts, so
 * they're checked first regardless of numeric value. Real I/O SAPIC
 * hardware resolves cr.ivr the same way: independently-latched vectors,
 * highest priority wins, lower ones stay pending underneath. */
static int ext_highest_unmasked(Merced *m) {
    if (!m->ext_pending_count)
        return -1;
    if (ext_pending_test(m, 2) && interrupt_unmasked(m, 2))
        return 2;
    if (ext_pending_test(m, 0) && interrupt_unmasked(m, 0))
        return 0;
    for (int v = 255; v >= 16; v--) {
        if (ext_pending_test(m, (uint8_t)v) && interrupt_unmasked(m, (uint8_t)v))
            return v;
    }
    return -1;
}

static uint64_t fault_vector_counts[0x5B];

void merced_fault_stats(uint64_t *counts, size_t count) {
    if (count > sizeof(fault_vector_counts) / sizeof(fault_vector_counts[0]))
        count = sizeof(fault_vector_counts) / sizeof(fault_vector_counts[0]);
    memcpy(counts, fault_vector_counts, count * sizeof(*counts));
}

/* Read guest virtual memory for a debug hook.  Translation is speculative so
 * a non-resident page truncates the read instead of faulting the guest. */
static bool dbg_read(Merced *m, uint64_t va, void *dst, size_t n) {
    uint8_t *d = dst;
    for (size_t i = 0; i < n; i++) {
        uint64_t pa;
        MercedStatus st;
        if (!va_translate(m, va + i, false, true, 0, &pa, &st))
            return false;
        d[i] = (uint8_t)phys_read(m, pa, 1);
    }
    return true;
}

static uint32_t dbg_u32(Merced *m, uint64_t va) {
    uint32_t v = 0;
    dbg_read(m, va, &v, sizeof(v));
    return v;
}

/* Name the loaded PE image a guest address falls in, and the exported symbol
 * it sits closest behind.  Windows/IA-64 has no symbols to hand here, so
 * walking back to the image's own MZ/PE headers and reading its export
 * directory is the only way to turn a bare kernel address into something
 * recognisable ("ntoskrnl.exe+0x1AF8F0, after KeSetEvent") without attaching
 * a kernel debugger. */
static void nt_identify(Merced *m, uint64_t va, const char *what,
                        bool follow_descriptor) {
    uint64_t base;
    unsigned page;

    for (page = 0, base = va & ~UINT64_C(0xFFF); page < 8192;
         page++, base -= 0x1000) {
        uint16_t mz = 0;
        uint32_t lfanew, exp_rva, exp_size, nfuncs, nnames;
        uint64_t exp, opt;
        char name[64] = "?";
        uint64_t best = 0;
        uint32_t best_idx = 0;
        uint32_t i;

        if (!dbg_read(m, base, &mz, sizeof(mz)) || mz != 0x5A4D)
            continue;
        lfanew = dbg_u32(m, base + 0x3C);
        if (lfanew < 0x40 || lfanew > 0x800) continue;
        if (dbg_u32(m, base + lfanew) != 0x00004550) continue;   /* "PE\0\0" */

        /* PE32+ optional header: the data directories start at +0x70, and
         * entry 0 is the export table. */
        opt = base + lfanew + 24;
        exp_rva = dbg_u32(m, opt + 0x70);
        exp_size = dbg_u32(m, opt + 0x74);
        if (!exp_rva || !exp_size) {
            fprintf(stderr, "merced: %s %016" PRIX64 " = <image at %016"
                    PRIX64 ">+%#" PRIx64 " (no exports)\n",
                    what, va, base, va - base);
            return;
        }
        exp = base + exp_rva;
        dbg_read(m, base + dbg_u32(m, exp + 0x0C), name, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        nfuncs = dbg_u32(m, exp + 0x14);
        nnames = dbg_u32(m, exp + 0x18);
        if (nfuncs > 8192) nfuncs = 8192;
        if (nnames > 8192) nnames = 8192;

        /* An IA-64 *code* export points at a function descriptor (entry, gp),
         * so it needs one indirection to reach the code the name refers to.
         * A *data* export is already the address itself - following it there
         * would dereference the variable's contents. */
        for (i = 0; i < nfuncs; i++) {
            uint64_t target = base + dbg_u32(m, base + dbg_u32(m, exp + 0x1C) +
                                                i * 4);
            if (follow_descriptor && !dbg_read(m, target, &target,
                                               sizeof(target)))
                continue;
            if (target <= va && target > best) { best = target; best_idx = i; }
        }

        fprintf(stderr, "merced: %s %016" PRIX64 " = %s+%#" PRIx64,
                what, va, name, va - base);
        if (best) {
            uint64_t names = base + dbg_u32(m, exp + 0x20);
            uint64_t ords = base + dbg_u32(m, exp + 0x24);
            char sym[96] = "";
            for (i = 0; i < nnames; i++) {
                uint16_t ord = 0;
                dbg_read(m, ords + i * 2, &ord, sizeof(ord));
                if (ord != best_idx) continue;
                dbg_read(m, base + dbg_u32(m, names + i * 4), sym,
                         sizeof(sym) - 1);
                sym[sizeof(sym) - 1] = 0;
                break;
            }
            fprintf(stderr, "  (after %s+%#" PRIx64 ")",
                    sym[0] ? sym : "<ordinal-only>", va - best);
        }
        fprintf(stderr, "\n");
        return;
    }
    fprintf(stderr, "merced: %s %016" PRIX64 " = <no PE image found>\n",
            what, va);
}

/* Windows/IA-64's DebugPrint is a break 0x80014 with the message buffer's
 * virtual address in GR2 and its length in GR3 (NT rtl/ia64 debugstb.s).
 * Echoing it turns an otherwise anonymous kernel stall into a readable
 * error record - it is how NT reports things like a failed ZwOpenKey or a
 * HAL initialization refusal, which no fault trace would otherwise show. */
static void nt_debugprint(Merced *m) {
    uint32_t len = (uint32_t)(gr_read(m, 3, NULL) & 0xFFFF);
    uint64_t va = gr_read(m, 2, NULL);
    char buf[512];
    uint32_t got = 0;

    if (len > sizeof(buf) - 1) len = sizeof(buf) - 1;
    while (got < len) {
        uint64_t pa;
        MercedStatus st;
        /* Speculative translation: a buffer page that is not resident must
         * truncate the message, never fault the guest from a debug hook. */
        if (!va_translate(m, va + got, false, true, 0, &pa, &st))
            break;
        buf[got++] = (char)phys_read(m, pa, 1);
    }
    while (got > 0 && (buf[got - 1] == '\n' || buf[got - 1] == '\r'))
        got--;
    if (got == 0) return;
    buf[got] = 0;
    fprintf(stderr, "merced: NT-DBGPRINT %s\n", buf);
    fflush(stderr);
}

static bool rse_preserve_interrupted_partition(Merced *m, uint64_t iip,
                                                uint64_t ipsr) {
    if (m->interruption_rse_depth >=
        sizeof(m->interruption_rse) / sizeof(m->interruption_rse[0]))
        return false;
    typeof(m->interruption_rse[0]) *s =
        &m->interruption_rse[m->interruption_rse_depth++];
    s->iip = iip;
    s->ipsr = ipsr;
    memcpy(s->gr_stack, m->gr_stack, sizeof(s->gr_stack));
    memcpy(s->nat_stack, m->nat_stack, sizeof(s->nat_stack));
    s->bof = m->bof;
    s->bof_total = m->bof_total;
    s->anchor_addr = m->rse_anchor_addr;
    s->anchor_regs = m->rse_anchor_regs;
    s->flushed_regs = m->rse_flushed_regs;
    return true;
}

static bool rse_restore_interrupted_partition(Merced *m, uint64_t iip,
                                               uint64_t ipsr) {
    if (m->interruption_rse_depth == 0)
        return false;
    typeof(m->interruption_rse[0]) *s =
        &m->interruption_rse[m->interruption_rse_depth - 1];
    /* A deliberately rewritten IIP/IPSR is a context switch, not a return
     * to the interrupted partition.  Drop the stale implementation record
     * without overwriting the target context's RSE resources. */
    m->interruption_rse_depth--;
    if (s->iip != iip || s->ipsr != ipsr)
        return false;
    memcpy(m->gr_stack, s->gr_stack, sizeof(s->gr_stack));
    memcpy(m->nat_stack, s->nat_stack, sizeof(s->nat_stack));
    m->bof = s->bof;
    m->bof_total = s->bof_total;
    m->rse_anchor_addr = s->anchor_addr;
    m->rse_anchor_regs = s->anchor_regs;
    m->rse_flushed_regs = s->flushed_regs;
    return true;
}

static MercedStatus deliver_fault(Merced *m, uint32_t vec, uint64_t isr,
                                  uint64_t ifa, bool set_ifa) {
    m->nfaults++;
    /* Interruption delivery is both an instruction- and data-serialization
     * event (SDM Vol. 2, 3.1.4).  In particular, a ptc/ptr issued by a page
     * fault handler must have completed before any nested handler lookup.
     * Leaving the invalidation pending let NT immediately re-hit a cached
     * P=0 VHPT entry and fault forever at "Setup is starting Windows". */
    tlb_serialize_data(m);
    tlb_serialize_instruction(m);
    /* XP/IA-64 currently reaches KeBugCheckEx with
     * STATUS_REG_NAT_CONSUMPTION after executing the bundle at 8355b4e0.
     * Preserve the pre-interruption register/NaT state here: once control
     * reaches the bugcheck path the consumed operand is no longer visible.
     * This is intentionally opt-in because NaT faults are otherwise normal
     * while exercising the architecture conformance suite. */
    if (vec == VEC_NAT && getenv("MERCED_DEBUG_NAT_CRASH") &&
        (m->ip >> 61) == 7) {
        static bool dumped;
        if (!dumped) {
            dumped = true;
            fprintf(stderr, "merced: NAT-CRASH ip=%016" PRIX64
                    " slot=%u isr=%016" PRIX64 " ifa=%016" PRIX64
                    " set_ifa=%u psr=%016" PRIX64 " ninsts=%" PRIu64 "\n",
                    m->ip, (unsigned)(m->ip & 3), isr, ifa, set_ifa,
                    m->psr, m->ninsts);
            fprintf(stderr, "merced: NAT-ORIGIN bank1-r27 set-ip=%016" PRIX64
                    " set-ninsts=%" PRIu64 " age=%" PRIu64 "\n",
                    debug_bank1_r27_nat_set_ip,
                    debug_bank1_r27_nat_set_ninsts,
                    m->ninsts - debug_bank1_r27_nat_set_ninsts);
            for (unsigned base = 0; base < 128; base += 4) {
                fprintf(stderr, "merced: NAT-GR");
                for (unsigned r = base; r < base + 4; r++) {
                    uint8_t nat = 0;
                    uint64_t value = gr_read(m, r, &nat);
                    fprintf(stderr, " r%u=%016" PRIX64 "/nat%u",
                            r, value, !!nat);
                }
                fputc('\n', stderr);
            }
            if (getenv("MERCED_DEBUG_NAT_CRASH_FULL")) {
                fprintf(stderr, "merced: NAT-CRASH preceding instructions\n");
                merced_dump_trace(m, 128, stderr);
            }
            fflush(stderr);
        }
    }
    if (getenv("MERCED_DEBUG_FAULTHIST")) {
        static uint64_t vec_counts[32];
        static uint64_t site_ip[64], site_vec[64];
        static uint32_t site_counts[64];
        static unsigned site_nseen;
        static uint64_t total;
        total++;
        unsigned vslot = (vec >> 8) & 31;
        vec_counts[vslot]++;
        uint64_t site_key_ip = m->ip & ~UINT64_C(0xF);
        unsigned s;
        for (s = 0; s < site_nseen; s++) {
            if (site_ip[s] == site_key_ip && site_vec[s] == vec) {
                site_counts[s]++;
                break;
            }
        }
        if (s == site_nseen && site_nseen < 64) {
            site_ip[site_nseen] = site_key_ip;
            site_vec[site_nseen] = vec;
            site_counts[site_nseen] = 1;
            site_nseen++;
        }
        if (total % 500000 == 0) {
            fprintf(stderr, "merced: FAULTHIST total=%" PRIu64
                    " ninsts=%" PRIu64 "\n", total, m->ninsts);
            for (unsigned v = 0; v < 32; v++)
                if (vec_counts[v])
                    fprintf(stderr, "  vec=%04X count=%" PRIu64 "\n",
                            v << 8, vec_counts[v]);
            /* Top 15 fault sites by count, simple selection sort on a copy. */
            unsigned idx[64];
            for (unsigned k = 0; k < site_nseen; k++) idx[k] = k;
            for (unsigned a = 0; a < site_nseen && a < 15; a++) {
                unsigned best = a;
                for (unsigned b = a + 1; b < site_nseen; b++)
                    if (site_counts[idx[b]] > site_counts[idx[best]])
                        best = b;
                unsigned t = idx[a]; idx[a] = idx[best]; idx[best] = t;
                fprintf(stderr, "  site ip=%016" PRIX64 " vec=%04X count=%u\n",
                        site_ip[idx[a]], (unsigned)site_vec[idx[a]],
                        site_counts[idx[a]]);
            }
            fflush(stderr);
        }
    }
    /*
     * Interruption collection is a translation serialization event.  A
     * purge issued before the interruption must therefore be complete when
     * its handler makes a memory reference; otherwise the handler can reuse
     * the translation that ptr/ptc just retired.
     */
    tlb_serialize_data(m);
    tlb_serialize_instruction(m);
    if (getenv("MERCED_DEBUG_ALLFAULTS")) {
        static unsigned n;
        if (n++ < 40)
            fprintf(stderr, "merced: fault #%u vec=%04X ip=%016" PRIX64
                    " psr_ic=%d ninsts=%" PRIu64 "\n", n, vec, m->ip,
                    !!(m->psr & PSR_IC), m->ninsts);
    }
    if (set_ifa && ifa >= UINT64_C(0xE000010600000000) &&
        ifa < UINT64_C(0xE000010600010000) &&
        getenv("MERCED_DEBUG_TARGET_FAULT")) {
        static unsigned target_fault_count;
        if (target_fault_count++ < 64) {
            fprintf(stderr, "merced: TARGET-FAULT vec=%04X ip=%016" PRIX64
                    " ifa=%016" PRIX64 " isr=%016" PRIX64
                    " psr=%016" PRIX64 " iha=%016" PRIX64
                    " r16=%016" PRIX64 " r26=%016" PRIX64
                    " r30=%016" PRIX64 " ninsts=%" PRIu64 "\n",
                    vec, m->ip, ifa, isr, m->psr, m->cr[CR_IHA],
                    gr_read(m, 16, NULL), gr_read(m, 26, NULL),
                    gr_read(m, 30, NULL), m->ninsts);
            fflush(stderr);
        }
    }
    if (set_ifa && getenv("MERCED_DEBUG_B520_FAULT") &&
        (m->ip & ~UINT64_C(0xF)) == UINT64_C(0xE00000008355B520)) {
        debug_b520_fault_active = true;
        static unsigned b520_faults;
        if (b520_faults++ == 0) {
            fprintf(stderr, "merced: B520-FAULT vec=%04X ip=%016" PRIX64
                    " ifa=%016" PRIX64 " isr=%016" PRIX64
                    " psr=%016" PRIX64 " ninsts=%" PRIu64 "\n",
                    vec, m->ip, ifa, isr, m->psr, m->ninsts);
            for (unsigned r = 0; r < 64; r += 4)
                fprintf(stderr, "merced: B520-GR r%u=%016" PRIX64 "/%u"
                        " r%u=%016" PRIX64 "/%u r%u=%016" PRIX64 "/%u"
                        " r%u=%016" PRIX64 "/%u\n",
                        r, gr_read(m, r, NULL), gr_nat(m, r),
                        r + 1, gr_read(m, r + 1, NULL), gr_nat(m, r + 1),
                        r + 2, gr_read(m, r + 2, NULL), gr_nat(m, r + 2),
                        r + 3, gr_read(m, r + 3, NULL), gr_nat(m, r + 3));
            merced_dump_trace(m, 96, stderr);
            fflush(stderr);
        }
    }
    if (vec == VEC_BREAK && getenv("MERCED_DEBUG_BREAKTRACE")) {
        static uint64_t seen_iip[64], seen_iim[64], seen_count[64];
        static unsigned nseen;
        uint64_t iip = m->ip & ~UINT64_C(0xF);
        uint64_t iim = m->cr[CR_IIM];
        unsigned i;
        for (i = 0; i < nseen; i++)
            if (seen_iip[i] == iip && seen_iim[i] == iim) break;
        if (i == nseen && nseen < 64) {
            seen_iip[nseen] = iip;
            seen_iim[nseen] = iim;
            seen_count[nseen] = 0;
            nseen++;
            fprintf(stderr, "GEMU-FAULT-NEW excp=BREAK vector=0x2c00 iip=0x%016"
                    PRIX64 " iim=0x%016" PRIX64 " isr=0x%016" PRIX64
                    " ninsts=%" PRIu64 "\n", iip, iim, isr, m->ninsts);
            fflush(stderr);
        }
        if (i < nseen) seen_count[i]++;
    }
    if (vec == VEC_BREAK &&
        (m->ip & ~UINT64_C(0xF)) == UINT64_C(0xE0000000830245B0) &&
        gr_read(m, 2, NULL) == 4 && getenv("MERCED_DEBUG_WAIT4_HIST")) {
        static unsigned hit_n;
        unsigned h_this = hit_n++;
        if (h_this == 0) {
            fprintf(stderr, "merced: WAIT4-CALLCHAIN hit=%u ninsts=%" PRIu64 "\n",
                    h_this, m->ninsts);
            unsigned avail = m->call_history_next < MERCED_CALL_HISTORY
                           ? m->call_history_next : MERCED_CALL_HISTORY;
            for (unsigned i = m->call_history_next - avail;
                 i < m->call_history_next; i++) {
                unsigned h = i % MERCED_CALL_HISTORY;
                fprintf(stderr, "merced:   %s %016" PRIX64 " -> %016" PRIX64 "\n",
                        m->call_history[h].is_return ? "ret " : "call",
                        m->call_history[h].from & ~UINT64_C(0xF),
                        m->call_history[h].to & ~UINT64_C(0xF));
            }
            fflush(stderr);
        } else if (h_this < 3 || (h_this % 1000) == 0) {
            fprintf(stderr, "merced: WAIT4-CALLCHAIN hit=%u ninsts=%" PRIu64
                    " (history omitted)\n", h_this, m->ninsts);
        }
    }
    if (vec == VEC_EXTINT && getenv("MERCED_DEBUG_EXTINT")) {
        static uint64_t last_ninsts;
        static unsigned count;
        if (count++ < 4000)
            fprintf(stderr, "merced: EXTINT ip=%016" PRIX64
                    " ninsts=%" PRIu64 " delta=%" PRIu64
                    " itc=%016" PRIX64 " itm=%016" PRIX64 " itv=%016" PRIX64 "\n",
                    m->ip, m->ninsts, m->ninsts - last_ninsts,
                    m->ar[AR_ITC], m->cr[CR_ITM], m->cr[CR_ITV]);
        last_ninsts = m->ninsts;
    }
    /* Arm the fault-handler reference log on the first fault against the
     * watched range - independent of any other debug switch. */
    if (mmfault_hi && watch_va_end && set_ifa &&
        ifa >= watch_va_base && ifa < watch_va_end)
        mmfault_armed = 1;
    if (vec == VEC_BREAK && m->cr[CR_IIM] == 0x80014 &&
        getenv("MERCED_NT_DEBUGPRINT"))
        nt_debugprint(m);
    if ((m->ip & ~UINT64_C(0xF)) == UINT64_C(0xE00000008311D550) ||
        (m->ip & ~UINT64_C(0xF)) == UINT64_C(0xE00000008311D5A0)) {
        static unsigned memset_pnp_debug;
        if (memset_pnp_debug++ < 8)
            fprintf(stderr, "merced: memset fault vec=%04X ifa=%016" PRIX64
                    " r32=%016" PRIX64 " r33=%016" PRIX64
                    " r34=%016" PRIX64 " lc=%016" PRIX64
                    " b0=%016" PRIX64 " ninsts=%" PRIu64 "\n",
                    vec, ifa, gr_read(m, 32, NULL), gr_read(m, 33, NULL),
                    gr_read(m, 34, NULL), m->ar[AR_LC], m->br[0],
                    m->ninsts);
    }
    if (vec == VEC_PAGE_NOT_PRESENT &&
        ifa == UINT64_C(0xE000010600000000)) {
        static unsigned target_pnp_debug;
        unsigned target_hit = target_pnp_debug++;
        target_trap_slot_armed = true;
        if (target_hit < 8)
            fprintf(stderr, "merced: target PNP ip=%016" PRIX64
                    " psr=%016" PRIX64 " ic=%u isr=%016" PRIX64
                    " iha=%016" PRIX64 " ipsr=%016" PRIX64
                    " iip=%016" PRIX64 " ninsts=%" PRIu64
                    " r32=%016" PRIX64 " r33=%016" PRIX64
                    " r34=%016" PRIX64 " lc=%016" PRIX64
                    " b0=%016" PRIX64 "\n",
                    m->ip, m->psr, !!(m->psr & PSR_IC), isr,
                    m->cr[CR_IHA], m->cr[CR_IPSR], m->cr[CR_IIP],
                    m->ninsts, gr_read(m, 32, NULL), gr_read(m, 33, NULL),
                    gr_read(m, 34, NULL), m->ar[AR_LC], m->br[0]);
        if (target_hit == 1) {
            fprintf(stderr, "merced: target-handler final 512 instructions\n");
            merced_dump_trace(m, 512, stderr);
            unsigned avail = m->call_history_next < MERCED_CALL_HISTORY
                           ? m->call_history_next : MERCED_CALL_HISTORY;
            for (unsigned i = m->call_history_next - avail;
                 i < m->call_history_next; i++) {
                unsigned h = i % MERCED_CALL_HISTORY;
                fprintf(stderr, "merced: target-hist %s %016" PRIX64
                        " -> %016" PRIX64 "\n",
                        m->call_history[h].is_return ? "ret " : "call",
                        m->call_history[h].from & ~UINT64_C(0xF),
                        m->call_history[h].to & ~UINT64_C(0xF));
            }
        }
    }
    if (vec == VEC_PAGE_NOT_PRESENT && getenv("MERCED_PNP_DEBUG")) {
        if (ifa == 0x20) {
            /* The NULL+0x20 spin: dump the faulting code's own register file
             * and the bundles around it, so the loop can be identified
             * without a kernel debugger.  deliver_fault() runs before the
             * interruption state switch, so the GRs here are still the
             * faulting instruction's. */
            static unsigned pnp20_debug;
            if (pnp20_debug++ < 4) {
                fprintf(stderr, "merced: PNP ifa=0x20 at ip=%016" PRIX64
                        " isr=%016" PRIX64 " ninsts=%" PRIu64
                        " cfm sof=%u sol=%u bof=%u\n",
                        m->ip, isr, m->ninsts, CFM_SOF(m->cfm),
                        CFM_SOL(m->cfm), m->bof);
                for (unsigned r = 1; r < 48; r += 4) {
                    fprintf(stderr, "merced:   r%-3u", r);
                    for (unsigned k = 0; k < 4 && r + k < 48; k++)
                        fprintf(stderr, " %016" PRIX64,
                                gr_read(m, r + k, NULL));
                    fprintf(stderr, "\n");
                }
                uint64_t base = (m->ip & ~0xFull) - 0x60;
                for (unsigned b = 0; b < 13; b++) {
                    uint64_t va = base + b * 16, pa;
                    MercedStatus st2;
                    if (!va_translate(m, va, true, true, 0, &pa, &st2))
                        continue;
                    fprintf(stderr, "merced:   bundle %016" PRIX64
                            " (pa %012" PRIX64 ") %016" PRIX64
                            " %016" PRIX64 "%s\n", va, pa,
                            phys_read(m, pa, 8), phys_read(m, pa + 8, 8),
                            va == (m->ip & ~0xFull) ? "  <== faulting" : "");
                }
                /* The loop is a round-robin scan of table[1..r28] anchored at
                 * r18, with its cursor in memory at r19; dump both so the
                 * empty table itself, not just the NULL deref, is visible. */
                uint64_t table = gr_read(m, 18, NULL);
                uint64_t limit = gr_read(m, 28, NULL);
                if (limit > 16) limit = 16;
                fprintf(stderr, "merced:   cursor [%016" PRIX64 "] = %u\n",
                        gr_read(m, 19, NULL),
                        dbg_u32(m, gr_read(m, 19, NULL)));
                for (unsigned e = 0; e <= limit; e++) {
                    uint64_t ent = 0;
                    if (!dbg_read(m, table + e * 8, &ent, sizeof(ent)))
                        break;
                    fprintf(stderr, "merced:   table[%u] @%016" PRIX64
                            " = %016" PRIX64 "\n", e, table + e * 8, ent);
                }
                nt_identify(m, m->ip & ~0xFull, "faulting code", true);
                nt_identify(m, m->br[0] & ~0xFull, "return address b0", true);
                nt_identify(m, table, "table base r18", false);
                nt_identify(m, gr_read(m, 19, NULL), "cursor r19", false);
                /* With no unwind information the call-history ring is the only
                 * record of who asked for the allocation.  Dump it raw and
                 * whole - naming 128 addresses in-emulator would mean 128
                 * export-table walks, and the addresses are just as easily
                 * resolved offline against the loaded image. */
                unsigned avail = m->call_history_next < MERCED_CALL_HISTORY
                               ? m->call_history_next : MERCED_CALL_HISTORY;
                for (unsigned i = m->call_history_next - avail;
                     i < m->call_history_next; i++) {
                    unsigned h = i % MERCED_CALL_HISTORY;
                    fprintf(stderr, "merced:   hist %s %016" PRIX64
                            " -> %016" PRIX64 "\n",
                            m->call_history[h].is_return ? "ret " : "call",
                            m->call_history[h].from & ~UINT64_C(0xF),
                            m->call_history[h].to & ~UINT64_C(0xF));
                }
            }
        }
        /* Track whether repeated page-not-present faults are hitting a
         * small, fixed set of addresses (handler's fixup isn't sticking)
         * or a large, growing set (genuine forward progress). */
        static uint64_t pnp_addrs[256];
        static uint32_t pnp_counts[256];
        static unsigned pnp_nseen;
        static uint64_t pnp_total, pnp_overflow;
        pnp_total++;
        unsigned i;
        for (i = 0; i < pnp_nseen; i++) {
            if (pnp_addrs[i] == ifa) {
                pnp_counts[i]++;
                break;
            }
        }
        if (i == pnp_nseen) {
            if (pnp_nseen < 256) {
                pnp_addrs[pnp_nseen] = ifa;
                pnp_counts[pnp_nseen] = 1;
                pnp_nseen++;
            } else {
                pnp_overflow++;
            }
        }
        if (pnp_total % 50000 == 0) {
            fprintf(stderr, "merced: PNP dist total=%" PRIu64
                    " distinct=%u overflow=%" PRIu64 "\n",
                    pnp_total, pnp_nseen, pnp_overflow);
            for (unsigned k = 0; k < pnp_nseen; k++)
                fprintf(stderr, "  ifa=%016" PRIX64 " count=%u\n",
                        pnp_addrs[k], pnp_counts[k]);
        }
    }
    if ((ifa >> 61) == 4) {
        static unsigned region4_debug;
        unsigned hit = region4_debug++;
        if (hit < 12)
            fprintf(stderr, "merced: REGION4 fault vec=%04X ip=%016" PRIX64
                    " ifa=%016" PRIX64 " isr=%016" PRIX64
                    " ic=%u it=%u dt=%u rt=%u pta=%016" PRIX64
                    " rr4=%016" PRIX64 " ninsts=%" PRIu64 "\n",
                    vec, m->ip, ifa, isr,
                    !!(m->psr & PSR_IC), !!(m->psr & PSR_IT),
                    !!(m->psr & PSR_DT), !!(m->psr & PSR_RT),
                    m->cr[CR_PTA], m->rr[4], m->ninsts);
        if (hit == 0 && getenv("MERCED_DEBUG_REGION4")) {
            for (unsigned r = 0; r < 64; r += 4)
                fprintf(stderr, "merced: R4 r%-2u=%016" PRIX64 "/%u"
                        " r%-2u=%016" PRIX64 "/%u"
                        " r%-2u=%016" PRIX64 "/%u"
                        " r%-2u=%016" PRIX64 "/%u\n",
                        r, gr_read(m, r, NULL), gr_nat(m, r),
                        r + 1, gr_read(m, r + 1, NULL), gr_nat(m, r + 1),
                        r + 2, gr_read(m, r + 2, NULL), gr_nat(m, r + 2),
                        r + 3, gr_read(m, r + 3, NULL), gr_nat(m, r + 3));
            merced_dump_trace(m, 128, stderr);
        }
    }
    if ((vec & 0xFF) == 0 && (vec >> 8) <
        sizeof(fault_vector_counts) / sizeof(fault_vector_counts[0]))
        fault_vector_counts[vec >> 8]++;
    static unsigned low_fault_debug;
    if (getenv("MERCED_FAULT_DEBUG") && (m->ip >> 61) == 7 &&
        set_ifa && ifa < UINT64_C(0x100000) && low_fault_debug++ < 32) {
        fprintf(stderr, "merced: low fault vec=%04X ip=%016" PRIX64
                " ifa=%016" PRIX64 " iha=%016" PRIX64
                " pta=%016" PRIX64 " isr=%016" PRIX64 " ic=%u\n",
                vec, m->ip, ifa, m->cr[CR_IHA], m->cr[CR_PTA], isr,
                !!(m->psr & PSR_IC));
        for (unsigned r = 0; r < 64; r += 4)
            fprintf(stderr, "  r%-2u=%016" PRIX64 " r%-2u=%016" PRIX64
                    " r%-2u=%016" PRIX64 " r%-2u=%016" PRIX64 "\n",
                    r, gr_read(m, r, NULL), r + 1, gr_read(m, r + 1, NULL),
                    r + 2, gr_read(m, r + 2, NULL),
                    r + 3, gr_read(m, r + 3, NULL));
    }
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
        for (unsigned i = 0; i < MERCED_N_DTR; i++)
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
        if (m->cr[CR_IVA] == UINT64_C(0x10000) && getenv("MERCED_DEBUG_SALFAULT")) {
            /* Same SAL-handoff window as the collected-path hook below, but
             * this fault is arriving with ic already 0 - either a genuine
             * nested fault inside a PAL handler, or code that itself cleared
             * psr.ic (e.g. the rfi mode-switch trampoline) faulting before
             * re-enabling it.  cr.iip/ipsr are frozen from whatever collected
             * fault happened last, so log m->ip (the actual current bundle)
             * instead - that is what pinpoints the real trigger. */
            static unsigned salnest_debug;
            if (salnest_debug++ < 16) {
                fprintf(stderr, "merced: SAL-handoff NESTED fault #%u vec=%04X"
                        " ip=%016" PRIX64 " psr=%016" PRIX64 " isr=%016"
                        PRIX64 " frozen_iip=%016" PRIX64 " frozen_ipsr=%016"
                        PRIX64 " ninsts=%" PRIu64 "\n", salnest_debug, vec,
                        m->ip, m->psr, isr, m->cr[CR_IIP], m->cr[CR_IPSR],
                        m->ninsts);
                fflush(stderr);
            }
        }
        /* Translation faults encountered while interruption collection is
         * disabled have dedicated vectors and must not overwrite the saved
         * interruption state.  The handlers use these paths to establish a
         * mapping needed by the original miss handler. */
        if (!m->psr_ic_inflight &&
            (vec == VEC_DTLB || vec == VEC_ALT_DTLB || vec == VEC_VHPT ||
             (vec == VEC_PAGE_NOT_PRESENT && !(isr & ISR_X)))) {
            /* Data Nested TLB faults preserve IFA, ITIR, IHA and ISR: they
             * belong to the interrupted outer fault.  In particular, a
             * missing mapping for a VHPT entry while vector 0 is running
             * comes here rather than recursively entering vector 0. */
            m->ip = (m->cr[CR_IVA] & ~0x7FFFull) + VEC_NESTED_DTLB;
            m->taken = 1;
            return MERCED_OK;
        }
        if (vec == VEC_ITLB || vec == VEC_ALT_ITLB) {
            /* PSR.ic == 0 means interruption collection is OFF: cr.ifa,
             * cr.itir and cr.iha belong to the *outer* interruption whose
             * handler is currently running, and the architecture does not
             * update them here.  Overwriting them corrupts the fault the OS
             * is in the middle of servicing - Windows' page-fault handler
             * runs with ic=0, re-reads cr.ifa after its fast path, and would
             * then be handed the address of its own code page (whichever
             * instruction fetch missed) instead of the data address that
             * faulted.  It duly maps that page, returns STATUS_SUCCESS, and
             * the original access faults forever.  Only ISR is reported, with
             * ni set to mark the nested interruption - matching the
             * reference core, which guards exactly these three registers
             * behind PSR.ic (ia64_raise_data_reference_exception_at()). */
            m->cr[CR_ISR] = isr | (1ull << 39); /* ni: nested interruption */
            m->ip = (m->cr[CR_IVA] & ~0x7FFFull) + VEC_ALT_ITLB;
            m->taken = 1;
            return MERCED_OK;
        }
        /* Any other fault taken with interruption collection disabled still
         * vectors normally - it just leaves cr.ifa/iip/ipsr/itir/iha holding
         * the outer interruption's state, which is exactly why an OS runs its
         * handlers with PSR.ic=0.  Halting instead turned a recoverable
         * nested fault into a dead machine.  ISR still reports the new fault
         * with ni set, matching the reference core. */
        m->cr[CR_ISR] = isr | (1ull << 39); /* ni: nested interruption */
        m->ip = (m->cr[CR_IVA] & ~0x7FFFull) + vec;
        m->taken = 1;
        return MERCED_OK;
    }
    if (m->cr[CR_IVA] == UINT64_C(0x10000) && getenv("MERCED_DEBUG_SALFAULT")) {
        /* The SAL boot-handoff window (fw_prepare_sal_handoff_registers)
         * sets cr.iva=0x10000 but never populates a real vector table there -
         * firmware relies on nothing faulting while this state is active.
         * Any fault landing here at all is the bug: log the first few with
         * full state so the *original* trigger (not the recursive-break
         * spin it collapses into) is visible. */
        static unsigned salfault_debug;
        if (salfault_debug++ < 8) {
            fprintf(stderr, "merced: SAL-handoff fault #%u vec=%04X ip=%016"
                    PRIX64 " psr=%016" PRIX64 " isr=%016" PRIX64
                    " ifa=%016" PRIX64 " set_ifa=%d cfm=%016" PRIX64
                    " pfs=%016" PRIX64 " b0=%016" PRIX64 " ninsts=%" PRIu64
                    "\n", salfault_debug, vec, m->ip, m->psr, isr, ifa,
                    set_ifa, m->cfm, m->ar[AR_PFS], m->br[0], m->ninsts);
            for (unsigned r = 0; r < 64; r += 4)
                fprintf(stderr, "  r%-2u=%016" PRIX64 " r%-2u=%016" PRIX64
                        " r%-2u=%016" PRIX64 " r%-2u=%016" PRIX64 "\n",
                        r, gr_read(m, r, NULL), r + 1, gr_read(m, r + 1, NULL),
                        r + 2, gr_read(m, r + 2, NULL),
                        r + 3, gr_read(m, r + 3, NULL));
            fflush(stderr);
        }
    }
    bool ia32 = (m->psr & PSR_IS) != 0;
    /* IA-32 CSD/SSD live in mapped GR25/GR26 while PSR.is is set, but in
     * AR.CSD/AR.SSD while native IA-64 code handles the interruption.  The
     * transition copies are architectural (SDM Vol. 1, 6.4.3).  Omitting
     * this lost the real-mode CS base across the first PIT interruption:
     * F000:FEA5's near jump was consequently resolved from base zero and
     * landed at 0000:E06F instead of F000:E06F. */
    if (ia32) {
        m->ar[25] = gr_read(m, 25, NULL); /* AR.CSD */
        m->ar[26] = gr_read(m, 26, NULL); /* AR.SSD */
    }
    unsigned slot = ia32 ? 0 : (unsigned)(m->ip & 3);
    uint64_t interrupted_iip =
        ia32 ? (uint32_t)m->ip : (m->ip & ~UINT64_C(0xF));
    uint64_t interrupted_ipsr =
        m->psr | ((uint64_t)slot << PSR_RI_SHIFT);
    if (vec == VEC_EXTINT)
        ext_interrupt_in_service = true;
    rse_preserve_interrupted_partition(m, interrupted_iip,
                                       interrupted_ipsr);
    m->cr[CR_IPSR] = interrupted_ipsr;
    m->cr[CR_IIP]  = interrupted_iip;
    m->cr[CR_ISR]  = isr | ((uint64_t)slot << 41) |
                     (m->psr_ic_inflight ? (1ull << 39) : 0);
    /*
     * IIPA names the most recently completed bundle, not necessarily the
     * bundle containing the interrupted instruction.  In particular a
     * slot-0 break/fault reports the preceding successful bundle, while a
     * later-slot fault reports the current bundle after an earlier slot
     * completed.  NT's kernel-breakpoint dispatcher uses this distinction
     * when advancing past break.i 0x80016.
     */
    m->cr[CR_IIPA] = ia32 ? (uint32_t)m->ip : m->last_successful_bundle;
    /* A collected interruption invalidates the interrupted-function state
     * as a whole.  Retaining stale IFM fields below IFS.v is architecturally
     * wrong: a subsequent cover in the handler can expose those fields to
     * rfi and restore an unrelated stacked-register frame. */
    m->cr[CR_IFS] = 0;
    if (set_ifa) {
        m->cr[CR_IFA] = ifa;
        unsigned vrn = (unsigned)(ifa >> 61);
        uint64_t rr = m->rr[vrn];
        /* Only faults that actually consult the VHPT initialize cr.iha.  The
         * alternate TLB, nested-DTLB, key-miss and key-permission vectors
         * leave it holding the value from whatever earlier interruption did
         * consult the VHPT - software reads it to find the hash entry, so
         * overwriting it here hands the handler an address for the wrong
         * fault.  Matches ia64_exception_initializes_iha() in the reference
         * core; covered by the alt_{d,i}tlb_preserves_iha cases. */
        if (vec != VEC_ALT_ITLB && vec != VEC_ALT_DTLB &&
            vec != VEC_NESTED_DTLB && vec != 0x1800 /* inst key miss */ &&
            vec != 0x1C00 /* data key miss */ && vec != 0x5100 /* key perm */)
            m->cr[CR_IHA] = vhpt_hash_address(m, ifa);
        /* A VHPT Translation fault describes the translation needed to
         * access cr.iha, not the translation of the original cr.ifa. */
        if (vec == VEC_VHPT)
            rr = m->rr[m->cr[CR_IHA] >> 61];
        m->cr[CR_ITIR] = (rr & 0xFCu) |
                         (((rr >> 8) & 0xFFFFFFull) << 8);
    }
    /*
     * Exception entry does not merely clear ic/i.  The interruption PSR is
     * reconstructed from the architecturally preserved fields, with be/pp
     * supplied by cr.dcr (SDM 5.5.2).  In particular dfl/dfh/sp/di/si from
     * the interrupted context must not leak into the handler.  NT relies on
     * this while its page-fault path uses the banked/FP execution resources.
     * IPSR above retains the complete old PSR for rfi.
     */
    m->psr &= PSR_UP | PSR_MFL | PSR_MFH | PSR_PK | PSR_DT |
              PSR_RT | PSR_MC | PSR_IT;
    if (m->cr[CR_DCR] & (1ull << 1))
        m->psr |= PSR_BE;
    if (m->cr[CR_DCR] & (1ull << 0))
        m->psr |= PSR_PP;
    m->psr_ic_inflight = 0;
    m->ip = (m->cr[CR_IVA] & ~0x7FFFull) + vec;
    m->taken = 1;
    return MERCED_OK;
}

static bool va_translate(Merced *m, uint64_t va, bool ifetch, bool spec,
                         uint64_t isr_access,
                         uint64_t *pa, MercedStatus *st) {
    bool ok = va_translate_ex(m, va, ifetch, spec, isr_access, false, pa, st);
    /* MERCED_WATCH_VA traces every data reference to a virtual range and the
     * physical address it resolved to.  Watching the VA rather than the PA is
     * what separates "the guest never wrote this" from "the guest wrote it
     * and we aliased the store somewhere else". */
    if (mmfault_armed && mmfault_hi && !ifetch && m->ip >= mmfault_lo &&
        m->ip < mmfault_hi && !spec) {
        static unsigned mmf_dbg;
        if (mmf_dbg++ < 400)
            fprintf(stderr, "merced: MMF %s va=%016" PRIX64 " -> %s%016" PRIX64
                    " ip=%016" PRIX64 "\n",
                    (isr_access & ISR_W) ? "wr" : "rd", va,
                    ok ? "pa=" : "FAULT ", ok ? *pa : 0, m->ip);
    }
    if (va >= watch_va_base && va < watch_va_end && !spec) {
        static unsigned watch_va_debug;
        if (watch_va_debug++ < 64) {
            fprintf(stderr, "merced: WATCH va=%016" PRIX64 " -> %s%016" PRIX64
                    " %s ip=%016" PRIX64 " ninsts=%" PRIu64 "\n",
                    va, ok ? "pa=" : "FAULT ", ok ? *pa : 0,
                    (isr_access & ISR_W) ? "write" : "read",
                    m->ip, m->ninsts);
            fflush(stderr);
        }
    }
    if (!ifetch && !spec &&
        (m->ip & ~UINT64_C(0xF)) == UINT64_C(0xE00000008301BE20) &&
        getenv("MERCED_DEBUG_XP_PTE_LOOP")) {
        static unsigned xp_pte_loop_reads;
        if (xp_pte_loop_reads++ == 1) {
            fprintf(stderr, "merced: XP-PTE-LOOP second read va=%016" PRIX64
                    " value=%016" PRIX64 " ninsts=%" PRIu64 "\n",
                    va, ok ? phys_read(m, *pa, 8) : 0, m->ninsts);
            merced_dump_trace(m, 800, stderr);
            fflush(stderr);
        }
    }
    return ok;
}

/* Effective R/W/X permission a PTE's ar/pl grants at a given privilege
 * level (SDM Vol.2 4.4, "Memory Access Rights").  ar 4-6 grant extra rights
 * to code running strictly more privileged than pl; ar 7 is the promotion
 * (gate page) encoding, which is executable at every level. */
static unsigned tlb_effective_perm(unsigned ar, unsigned pl, unsigned cpl) {
    if (cpl > 3 || pl > 3)
        return 0;
    switch (ar & 7) {
    case 0: return cpl <= pl ? PERM_R : 0;
    case 1: return cpl <= pl ? (PERM_R | PERM_X) : 0;
    case 2: return cpl <= pl ? (PERM_R | PERM_W) : 0;
    case 3: return cpl <= pl ? (PERM_R | PERM_W | PERM_X) : 0;
    case 4: return cpl > pl ? 0 : (cpl < pl ? (PERM_R | PERM_W) : PERM_R);
    case 5: return cpl > pl ? 0
                            : (cpl < pl ? (PERM_R | PERM_W | PERM_X)
                                        : (PERM_R | PERM_X));
    case 6: return cpl > pl ? 0
                            : (cpl < pl ? (PERM_R | PERM_W | PERM_X)
                                        : (PERM_R | PERM_W));
    default: return cpl == 0 ? (PERM_R | PERM_X) : PERM_X;
    }
}

/* Protection-key check.  A key with no matching valid PKR raises Key Miss;
 * a match whose rd/wd/xd bit denies the access raises Key Permission. */
static uint32_t key_fault_vector(Merced *m, uint32_t key, unsigned needed,
                                 bool ifetch) {
    uint64_t match = 0;
    bool matched = false;
    for (unsigned i = 0; i < 16; i++) {
        if ((m->pkr[i] & PKR_VALID) &&
            ((m->pkr[i] >> PKR_KEY_SHIFT) & 0xFFFFFFull) == key) {
            match = m->pkr[i];
            matched = true;
            break;
        }
    }
    if (!matched)
        return ifetch ? VEC_INST_KEY_MISS : VEC_DATA_KEY_MISS;
    if (((needed & PERM_R) && (match & PKR_RD)) ||
        ((needed & PERM_W) && (match & PKR_WD)) ||
        ((needed & PERM_X) && (match & PKR_XD)))
        return VEC_KEY_PERMISSION;
    return 0;
}

/* Faults a present translation can still raise, in the architected priority
 * order: NaT Page Consumption, Key Miss, Key Permission, Access Rights,
 * Dirty Bit, Access Bit.  Returns 0 when the access may proceed.
 *
 * The Dirty Bit fault deliberately outranks the Data Access Bit fault, so a
 * store to a page with both bits clear reports the dirty vector - that is
 * what lets an OS resolve both bits in one handler rather than taking a
 * second fault. */
static uint32_t translation_fault_vector_at_pl(Merced *m,
                                               const MercedTlbEntry *e,
                                               unsigned needed, bool ifetch,
                                               bool is_write, bool is_rse,
                                               unsigned cpl) {
    uint64_t pte = e->pte;
    unsigned perm;
    uint32_t kv;

    if (((pte >> PTE_MA_SHIFT) & 7) == PTE_MA_NATPAGE)
        return VEC_NAT;

    /* Key checks apply only with PSR.pk set and the matching translation
     * bit on - the RSE uses PSR.rt rather than PSR.dt. */
    if ((m->psr & PSR_PK) &&
        (m->psr & (ifetch ? PSR_IT : (is_rse ? PSR_RT : PSR_DT)))) {
        kv = key_fault_vector(m, (uint32_t)((e->itir >> 8) & 0xFFFFFFull),
                              needed, ifetch);
        if (kv)
            return kv;
    }

    perm = tlb_effective_perm((unsigned)((pte >> PTE_AR_SHIFT) & 7),
                              (unsigned)((pte >> PTE_PL_SHIFT) & 3), cpl);
    if ((perm & needed) != needed)
        return ifetch ? VEC_INST_ACCESS_RIGHTS : VEC_DATA_ACCESS_RIGHTS;

    if (ifetch) {
        if (!(pte & PTE_ACCESSED) && !(m->psr & PSR_IA))
            return VEC_IACCESS;
    } else {
        if (is_write && !(pte & PTE_DIRTY) && !(m->psr & PSR_DA))
            return VEC_DIRTY;
        if (!(pte & PTE_ACCESSED) && !(m->psr & PSR_DA))
            return VEC_DACCESS;
    }
    return 0;
}

static uint32_t translation_fault_vector(Merced *m, const MercedTlbEntry *e,
                                         unsigned needed, bool ifetch,
                                         bool is_write, bool is_rse) {
    unsigned cpl = (unsigned)((m->psr >> PSR_CPL_SHIFT) & 3);
    return translation_fault_vector_at_pl(m, e, needed, ifetch, is_write,
                                          is_rse, cpl);
}

/* DCR deferral-enable bit for a fault a control-speculative load can defer.
 * UINT64_MAX means "always defers" (unimplemented data address); 0 means the
 * fault is not deferrable through the DCR at all. */
static uint64_t spec_dcr_mask(uint32_t vec) {
    switch (vec) {
    case VEC_ALT_DTLB: case VEC_VHPT: case VEC_DTLB: return 1ull << 8;  /* dm */
    case VEC_PAGE_NOT_PRESENT:                        return 1ull << 9;  /* dp */
    case VEC_DATA_KEY_MISS:                           return 1ull << 10; /* dk */
    case VEC_KEY_PERMISSION:                          return 1ull << 11; /* dx */
    case VEC_DATA_ACCESS_RIGHTS:                      return 1ull << 12; /* dr */
    case VEC_DACCESS:                                 return 1ull << 13; /* da */
    default:                                          return 0;
    }
}

/* The ED bit of the page holding the currently executing bundle.  It only
 * has meaning when instruction translation is on. */
static bool code_page_ed(Merced *m) {
    unsigned vrn;
    uint32_t rid;
    const MercedTlbEntry *e;
    if (!(m->psr & PSR_IT))
        return false;
    vrn = (unsigned)(m->ip >> 61);
    rid = (uint32_t)((m->rr[vrn] >> 8) & 0xFFFFFFull);
    e = tlb_search(m->itr, MERCED_N_ITR, rid, m->ip & ~0xFull, &m->itr_hint);
    if (!e)
        e = tlb_search(m->itc, MERCED_N_TC, rid, m->ip & ~0xFull, &m->itc_hint);
    return e && (e->pte & (1ull << 52));
}

/* Whether a control-speculative (ld.s) reference defers this fault - taking
 * a NaT instead of an interruption - or actually raises it.
 *
 * We used to defer unconditionally, which is wrong in both directions: it
 * hides faults software expects to see, and it makes an ld.s look successful
 * where hardware would have trapped.  Per SDM Vol.2 (control speculation)
 * there are five INDEPENDENT enables. */
static bool spec_defers(Merced *m, uint32_t vec) {
    uint64_t mask;
    if (!(m->psr & PSR_IC))
        return true;                          /* no way to report it */
    if (vec == VEC_NAT)
        return true;                          /* NaTVal source always defers */
    mask = spec_dcr_mask(vec);
    if (mask == UINT64_MAX)
        return true;                          /* unimplemented data address */
    if (code_page_ed(m))
        return true;
    /* The DCR path is independent of PSR.it, so an ld.s issued from
     * physical-mode code still defers when the DCR bit is set. */
    return mask != 0 && (m->cr[CR_DCR] & mask);
}

static bool va_translate_ex(Merced *m, uint64_t va, bool ifetch, bool spec,
                            uint64_t isr_access, bool force,
                            uint64_t *pa, MercedStatus *st) {
    /* The debug helpers (symbolization, disassembly, pointer following) probe
     * translations with spec=true and no access type.  Those must never
     * perturb the guest, so they defer unconditionally; only a real
     * control-speculative load consults the architected deferral rules. */
    bool silent_probe = spec && isr_access == 0;
    /* A speculative reference whose fault is not deferred still reports that
     * it was speculative. */
    uint64_t spec_isr = spec ? ISR_SP : 0;
    /* ISR.ed describes the translation of the instruction which issued a
     * data reference, not the data translation itself.  When its code-page
     * PTE has ED set, all deferrable data faults report ISR.ed so the OS can
     * choose its deferred-exception path.  NT maps the PFN initialization
     * loop this way; omitting ED made its VHPT handler treat the fault as an
     * ordinary miss and retry the same store forever.  NaT-page consumption
     * is the architectural exception and forces ED clear below. */
    uint64_t code_ed_isr = (!ifetch && code_page_ed(m)) ? ISR_ED : 0;
    *st = MERCED_OK;
    bool is_rse = !ifetch && (isr_access & ISR_RS) != 0;
    bool on = force ||
              (ifetch ? (m->psr & PSR_IT) != 0
                      : (m->psr & (is_rse ? PSR_RT : PSR_DT)) != 0);
    if (!on) {
        *pa = va & MERCED_PHYS_MASK;
        return true;
    }
    unsigned vrn = (unsigned)(va >> 61);
    uint32_t rid = (uint32_t)((m->rr[vrn] >> 8) & 0xFFFFFFull);
    uint64_t lookup_va = va;
    /* Firmware owns this identity window while its IVT is installed, and
     * while runtime firmware code is executing during the IVA handoff.  It
     * must take precedence over stale/temporary guest TRs: firmware's RFI
     * exception self-test deliberately installs an alternate translation
     * for the same VA, then verifies that changing IVA hands precedence to
     * that translation. */
    if (firmware_identity_pa(m, va, pa))
        return true;
    const MercedTlbEntry *e =
        tlb_search(ifetch ? m->itr : m->dtr,
                   ifetch ? MERCED_N_ITR : MERCED_N_DTR, rid, lookup_va,
                   ifetch ? &m->itr_hint : &m->dtr_hint);
    if (!e)
        e = tlb_search(ifetch ? m->itc : m->dtc, MERCED_N_TC, rid, lookup_va,
                       ifetch ? &m->itc_hint : &m->dtc_hint);
    if (!e && !ifetch && (m->cr[CR_IVA] >> 61) == 0) {
        /* Merced-style unified behavior: the i2000 firmware maps its 4 MiB
         * shadow with itr.i only, yet reads/writes data in that region with
         * translation on (e.g. the alt-DTLB handler's own descriptor at
         * [r12+112]). Keep that firmware compatibility only while a
         * region-0 IVT owns interruption handling. ITRs and DTRs are
         * architecturally independent once an OS installs its region-7 IVT;
         * XP intentionally has an instruction-only [64,80 MiB] staging TR
         * whose data references must fall through to the KSEG alias below. */
        e = tlb_search(m->itr, MERCED_N_ITR, rid, lookup_va, &m->itr_hint);
        if (!e)
            e = tlb_search(m->itc, MERCED_N_TC, rid, lookup_va, &m->itc_hint);
    }
    /*
     * The loader and early NT kernel retain pointers through the region-7
     * KSEG alias VA payload = 0x80000000 + PA.  It remains valid after the
     * firmware IVT is replaced; otherwise KdInitSystem faults before the
     * recursive page-table window is ready and cascades into a PNP loop.
     * Explicit TR/TC translations above always take precedence.
     */
    if (!e && vrn == 7 && m->bus.ram_size) {
        uint64_t payload = va & UINT64_C(0x1FFFFFFFFFFFFFFF);
        if (payload >= UINT64_C(0x80000000) &&
            payload < m->region7_directmap_limit) {
            *pa = payload - UINT64_C(0x80000000);
            return true;
        }
    }
    /* Before ExitBootServices, the SAL environment owns the IVT and services
     * loader translation misses with identity mappings, including the 460GX
     * 0xe00 high-DRAM aperture used for the firmware RSE backing store. This
     * is firmware handoff behavior, not a permanent region shortcut: it is
     * active only
     * while a generic firmware IVA is installed and interruption state is
     * collectible.  Explicit guest TR/TC entries above always win.  This
     * matches ia64_sal_boot_virtual_pa() in the reference IA-64 QEMU core,
     * which checks against IA64_FIRMWARE_IVT_BASE (0x10000) - the vector
     * table location reference/qemu-system-ia64-merced's own ia64-firmware.bin
     * uses. GEMU's own firmware (reference/gemu-efi/src/start.s) instead
     * installs its IVT at 0xFFF80000, inside its top-aligned ROM image (see
     * that file's cr.iva setup); both are legitimate firmware images this
     * emulator boots, so both IVA values get the same boot-time shortcut. */
    if (!e && (m->cr[CR_IVA] == 0 ||
               m->cr[CR_IVA] == UINT64_C(0x0000000000010000) ||
               m->cr[CR_IVA] == UINT64_C(0x00000000FFF80000)) &&
        (m->psr & PSR_IC) && vrn == 0 && va <= MERCED_PHYS_MASK) {
        *pa = va;
        if (getenv("MERCED_MMU_DEBUG")) {
            static unsigned sal_identity_debug;
            if (sal_identity_debug++ < 128)
                fprintf(stderr, "merced: XLATE SAL identity ip=%016" PRIX64
                        " va=%016" PRIX64 " ninsts=%" PRIu64 "\n",
                        m->ip, va, m->ninsts);
        }
        return true;
    }
    if (!e) {
        int walk = vhpt_walk(m, va, ifetch);
        if (walk == VHPT_HIT) {
            /* Walk succeeded and installed a TC entry exactly as software's
             * itc.d/itc.i would have - re-search to pick it up. */
            e = tlb_search(ifetch ? m->itc : m->dtc, MERCED_N_TC,
                           rid, lookup_va, ifetch ? &m->itc_hint : &m->dtc_hint);
        } else if (walk == VHPT_NOT_PRESENT) {
            if (spec && (silent_probe || spec_defers(m, VEC_PAGE_NOT_PRESENT))) return false;
            *st = deliver_fault(m, VEC_PAGE_NOT_PRESENT,
                                isr_access | spec_isr | code_ed_isr, va, true);
            return false;
        } else if (walk == VHPT_TRANSLATION) {
            if (spec && (silent_probe || spec_defers(m, VEC_VHPT))) return false;
            /* The walker was enabled and the translation for the VHPT
             * entry itself was absent.  This is not an ordinary ITLB/DTLB
             * miss: vector 0 lets the OS make its page-table backing
             * resident before retrying the original reference. */
            *st = deliver_fault(m, VEC_VHPT,
                                isr_access | spec_isr | code_ed_isr, va, true);
            return false;
        }
    }
    if (!e) {
        /* Vector choice depends on whether the VHPT walker would have run
         * for this reference: pta.ve=0 or rr.ve=0 disables it, and misses
         * then raise the Alternate ITLB/DTLB vectors. A walker that's
         * enabled but found nothing (vhpt_walk() above returned false)
         * still reaches the plain miss vectors here - exactly matching
         * real hardware, whose walker also falls through to the
         * software-visible miss fault when the VHPT itself doesn't
         * resolve the reference. */
        bool walker = (m->cr[CR_PTA] & 1) && (m->rr[vrn] & 1);
        uint32_t fvec;
        if (ifetch)
            fvec = walker ? VEC_ITLB : VEC_ALT_ITLB;
        else
            fvec = walker ? VEC_DTLB : VEC_ALT_DTLB;
        if (spec && (silent_probe || spec_defers(m, fvec)))
            return false;         /* ld.s: caller sets NaT, no fault */
        *st = deliver_fault(m, fvec,
                            isr_access | spec_isr | code_ed_isr, va, true);
        return false;
    }
    if (!(e->pte & PTE_PRESENT)) {
        if (spec && (silent_probe || spec_defers(m, VEC_PAGE_NOT_PRESENT))) return false;
        *st = deliver_fault(m, VEC_PAGE_NOT_PRESENT,
                            isr_access | spec_isr | code_ed_isr, va, true);
        return false;
    }
    {
        /* isr_access carries what the reference calls "needed": which of
         * read/write/execute this reference actually requires. */
        unsigned needed = ifetch ? PERM_X : 0;
        uint32_t fvec;
        if (isr_access & ISR_R) needed |= PERM_R;
        if (isr_access & ISR_W) needed |= PERM_W;
        if (isr_access & ISR_X) needed |= PERM_X;
        fvec = translation_fault_vector(m, e, needed, ifetch,
                                        (isr_access & ISR_W) != 0, is_rse);
        if (fvec) {
            /* A control-speculative load defers a deferrable fault rather
             * than raising it; the caller turns our refusal into a NaT. */
            if (spec && (silent_probe || spec_defers(m, fvec))) return false;
            uint64_t fault_isr = isr_access | spec_isr | code_ed_isr;
            if (fvec == VEC_NAT) {
                fault_isr &= ~ISR_ED;
                fault_isr |= UINT64_C(0x20); /* NaT-page consumption code */
            }
            *st = deliver_fault(m, fvec, fault_isr, va, true);
            return false;
        }
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
/* Physical range to trace stores to, as MERCED_WATCH_PA[:LEN] (LEN defaults
 * to 0x40).  Resolved once - this sits on the store path, so a getenv() per
 * store would dominate the emulator's run time. */

static void watch_range(const char *var, uint64_t *base, uint64_t *end) {
    const char *spec = getenv(var);
    const char *colon;
    if (!spec) return;
    *base = strtoull(spec, NULL, 0);
    colon = strchr(spec, ':');
    *end = *base + (colon ? strtoull(colon + 1, NULL, 0) : 0x40);
}

/* Resolved once at startup: merced_step() runs billions of times, so a
 * getenv() per instruction would cost more than the emulation itself. */
static bool heartbeat_on;
static bool trace_history_on;
static bool trace_regs_on;

/* MERCED_WATCH_IP=<addr>: log the incoming argument registers every time
 * execution reaches a bundle.  Aimed at guest function entries - br.call has
 * already rotated the frame by then, so r32.. are the arguments. */
/* MERCED_TRACE_LO/HI: log every taken control transfer whose source lies in
 * [lo,hi).  One run then shows the exact path through a guest function, which
 * beats bisecting candidate branches one boot at a time. */
static uint64_t trace_lo, trace_hi;
static uint64_t trace_after_ninsts;
static uint64_t capture_lo, capture_hi;
static uint64_t watch_ip_after;
static bool cdb_validator_debug_on;
static bool cdb_force_clear_on;
static bool cdb_d8a0_debug_on;
static bool cdb_iter_debug_on;
static uint64_t cdb_iter_after_ninsts;
static bool atapi_trap_debug_on;
static bool translation_cache_on;
static bool ata_flow_debug_on;
static bool flash_loop_ret_debug_on;
static bool flash_cmp_debug_on;
static bool ata_portfn_debug_on;
static bool debug_hooks_on;


#define WATCH_IP_MAX 4
static uint64_t watch_ip_addr[WATCH_IP_MAX];
static unsigned watch_ip_count;

static void watch_init(void) {
    static bool done;
    const char *ipspec;
    if (done) return;
    done = true;
    heartbeat_on = getenv("MERCED_HEARTBEAT") != NULL;
    trace_regs_on = getenv("MERCED_TRACE_REGS") != NULL;
    trace_history_on = trace_regs_on ||
                       getenv("MERCED_TRACE_HISTORY") != NULL ||
                       getenv("MERCED_CAPTURE_LO") != NULL;
    r5_debug_on = getenv("R5_DEBUG") != NULL;
    r8_debug_on = getenv("R8_DEBUG") != NULL;
    r24_debug_on = getenv("R24_DEBUG") != NULL;
    r29_debug_on = getenv("R29_DEBUG") != NULL;
    r18_debug_on = getenv("MERCED_R18_DEBUG") != NULL;
    r48_debug_on = getenv("MERCED_R48_DEBUG") != NULL;
    zero_loop_debug_on = getenv("MERCED_DEBUG_ZERO_LOOP") != NULL;
    bad_store_debug_on = getenv("MERCED_DEBUG_BAD_STORE") != NULL;
    if (getenv("MERCED_WATCH_AFTER"))
        watch_ip_after = strtoull(getenv("MERCED_WATCH_AFTER"), NULL, 0);
    ipspec = getenv("MERCED_WATCH_IP");
    while (ipspec && *ipspec && watch_ip_count < WATCH_IP_MAX) {
        watch_ip_addr[watch_ip_count++] =
            strtoull(ipspec, NULL, 0) & ~UINT64_C(0xF);
        ipspec = strchr(ipspec, ',');
        if (ipspec) ipspec++;
    }
    watch_range("MERCED_WATCH_PA", &watch_pa_base, &watch_pa_end);
    {
        const char *lo = getenv("MERCED_MMFAULT_LO");
        const char *hi = getenv("MERCED_MMFAULT_HI");
        if (lo && hi) {
            mmfault_lo = strtoull(lo, NULL, 0);
            mmfault_hi = strtoull(hi, NULL, 0);
        }
    }
    {
        const char *lo = getenv("MERCED_TRACE_LO");
        const char *hi = getenv("MERCED_TRACE_HI");
        if (lo && hi) {
            trace_lo = strtoull(lo, NULL, 0);
            trace_hi = strtoull(hi, NULL, 0);
        }
        const char *after = getenv("MERCED_TRACE_AFTER_NINSTS");
        if (after)
            trace_after_ninsts = strtoull(after, NULL, 0);
    }
    {
        const char *lo = getenv("MERCED_CAPTURE_LO");
        const char *hi = getenv("MERCED_CAPTURE_HI");
        if (lo && hi) {
            capture_lo = strtoull(lo, NULL, 0);
            capture_hi = strtoull(hi, NULL, 0);
        }
    }
    watch_range("MERCED_WATCH_VA", &watch_va_base, &watch_va_end);
    cdb_validator_debug_on = getenv("CDB_VALIDATOR_DEBUG") != NULL;
    cdb_force_clear_on = getenv("CDB_FORCE_CLEAR") != NULL;
    cdb_d8a0_debug_on = getenv("CDB_D8A0_DEBUG") != NULL;
    cdb_iter_debug_on = getenv("CDB_ITER_DEBUG") != NULL;
    const char *cdb_iter_after = getenv("CDB_ITER_AFTER_NINSTS");
    cdb_iter_after_ninsts = cdb_iter_after
                          ? strtoull(cdb_iter_after, NULL, 0)
                          : UINT64_C(5000000000);
    atapi_trap_debug_on = getenv("ATAPI_TRAP_DEBUG") != NULL;
    ata_flow_debug_on = getenv("ATA_FLOW_DEBUG") != NULL;
    flash_loop_ret_debug_on = getenv("FLASH_LOOP_RET_DEBUG") != NULL;
    flash_cmp_debug_on = getenv("FLASH_CMP_DEBUG") != NULL;
    ata_portfn_debug_on = getenv("ATA_PORTFN_DEBUG") != NULL;
    debug_hooks_on = trace_history_on || zero_loop_debug_on ||
                     ata_flow_debug_on || flash_loop_ret_debug_on ||
                     flash_cmp_debug_on || ata_portfn_debug_on ||
                     cdb_validator_debug_on || cdb_force_clear_on ||
                     cdb_d8a0_debug_on || cdb_iter_debug_on ||
                     atapi_trap_debug_on ||
                     getenv("MERCED_DEBUG_FIRSTENTRY") != NULL ||
                     getenv("MERCED_DEBUG_TBITCHECK") != NULL ||
                     getenv("MERCED_DEBUG_CHUNKLOOP") != NULL ||
                     getenv("MERCED_DEBUG_EFISTALL") != NULL ||
                     getenv("MERCED_DEBUG_POLLNODE") != NULL ||
                     getenv("MERCED_DEBUG_WAIT4_RESULT") != NULL ||
                     getenv("MERCED_DEBUG_R57FLAG") != NULL ||
                     getenv("MERCED_DEBUG_5532D0") != NULL ||
                     getenv("MERCED_DEBUG_XP_HANDLER") != NULL ||
                     getenv("MERCED_DEBUG_B520_ENTRY") != NULL;
    const char *tc = getenv("MERCED_TRANSLATION_CACHE");
    translation_cache_on = !tc || strcmp(tc, "0") != 0;
}

/* First-stage portable dynamic translation cache.  This does not emit host
 * machine code yet; it caches the translation and decoded form that a later
 * native backend will consume. */
#define MERCED_TB_ENTRIES 16384u
#define MERCED_TB_PAGE_SLOTS 4096u

typedef struct {
    uint64_t va, pa, lo, hi, slots[3];
    uint32_t translation_generation, code_generation;
    uint8_t tmpl, valid;
} MercedTbEntry;

typedef struct {
    MercedTbEntry entry[MERCED_TB_ENTRIES];
    uint32_t page_generation[MERCED_TB_PAGE_SLOTS];
    uint32_t translation_generation;
} MercedTranslationCache;

static inline unsigned tb_index(uint64_t va) {
    return (unsigned)(((va >> 4) ^ (va >> 18)) & (MERCED_TB_ENTRIES - 1));
}

static inline unsigned tb_page_index(uint64_t pa) {
    return (unsigned)(((pa >> 12) ^ (pa >> 24)) &
                      (MERCED_TB_PAGE_SLOTS - 1));
}

static void tb_flush(Merced *m) {
    MercedTranslationCache *tc = m->translation_cache;
    if (!tc) return;
    memset(tc, 0, sizeof(*tc));
    tc->translation_generation = 1;
}

void merced_flush_translation_cache(Merced *m) { tb_flush(m); }

static void phys_write(Merced *m, uint64_t pa, uint64_t v, unsigned size) {
    pa &= MERCED_PHYS_MASK;
    if (pa <= UINT64_C(0x0E000590) &&
        pa + size > UINT64_C(0x0E000590) && v &&
        getenv("MERCED_DEBUG_XP_PTE_WRITER")) {
        static bool xp_pte_writer_dumped;
        if (!xp_pte_writer_dumped) {
            xp_pte_writer_dumped = true;
            fprintf(stderr, "merced: XP-PTE-WRITER pa=%016" PRIX64
                    " value=%016" PRIX64 " size=%u ip=%016" PRIX64
                    " ninsts=%" PRIu64 "\n", pa, v, size, m->ip, m->ninsts);
            for (unsigned r = 0; r < 64; r += 4)
                fprintf(stderr, "merced: XPW r%u=%016" PRIX64
                        " r%u=%016" PRIX64 " r%u=%016" PRIX64
                        " r%u=%016" PRIX64 "\n",
                        r, gr_read(m, r, NULL), r + 1, gr_read(m, r + 1, NULL),
                        r + 2, gr_read(m, r + 2, NULL),
                        r + 3, gr_read(m, r + 3, NULL));
            merced_dump_trace(m, 128, stderr);
            fflush(stderr);
        }
    }
    if (pa <= UINT64_C(0x2714000) &&
        pa + size > UINT64_C(0x2714000)) {
        static unsigned target_pte_lifetime_writes;
        if (target_pte_lifetime_writes++ < 128)
            fprintf(stderr, "merced: TARGET-PTE-LIFETIME write"
                    " pa=%016" PRIX64 " v=%016" PRIX64
                    " size=%u ip=%016" PRIX64 " ninsts=%" PRIu64
                    " r32=%016" PRIX64 " r33=%016" PRIX64
                    " r34=%016" PRIX64 " lc=%016" PRIX64
                    " b0=%016" PRIX64 "\n",
                    pa, v, size, m->ip, m->ninsts,
                    gr_read(m, 32, NULL), gr_read(m, 33, NULL),
                    gr_read(m, 34, NULL), m->ar[AR_LC], m->br[0]);
    }
    if (target_vhpt_entry_pa && pa <= target_vhpt_entry_pa &&
        pa + size > target_vhpt_entry_pa && target_vhpt_entry_writes++ < 128) {
        fprintf(stderr, "merced: TARGET-PTE write pa=%016" PRIX64
                " v=%016" PRIX64 " size=%u ip=%016" PRIX64
                " ninsts=%" PRIu64 "\n",
                pa, v, size, m->ip, m->ninsts);
        fflush(stderr);
    }
    if (target_trap_slot_armed &&
        pa <= UINT64_C(0x767E6C0) &&
        pa + size > UINT64_C(0x767E6C0)) {
        static unsigned target_soft_pte_writes;
        if (target_soft_pte_writes++ < 32)
            fprintf(stderr, "merced: TARGET-SOFT-PTE write pa=%016" PRIX64
                    " v=%016" PRIX64 " size=%u ip=%016" PRIX64
                    " ninsts=%" PRIu64 "\n",
                    pa, v, size, m->ip, m->ninsts);
    }
    if (bad_store_debug_on && size == 8 &&
        v == UINT64_C(0xE000010600000000) &&
        (m->ip >> 60) == UINT64_C(0xE)) {
        static unsigned bad_store_count;
        if (bad_store_count++ < 64) {
            fprintf(stderr, "merced: BAD-STORE pa=%016" PRIX64
                    " ip=%016" PRIX64 " ninsts=%" PRIu64
                    " b0=%016" PRIX64 "\n",
                    pa, m->ip, m->ninsts, m->br[0]);
            if (bad_store_count == 1)
                merced_dump_trace(m, 32, stderr);
            fflush(stderr);
        }
    }
    if (target_trap_slot_armed && pa == UINT64_C(0x2049C78) &&
        target_trap_slot_writes++ < 128) {
        fprintf(stderr, "merced: TARGET-SLOT write pa=%016" PRIX64
                " v=%016" PRIX64 " size=%u ip=%016" PRIX64
                " ninsts=%" PRIu64 "\n",
                pa, v, size, m->ip, m->ninsts);
        fflush(stderr);
    }
    if (pa >= watch_pa_base && pa < watch_pa_end) {
        static unsigned watch_debug;
        if (watch_debug++ < 64) {
            fprintf(stderr, "merced: WATCH write pa=%016" PRIX64
                    " v=%016" PRIX64 " size=%u ip=%016" PRIX64
                    " ninsts=%" PRIu64 "\n",
                    pa, v, size, m->ip, m->ninsts);
            fflush(stderr);
        }
    }
    /* All CPU-visible stores can collide with an advanced load, including
     * semaphore, FP, and RSE spill stores. Advanced loads are rare, so the
     * ALAT is usually empty - skip the scan entirely in that case. */
    if (m->alat_valid_mask)
        alat_invalidate_store(m, pa, size);
    /* Self-modifying code: a store that overlaps the cached bundle's
     * physical bytes must not let a later slot re-read the stale cache. */
    if (m->bundle_cache_valid &&
        pa < m->bundle_cache_pa + 16 && m->bundle_cache_pa < pa + size)
        m->bundle_cache_valid = false;
    MercedTranslationCache *tc = m->translation_cache;
    if (tc) {
        unsigned first = tb_page_index(pa);
        unsigned last = tb_page_index(pa + size - 1);
        tc->page_generation[first]++;
        if (last != first)
            tc->page_generation[last]++;
    }
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
    m->translation_cache = calloc(1, sizeof(MercedTranslationCache));
    if (!m->translation_cache) { free(m); return NULL; }
    m->bus = *bus;
    watch_init();
    merced_reset(m);
    return m;
}

void merced_destroy(Merced *m) {
    if (!m) return;
    free(m->translation_cache);
    free(m);
}

void merced_reset(Merced *m) {
    MercedBus bus = m->bus;
    uint64_t ninsts = 0;
    uint8_t cpu_revision = m->cpu_revision;
    uint8_t cpu_model = m->cpu_model;
    void *translation_cache = m->translation_cache;
    memset(m, 0, sizeof(*m));
    m->bus = bus;
    m->ninsts = ninsts;
    m->cpu_revision = cpu_revision;
    m->cpu_model = cpu_model;
    m->translation_cache = translation_cache;
    tb_flush(m);
    m->region7_directmap_limit = UINT64_C(0x80000000) + bus.ram_size;

    m->ip  = 0xFFFFFFB0ull;          /* PALE_RESET, 4 GiB - 0x50 */
    m->psr = 0;                      /* physical mode, bank 0 */
    m->pr  = 1;                      /* pr0 = 1 */
    m->cfm = 96;                     /* whole stacked file addressable */
    m->group_start = 1;              /* reset implicitly begins a new group */
    /* Architected/reset floating-point environment.  In particular, the
     * precision-control fields for sf0..sf3 are 3 (register/extended
     * precision).  Leaving FPSR zero made compiler-generated integer
     * division sequences round every .s1 FMA intermediate to single
     * precision, corrupting even SETUPLDR's 40/40 memory-map calculation. */
    m->ar[AR_FPSR] = UINT64_C(0x0009804c0270033f);
    /* CPUID: "GenuineIntel". cpuid[3] is the version register:
     * {number 7:0, revision 15:8, model 23:16, family 31:24, archrev 39:32}
     * = archrev 0, family 7 (Itanium), model cpu_model (0 = Merced,
     * 1 = McKinley/Itanium 2), rev cpu_revision, number 4 (cpuid[0..4]
     * implemented).
     *
     * The model and revision (stepping) values are machine-configurable
     * (see merced_set_cpu_model()/merced_set_cpu_revision()) because
     * different consumers disagree about what they want to see here:
     *   - i2000's own firmware cross-checks it against
     *     machine_i2000.c's memcard_cfg[0][2][0x05] (CBN:05.2 processor
     *     descriptor) - a mismatch reads as "no recognized processor"
     *     and parks SAL at FFFE2020.  Revision 0 is therefore used during
     *     that firmware's early enumeration; the machine promotes CPUID to
     *     production Merced C0 when it hands off to a loaded EFI image.
     *   - Windows for Itanium (SETUPLDR.EFI on the generic machine)
     *     refuses to proceed on anything it reads as "pre-B3 stepping" -
     *     real historical behavior, not an emulator bug. The generic
     *     machines expose revision 6 for this reason. */
    memcpy(&m->cpuid[0], "GenuineI", 8);
    memcpy(&m->cpuid[1], "ntel\0\0\0\0", 8);
    m->cpuid[2] = 0;
    m->cpuid[3] = (7ull << 24) | ((uint64_t)m->cpu_model << 16) |
                  ((uint64_t)m->cpu_revision << 8) | 4;
    m->cpuid[4] = 0;
    m->cr[CR_LID] = 0;               /* cpu 0 */
    strcpy(m->halt_msg, "never ran");
}

void merced_set_cpu_revision(Merced *m, uint8_t revision) {
    m->cpu_revision = revision;
    m->cpuid[3] = (m->cpuid[3] & ~0xFF00ull) | ((uint64_t)revision << 8);
}

void merced_set_cpu_model(Merced *m, uint8_t model) {
    m->cpu_model = model;
    m->cpuid[3] = (m->cpuid[3] & ~0xFF0000ull) | ((uint64_t)model << 16);
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
    /*
     * A call rotates the stacked-register name space.  Any ALAT entry
     * associated with r32-r127 therefore ceases to name the register that
     * issued the advanced load and must be discarded.  Keeping it lets a
     * callee's ld.c incorrectly validate an advanced load performed by its
     * caller; NT's lock-free page-list code uses exactly this pattern.
     * Static-register entries remain valid across a call.
     */
    alat_invalidate_stacked(m);
    m->bof = (m->bof + sol) % MERCED_RSE_CAPACITY;
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
    m->bof = (m->bof + MERCED_RSE_CAPACITY - sol) % MERCED_RSE_CAPACITY;
    m->bof_total -= sol;
    m->cfm = new_cfm;
    m->ar[AR_EC] = (pfs >> 52) & 0x3F;
    /* Returning changes the stacked-register name mapping just as a call
     * does.  An ALAT association made in the callee must not accidentally
     * validate an ld.c in the caller that now has the same architectural
     * register number. */
    alat_invalidate_stacked(m);
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

static inline bool major_is_alu(unsigned major) {
    return major == 8 || major == 9 || (major >= 0xC && major <= 0xE);
}

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
            if (x2a == 3)
                res = (uint32_t)res | (((b >> 30) & 3) << 61);      /* addp4 */
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
        case 2:
            /* addp4 expands a 32-bit pointer into the region selected by
             * bits 31:30 of the base operand.  Those two bits become VA
             * bits 62:61; it does not preserve the base's existing region
             * field.  NT relies on this when expanding compact pointers in
             * its loader/page tables. */
            res = (uint32_t)(a + b) | (((b >> 30) & 3) << 61);
            break;
        case 3:
            switch (x2b) {
            case 0: res = a & b; break;
            case 1: res = a & ~b; break;      /* andcm */
            case 2: res = a | b; break;
            default: res = a ^ b; break;
            }
            break;
        case 4:  res = (a << (x2b + 1)) + b; break;                  /* shladd */
        case 6:
            res = (uint32_t)((a << (x2b + 1)) + b) |
                  (((b >> 30) & 3) << 61);                         /* shladdp4 */
            break;
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
            case 1: res = imm & ~b; break;    /* andcm imm8,r3 */
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
        if (getenv("CMP_A7_DEBUG") && m->ip == UINT64_C(0x7F59D260)) {
            static unsigned a7_debug;
            if (a7_debug++ < 20)
                fprintf(stderr, "merced: CMP-DECODE ip=%016" PRIX64
                        " major=%X tb=%u ta=%u c=%u x2=%u imm_form=%d"
                        " r2=%u r3=%u p1=%u p2=%u ninsts=%" PRIu64 "\n",
                        m->ip, major, tb, ta, c, x2, imm_form, r2, r3, p1, p2,
                        m->ninsts);
        }
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
            if (getenv("CMP_A7_DEBUG") && r3 == 9 && major == 0xC) {
                static unsigned a7_debug;
                if (a7_debug++ < 100)
                    fprintf(stderr, "merced: A7-CMP ip=%016" PRIX64 " major=%X"
                            " ta=%u c=%u sb=%" PRId64 " rel=%d p1=%u p2=%u"
                            " ninsts=%" PRIu64 "\n", m->ip, major, ta, c, sb,
                            rel, p1, p2, m->ninsts);
            }
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

    /* M21 check loads use x=3 (bits 28:27), which overlaps the low x bit
     * used by M16 semaphore encodings.  Decode them first.  Treating an
     * ld.c.clr as cmpxchg was particularly destructive: SETUPLDR uses the
     * ld.a/ld.c pair in its image/driver byte scanner, so the bogus atomic
     * operation changed both its cursor state and destination register.
     * */
    if (major == 4 && !mbit && bits(raw, 27, 2) == 3 && x6 <= 3) {
        if (!qp) return MERCED_OK;
        uint64_t va = gr_read(m, r3, &n3), pa;
        unsigned size = 1u << x6;
        bool clear = bits(raw, 29, 1) == 0;
        if (!va_translate(m, va, false, false, ISR_R, &pa, &st))
            return st;
        if (!alat_check(m, r1, pa, size, clear))
            gr_write(m, r1, phys_read(m, pa, size), 0);
        return MERCED_OK;
    }

    if (major == 4 && xbit && !mbit) {
        /* semaphores: cmpxchg (00-07), xchg (08-0B), fetchadd (12/13/16/17);
         * getf (1C-1F) shares this x=1 encoding space */
        if (!qp) return MERCED_OK;
        if (x6 >= 0x1C && x6 <= 0x1F)
            goto getf;
        warn_once(m, WARN_SEMAPHORE, "semaphore ops executed non-atomically");
        uint64_t va = gr_read(m, r3, &n3), pa;
        unsigned size = 1u << (x6 & 3);
        if (!va_translate(m, va, false, false, ISR_R | ISR_W, &pa, &st)) return st;
        if (x6 <= 0x07) {                       /* cmpxchg.acq/.rel */
            uint64_t old = phys_read(m, pa, size);
            uint64_t ccv = m->ar[AR_CCV];
            if (getenv("MERCED_DEBUG_XP_PTE_LOOP") &&
                m->ip >= UINT64_C(0xE00000008301B000) &&
                m->ip < UINT64_C(0xE00000008301C000)) {
                static unsigned xp_cmpxchg_debug;
                if (xp_cmpxchg_debug++ < 64)
                    fprintf(stderr, "merced: XP-CMPXCHG ip=%016" PRIX64
                            " r1=%u r2=%u r3=%u va=%016" PRIX64
                            " size=%u old=%016" PRIX64
                            " ccv=%016" PRIX64 " new=%016" PRIX64
                            " match=%u ninsts=%" PRIu64 "\n",
                            m->ip, r1, r2, r3, va, size, old, ccv,
                            gr_read(m, r2, &n2), old == ccv, m->ninsts);
            }
            if (target_trap_slot_armed) {
                static unsigned target_cmpxchg_debug;
                if (target_cmpxchg_debug++ < 64)
                    fprintf(stderr, "merced: TARGET-CMPXCHG ip=%016" PRIX64
                            " va=%016" PRIX64 " pa=%016" PRIX64
                            " size=%u old=%016" PRIX64
                            " ccv=%016" PRIX64 " new=%016" PRIX64
                            " match=%u ninsts=%" PRIu64 "\n",
                            m->ip, va, pa, size, old, ccv,
                            gr_read(m, r2, &n2), old == ccv, m->ninsts);
            }
            /* The loaded 1/2/4-byte value is zero-extended, then compared
             * against the full 64-bit ar.ccv.  Truncating ar.ccv makes a
             * compare spuriously succeed when only its low word matches,
             * which can turn NT's failed lock-free update into a write. */
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
            /* Register postincrement operands are read as inputs before the
             * load writes r1.  In particular r1 may alias r2; re-reading r2
             * afterward would use the loaded value as the increment. */
            uint64_t reg_inc = 0;
            uint8_t reg_inc_nat = 0;
            if (major == 4 && mbit)
                reg_inc = gr_read(m, r2, &reg_inc_nat);
            int spec = (type == 1 || type == 3);   /* .s / .sa */
            bool debug_handler_spec = false;
            if (spec && getenv("MERCED_DEBUG_XP_SPEC") &&
                m->ip >= UINT64_C(0xE000000083018000) &&
                m->ip < UINT64_C(0xE000000083020000)) {
                static unsigned n;
                debug_handler_spec = n++ < 256;
                if (debug_handler_spec)
                    fprintf(stderr, "merced: XP-SPEC ip=%016" PRIX64
                            " x6=%02X r1=r%u r3=r%u va=%016" PRIX64
                            "/nat%u dcr=%016" PRIX64 " psr=%016" PRIX64
                            " ninsts=%" PRIu64 "\n", m->ip, x6, r1, r3,
                            va, !!n3, m->cr[CR_DCR], m->psr, m->ninsts);
            }
            if (x6 == 0x1B) size = 8;              /* ld8.fill */
            /* An ordinary load consumes a NaT address as a non-access
             * register NaT fault.  Speculative loads defer it by producing
             * a NaT destination, just like any other deferrable fault. */
            if (n3 && !spec)
                return deliver_fault(m, VEC_NAT,
                                     ISR_NA | ISR_R | UINT64_C(0x10),
                                     0, false);
            if (n3) {
                gr_write(m, r1, 0, 1);
                if (debug_handler_spec)
                    fprintf(stderr, "merced: XP-SPEC result=nat-address\n");
            } else if (!va_translate(m, va, false, spec, ISR_R, &pa, &st)) {
                if (st != MERCED_OK) return st;
                gr_write(m, r1, 0, 1);             /* NaT on deferred spec load */
                if (debug_handler_spec)
                    fprintf(stderr, "merced: XP-SPEC result=deferred-nat\n");
            } else {
                bool check = x6 >= 0x20 && x6 <= 0x2B;
                bool clear = x6 >= 0x20 && x6 <= 0x23;
                bool alat_hit = check && alat_check(m, r1, pa, size, clear);
                if (check && getenv("MERCED_DEBUG_XP_PTE_LOOP") &&
                    (m->ip & ~UINT64_C(0xF)) ==
                        UINT64_C(0xE00000008301BE20)) {
                    static unsigned xp_check_debug;
                    if (xp_check_debug++ < 64)
                        fprintf(stderr, "merced: XP-CHECK r%u old=%016" PRIX64
                            " va=%016" PRIX64 " pa=%016" PRIX64
                            " size=%u hit=%u clear=%u ninsts=%" PRIu64 "\n",
                            r1, gr_read(m, r1, NULL), va, pa, size,
                            alat_hit, clear, m->ninsts);
                    if (xp_check_debug <= 64)
                        fflush(stderr);
                }
                if (!check || !alat_hit) {
                    uint64_t v = phys_read(m, pa, size);
                    uint8_t nat = 0;
                    static unsigned target_load_debug;
                    if (target_trap_slot_armed &&
                        (m->ip & ~UINT64_C(0xF)) ==
                            UINT64_C(0xE000000083083850) &&
                        target_load_debug++ < 8) {
                        fprintf(stderr, "merced: TARGET-LOAD raw=%011" PRIX64
                                " r1=%u r3=%u va=%016" PRIX64
                                " pa=%016" PRIX64 " v=%016" PRIX64
                                " bof=%u cfm=%016" PRIX64
                                " ninsts=%" PRIu64 "\n",
                                raw, r1, r3, va, pa, v, m->bof, m->cfm,
                                m->ninsts);
                        fflush(stderr);
                    }
                    if (x6 == 0x1B)                /* ld8.fill: NaT from UNAT */
                        nat = (uint8_t)((m->ar[AR_UNAT] >> ((va >> 3) & 0x3F)) & 1);
                    gr_write(m, r1, v, nat);
                    if (debug_handler_spec)
                        fprintf(stderr, "merced: XP-SPEC result=value=%016"
                                PRIX64 " pa=%016" PRIX64 "\n", v, pa);
                    /* ld.a and ld.sa establish the value/address
                     * association consumed by a later ld.c. */
                    if ((x6 >= 0x08 && x6 <= 0x0F) ||
                        (x6 >= 0x28 && x6 <= 0x2B))
                        alat_set(m, r1, pa, size);
                }
            }
            /* base update */
            if (major == 5) {
                uint64_t base = gr_read(m, r3, &n3);
                gr_write(m, r3, base + (uint64_t)imm9, n3);
            } else if (mbit) {
                uint64_t base = gr_read(m, r3, &n3);
                gr_write(m, r3, base + reg_inc, n3 | reg_inc_nat);
            }
            return MERCED_OK;
        }

        if ((x6 >= 0x30 && x6 <= 0x37) || x6 == 0x3B) {   /* stores */
            if (!qp) return MERCED_OK;
            uint64_t va = gr_read(m, r3, &n3), pa;
            uint64_t reg_inc = 0;
            uint8_t reg_inc_nat = 0;
            if (major == 4 && mbit)
                reg_inc = gr_read(m, r1, &reg_inc_nat);
            if (x6 == 0x3B) size = 8;              /* st8.spill */
            if (n3)
                return deliver_fault(m, VEC_NAT,
                                     ISR_NA | ISR_W | UINT64_C(0x10),
                                     0, false);
            /* st8.spill is the one store which preserves a NaT source in
             * ar.unat; every ordinary store consumes it. */
            uint64_t v = gr_read(m, r2, &n2);
            if (x6 != 0x3B && n2)
                return deliver_fault(m, VEC_NAT,
                                     ISR_W | UINT64_C(0x10), 0, false);
            if (!va_translate(m, va, false, false, ISR_W, &pa, &st)) return st;
            if (x6 == 0x3B) {
                unsigned bit = (unsigned)((va >> 3) & 0x3F);
                if (n2) m->ar[AR_UNAT] |= 1ull << bit;
                else    m->ar[AR_UNAT] &= ~(1ull << bit);
            }
            phys_write(m, pa, v, size);
            if (major == 5)
                gr_write(m, r3, va + (uint64_t)imm9, n3);
            else if (mbit)
                gr_write(m, r3, va + reg_inc, n3 | reg_inc_nat);
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
        if (x6 >= 0x2C && x6 <= 0x2F) {
            /*
             * lfetch is only a hint, but the .fault forms (x6 2e/2f) must
             * perform the complete data-translation check unless psr.ed
             * defers it.  NT uses these forms in memory-management paths;
             * treating them all as nops hides misses and NaTPage faults.
             */
            uint64_t base = gr_read(m, r3, &n3), ignored_pa;
            if (qp && x6 >= 0x2E && !(m->psr & PSR_ED)) {
                uint64_t access = ISR_NA | ISR_R | UINT64_C(4);
                if (n3)
                    return deliver_fault(m, VEC_NAT, access, base, true);
                if (!va_translate(m, base, false, false, access,
                                  &ignored_pa, &st))
                    return st;
            }
            if (qp && major == 7) {                /* imm base update form */
                int64_t i9 = sext((bits(raw, 36, 1) << 8) |
                                  (bits(raw, 27, 1) << 7) | bits(raw, 13, 7), 9);
                gr_write(m, r3, base + (uint64_t)i9, n3);
            } else if (qp && mbit) {
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
        if (!va_translate(m, va, false, false, is_st ? ISR_W : ISR_R,
                          &pa, &st)) return st;
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

static bool tlb_payload_overlaps(uint64_t entry_start, uint64_t entry_end,
                                 uint64_t va, uint64_t len) {
    const uint64_t mask = (UINT64_C(1) << 61) - 1;
    uint64_t start = entry_start & mask;
    uint64_t end = entry_end & mask;
    uint64_t purge_start = va & mask;
    uint64_t purge_end = purge_start + len - 1;

    /* A TC/TR tag contains the RID and VPN, not the virtual region number.
     * Match the same 61-bit payload tlb_search() uses.  A very large purge
     * can wrap at the payload boundary and therefore covers two intervals. */
    if (len == 0 || len > (UINT64_C(1) << 61))
        return true;
    if (purge_end <= mask)
        return start <= purge_end && purge_start <= end;
    return start <= (purge_end & mask) || purge_start <= end;
}

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
    uint32_t rid = (uint32_t)((m->rr[vrn] >> 8) & 0xFFFFFFull);
    uint64_t new_start = ifa & page;
    uint64_t new_end = new_start + (ps >= 64 ? ~0ull : (1ull << ps) - 1);
    uint64_t new_len = ps >= 61 ? (UINT64_C(1) << 61) : (UINT64_C(1) << ps);

    /* An ITC insertion replaces any overlapping TC translation for the
     * same region ID.  Keeping both is not harmless: lookup order would
     * keep selecting an older VHPT-backing page after NT installed its
     * corrected mapping, producing a permanent not-present fault. */
    MercedTlbEntry *tc = instruction ? m->itc : m->dtc;
    for (unsigned i = 0; i < MERCED_N_TC; i++) {
        if (&tc[i] != e && tc[i].valid && tc[i].rid == rid &&
            tlb_payload_overlaps(tc[i].va_start, tc[i].va_end,
                                 new_start, new_len))
            tc[i].valid = 0;
    }
    /* TC occupancy and PTE.p are distinct architectural state.  A VHPT
     * walk may cache a non-present translation; subsequent references hit
     * that TC entry and raise Page Not Present.  Conflating P=0 with an
     * unused slot made probes and software refill logic observe a TLB miss
     * instead of the translation Windows actually published. */
    e->valid = 1;
    e->pending_purge = 0;
    e->rid = rid;
    e->va_start = new_start;
    e->va_end = new_end;
    e->pfn_base = (pte & 0x0003FFFFFFFFF000ull) & page;
    e->ps = (uint8_t)ps;
    e->itir = itir;
    e->pte = pte;
    if (getenv("MERCED_DEBUG_REGION4") &&
        (vrn == 4 || (rid == ((m->rr[4] >> 8) & 0xFFFFFFu) &&
                      new_start <= UINT64_C(0x802500) &&
                      UINT64_C(0x802500) <= new_end))) {
        fprintf(stderr, "merced: R4-XLATE install ip=%016" PRIX64
                " va=%016" PRIX64 "-%016" PRIX64
                " pa=%016" PRIX64 " ps=%u rid=%06X pte=%016" PRIX64
                " kind=%c ninsts=%" PRIu64 "\n",
                m->ip, new_start, new_end, e->pfn_base, ps, rid, pte,
                instruction ? 'I' : 'D', m->ninsts);
    }
    static unsigned mmu_debug_inserts;
    if (getenv("MERCED_MMU_DEBUG") && mmu_debug_inserts++ < 2048) {
        const char *kind = instruction ? "ITC" : "DTC";
        unsigned index = 0;
        for (unsigned i = 0; i < MERCED_N_ITR; i++)
            if (e == &m->itr[i]) { kind = "ITR"; index = i; break; }
        for (unsigned i = 0; i < MERCED_N_DTR; i++)
            if (e == &m->dtr[i]) { kind = "DTR"; index = i; break; }
        fprintf(stderr, "merced: XLATE install %s[%u] ip=%016" PRIX64
                " va=%016" PRIX64 " pa=%016" PRIX64
                " ps=%u rid=%06X pte=%016" PRIX64 " valid=%u"
                " ninsts=%" PRIu64 "\n",
                kind, index, m->ip, e->va_start, e->pfn_base,
                ps, e->rid, pte, e->valid, m->ninsts);
    }
    if (merced_dbg() && tlb_debug_events < TLB_DEBUG_MAX) {
        tlb_debug_events++;
        fprintf(stderr, "merced: insert %c ip=%016" PRIX64
                " va=%016" PRIX64 " pa=%016" PRIX64 " ps=%u rid=%06X"
                " pte=%016" PRIX64 " valid=%u\n",
                instruction ? 'I' : 'D', m->ip, e->va_start, e->pfn_base,
                ps, e->rid, pte, e->valid);
    }
}

static void tlb_cache_replaced_tr(Merced *m, const MercedTlbEntry *tr,
                                  bool instruction) {
    if (!tr->valid)
        return;

    MercedTlbEntry *tc = instruction ? m->itc : m->dtc;
    uint32_t *next = instruction ? &m->itc_next : &m->dtc_next;
    tc[(*next)++ % MERCED_N_TC] = *tr;
}

/* Reads a VHPT hash-table slot's home address. VHPT reads are always data
 * references, translated through the same TR/TC state (or the same
 * physical/pinned-window carve-outs) va_translate() itself uses for any
 * other data access. Deliberately does not recurse into vhpt_walk() - real
 * hardware requires its VHPT backing store to already be resident rather
 * than chasing it through another level of walking - and never raises a
 * fault on a miss; the caller treats "can't reach the hash table" the same
 * as "walk found nothing", falling back to the pre-existing software path. */
enum {
    VHPT_ENTRY_TLB_MISS = 0,
    VHPT_ENTRY_TRANSLATED = 1,
    VHPT_ENTRY_ABORT = -1,
};

/* SAL/PAL firmware keeps its 1 MiB runtime image identity-addressable while
 * it owns the IVT (and while executing inside that image during a handoff).
 * The hardware VHPT walker uses the same carve-out when reading the table.
 * This mirrors ia64_firmware_identity_pa() in the reference model. */
static bool firmware_identity_pa(Merced *m, uint64_t va, uint64_t *pa) {
    const uint64_t base = UINT64_C(0x00100000);
    const uint64_t end = UINT64_C(0x00200000);
    const uint64_t payload_mask = (UINT64_C(1) << 61) - 1;
    unsigned cpl = (unsigned)((m->psr >> PSR_CPL_SHIFT) & 3);
    bool firmware_iva = m->cr[CR_IVA] == 0 ||
                        m->cr[CR_IVA] == UINT64_C(0x00010000);
    bool firmware_ip = m->ip >= base && m->ip < end;
    bool ami_firmware_ip = m->ip >= UINT64_C(0x7F000000) &&
                           m->ip < UINT64_C(0x80000000);

    /* The i2000's 460GX encodes its high DRAM card/aperture selector in
     * physical bits 43:32.  AMI places the RSE backing store in the top
     * 64 MiB of that aperture.  It must remain physically addressable even
     * with interruption collection disabled inside a miss handler; the
     * later pre-ExitBootServices fallback cannot recover a nested miss. */
    if (cpl == 0 && firmware_iva &&
        (va & UINT64_C(0x00000fff00000000)) ==
            UINT64_C(0x00000e0000000000) &&
        (uint32_t)va >= UINT32_C(0x7c000000) &&
        (uint32_t)va < UINT32_C(0x80000000)) {
        *pa = va;
        return true;
    }

    /* AMI's FMM physical-access primitive zero-extends a 32-bit platform
     * address and ORs in region 4 (the uncacheable physical alias) before
     * loading it.  While SAL owns the region-0 IVT these references are
     * firmware physical accesses, even if psr.dt is set.  Sending them
     * through the guest TLB eventually strands the firmware at vector 0xc00
     * when it probes 0xc0000024, accumulating billions of nested faults.
     * Keep this strictly within the firmware-owned, CPL0 phase; an OS with
     * its own IVA continues through the normal region/TLB translation path. */
    if (cpl == 0 && (firmware_iva || ami_firmware_ip) &&
        (va >> 61) == 4 &&
        (va & payload_mask) <= UINT32_MAX) {
        *pa = va & UINT32_MAX;
        return true;
    }

    if (cpl == 0 && (firmware_iva || firmware_ip) &&
        va >= base && va < end) {
        *pa = va;
        return true;
    }
    return false;
}

static int vhpt_entry_pa(Merced *m, uint64_t entry_va, uint64_t *pa) {
    if (firmware_identity_pa(m, entry_va, pa))
        return VHPT_ENTRY_TRANSLATED;
    if (!(m->psr & PSR_DT)) {
        *pa = entry_va & MERCED_PHYS_MASK;
        return VHPT_ENTRY_TRANSLATED;
    }
    unsigned vrn = (unsigned)(entry_va >> 61);
    uint32_t rid = (uint32_t)((m->rr[vrn] >> 8) & 0xFFFFFFull);
    const MercedTlbEntry *e = tlb_search(m->dtr, MERCED_N_DTR, rid, entry_va,
                                         &m->dtr_hint);
    if (!e) e = tlb_search(m->dtc, MERCED_N_TC, rid, entry_va, &m->dtc_hint);
    if (!e) return VHPT_ENTRY_TLB_MISS;

    /*
     * A VHPT walker reads its table through the data-translation hierarchy
     * at CPL 0.  Finding a TC entry is not sufficient: a non-present,
     * inaccessible, unaccessed, keyed-off, or non-WB mapping aborts the
     * hardware walk and falls back to the original TLB-miss vector.  It must
     * not be used to fetch a bogus PTE, and it is not a VHPT Translation
     * fault (that vector is reserved for an actual miss translating the
     * table address).  This matches ia64_vhpt_entry_phys() in reference QEMU.
     */
    uint64_t pte = e->pte;
    unsigned ma = (unsigned)((pte >> PTE_MA_SHIFT) & 7);
    unsigned perm = tlb_effective_perm(
        (unsigned)((pte >> PTE_AR_SHIFT) & 7),
        (unsigned)((pte >> PTE_PL_SHIFT) & 3), 0);
    uint32_t key = (uint32_t)((e->itir >> 8) & 0xFFFFFFull);
    uint32_t key_fault = (m->psr & PSR_PK)
        ? key_fault_vector(m, key, PERM_R, false) : 0;
    if (!(pte & PTE_PRESENT) || ma != 0 || !(perm & PERM_R) ||
        (!(pte & PTE_ACCESSED) && !(m->psr & PSR_DA)) || key_fault) {
        if (getenv("MERCED_MMU_DEBUG")) {
            static unsigned vhpt_abort_debug;
            static unsigned vhpt_target_abort_debug;
            bool target = (entry_va & UINT64_C(0xFFFFFFFFFFFFE000)) ==
                          UINT64_C(0xFFF80000597CC000);
            if (vhpt_abort_debug++ < 64 ||
                (target && vhpt_target_abort_debug++ < 8))
                fprintf(stderr, "merced: VHPT backing abort entry=%016" PRIX64
                        " pte=%016" PRIX64 " itir=%016" PRIX64
                        " ma=%u perm=%u a=%u da=%u pk=%u key=%06X kvec=%X\n",
                        entry_va, pte, e->itir, ma, perm,
                        !!(pte & PTE_ACCESSED), !!(m->psr & PSR_DA),
                        !!(m->psr & PSR_PK), key, key_fault);
        }
        return VHPT_ENTRY_ABORT;
    }

    *pa = (e->pfn_base + (entry_va - e->va_start)) & MERCED_PHYS_MASK;
    return VHPT_ENTRY_TRANSLATED;
}

static uint64_t vhpt_load_u64(Merced *m, uint64_t pa) {
    uint64_t value = phys_read(m, pa, 8);
    return (m->cr[CR_DCR] & (UINT64_C(1) << 1))
         ? __builtin_bswap64(value) : value;
}

/* Hardware-style VHPT walk, tried on a TLB miss before the software fault
 * path: hash the faulting VA into the OS-managed VHPT and, on a genuine
 * hit, install a TC entry via the same tlb_insert() a software itc.d/itc.i
 * would have used. Hash addressing and the long-format tag match
 * reference/qemu-system-ia64/target/ia64/op_helper.c's
 * ia64_vhpt_{short,long}_hash_address()/ia64_vhpt_long_tag() exactly.
 *
 * Safe by construction even if some corner of the math below is subtly
 * off: every failure mode here (wrong hash address, tag mismatch,
 * unreadable hash table, PTE not present) just returns false, and the
 * caller falls through to the software TLB-miss fault path that ran
 * unconditionally before this walker existed. A bug here can only cost
 * the speedup - it can't install a translation the OS didn't itself write
 * into its own VHPT. */
static uint64_t dbg_vhpt_calls, dbg_vhpt_disabled, dbg_vhpt_hit,
                dbg_vhpt_unmapped, dbg_vhpt_tagfail, dbg_vhpt_np;

/* Temporary diagnostic accessor (see machine_generic_ia64.c's "vhptstats"
 * monitor command) for measuring how often the hardware VHPT walk above
 * actually resolves a miss vs. falling back to the software path - remove
 * both once that question is answered. */
void merced_vhpt_stats(uint64_t *calls, uint64_t *disabled, uint64_t *hit,
                       uint64_t *unmapped, uint64_t *tagfail, uint64_t *np) {
    *calls = dbg_vhpt_calls; *disabled = dbg_vhpt_disabled;
    *hit = dbg_vhpt_hit; *unmapped = dbg_vhpt_unmapped;
    *tagfail = dbg_vhpt_tagfail; *np = dbg_vhpt_np;
}

static uint64_t vhpt_hash_address(Merced *m, uint64_t va) {
    uint64_t pta = m->cr[CR_PTA];
    unsigned vrn = (unsigned)(va >> 61);
    uint64_t rr = m->rr[vrn];
    unsigned size = (unsigned)((pta >> 2) & 0x3F);
    bool long_format = (pta & (1ull << 8)) != 0;
    unsigned rr_ps;

    /* PTA.ve and RR.ve gate only the hardware walker (see vhpt_walk).  The
     * software-visible thash/ttag instructions and cr.iha keep using the
     * configured PTA base, size and format even with the walker switched
     * off - an OS that computes a VHPT pointer while servicing its own
     * misses must get the real address, not the input va back. */
    if (size < 15 || size > (long_format ? 61u : 52u))
        return va;                            /* PTA misconfigured: no hash */

    rr_ps = (unsigned)((rr >> 2) & 0x3F);
    unsigned ps = rr_ps < 12 ? 12 : rr_ps;    /* region's preferred page size */

    /* IA64_IMPL_VA_MSB=60 in the reference model: the full 61-bit payload
     * below the 3-bit region field is implemented, so there are no
     * unimplemented bits to sign-extend/validate here. */
    uint64_t payload = va & ((1ull << 61) - 1);
    uint64_t hpn = payload >> ps;
    uint32_t rid = (uint32_t)((rr >> 8) & 0xFFFFFFull);

    if (!long_format) {
        uint64_t region = va & (0x7ull << 61);
        uint64_t offset = hpn << 3;
        uint64_t mask = (1ull << size) - 1;
        uint64_t base = pta & (((1ull << 61) - 1) & ~0x7FFFull);
        return region | ((base & ~mask) | (offset & mask));
    }
    {
        uint64_t base = pta & ~0x7FFFull;
        uint64_t entries = 1ull << (size - 5);
        uint64_t hash = (hpn ^ (hpn >> 7) ^ rid) & (entries - 1);
        uint64_t offset = hash << 5;
        uint64_t mask = (1ull << size) - 1;
        return (base & ~mask) | (offset & mask);
    }
}

static int vhpt_walk(Merced *m, uint64_t va, bool ifetch) {
    dbg_vhpt_calls++;
    uint64_t pta = m->cr[CR_PTA];
    if (!(pta & 1)) {
        dbg_vhpt_disabled++;
        return VHPT_MISS;                    /* pta.ve=0: walker off */
    }
    unsigned vrn = (unsigned)(va >> 61);
    uint64_t rr = m->rr[vrn];
    if (!(rr & 1)) {
        dbg_vhpt_disabled++;
        return VHPT_MISS;                    /* rr.ve=0: walker off here */
    }

    unsigned size = (unsigned)((pta >> 2) & 0x3F);
    bool long_format = (pta & (1ull << 8)) != 0;
    unsigned rr_ps = (unsigned)((rr >> 2) & 0x3F);
    unsigned ps = rr_ps < 12 ? 12 : rr_ps;    /* region's preferred page size */

    /* Merced implements 4/8/16 KiB and then power-of-four page sizes
     * through 256 MiB.  An unsupported RR.ps disables the hardware walk;
     * it must not be rounded into a different, apparently valid mapping. */
    if (!(ps == 12 || ps == 13 || ps == 14 ||
          (ps >= 16 && ps <= 28 && !(ps & 1))))
        return VHPT_MISS;

    /* IA64_IMPL_VA_MSB=60 in the reference model: the full 61-bit payload
     * below the 3-bit region field is implemented, so there are no
     * unimplemented bits to sign-extend/validate here. */
    uint64_t payload = va & ((1ull << 61) - 1);
    uint64_t hpn = payload >> ps;
    uint32_t rid = (uint32_t)((rr >> 8) & 0xFFFFFFull);

    if (long_format && size < 5)
        return VHPT_MISS;                    /* table smaller than one entry */
    uint64_t entry_va = vhpt_hash_address(m, va);

    uint64_t entry_pa;
    int entry_status = vhpt_entry_pa(m, entry_va, &entry_pa);
    if (entry_status != VHPT_ENTRY_TRANSLATED) {
        static bool b520_vhpt_dumped;
        if (!b520_vhpt_dumped && getenv("MERCED_DEBUG_B520_FAULT") &&
            (m->ip & ~UINT64_C(0xF)) ==
                UINT64_C(0xE00000008355B520)) {
            b520_vhpt_dumped = true;
            uint32_t entry_rid = (uint32_t)
                ((m->rr[entry_va >> 61] >> 8) & 0xFFFFFFull);
            fprintf(stderr, "merced: B520-VHPT va=%016" PRIX64
                    " entry_va=%016" PRIX64 " status=%d"
                    " pta=%016" PRIX64 " rr=%016" PRIX64
                    " entry_rid=%06X\n",
                    va, entry_va, entry_status, pta, rr, entry_rid);
            for (unsigned i = 0; i < MERCED_N_DTR; i++) {
                const MercedTlbEntry *x = &m->dtr[i];
                if (x->valid)
                    fprintf(stderr, "merced: B520-DTR[%u] rid=%06X"
                            " va=%016" PRIX64 "-%016" PRIX64
                            " pa=%016" PRIX64 " pte=%016" PRIX64
                            " itir=%016" PRIX64 "\n",
                            i, x->rid, x->va_start, x->va_end, x->pfn_base,
                            x->pte, x->itir);
            }
            for (unsigned i = 0; i < MERCED_N_TC; i++) {
                const MercedTlbEntry *x = &m->dtc[i];
                if (x->valid && (x->rid == entry_rid ||
                                 (x->va_start <= entry_va &&
                                  entry_va <= x->va_end)))
                    fprintf(stderr, "merced: B520-DTC[%u] rid=%06X"
                            " va=%016" PRIX64 "-%016" PRIX64
                            " pa=%016" PRIX64 " pte=%016" PRIX64
                            " itir=%016" PRIX64 "\n",
                            i, x->rid, x->va_start, x->va_end, x->pfn_base,
                            x->pte, x->itir);
            }
            fflush(stderr);
        }
        static unsigned mmu_debug_unmapped;
        if ((m->ip >> 61) == 7 && getenv("MERCED_MMU_DEBUG") &&
            mmu_debug_unmapped++ < 512)
            fprintf(stderr, "merced: VHPT inaccessible ip=%016" PRIX64
                    " va=%016" PRIX64 " entry=%016" PRIX64
                    " pta=%016" PRIX64 " rr=%016" PRIX64 " status=%d\n",
                    m->ip, va, entry_va, pta, rr, entry_status);
        if (entry_status == VHPT_ENTRY_ABORT)
            return VHPT_MISS;
        dbg_vhpt_unmapped++;
        return VHPT_TRANSLATION;
    }

    uint64_t pte, itir;
    if (va == UINT64_C(0xE000010600000000))
        target_vhpt_entry_pa = entry_pa;
    if (va >= watch_va_base && va < watch_va_end) {
        static unsigned walk_dbg;
        if (walk_dbg++ < 12) {
            fprintf(stderr, "merced: VHPT-WALK va=%016" PRIX64
                    " entry_va=%016" PRIX64 " entry_pa=%016" PRIX64
                    " pte=%016" PRIX64 " pta=%016" PRIX64 " ninsts=%"
                    PRIu64 "\n", va, entry_va, entry_pa,
                    phys_read(m, entry_pa, 8), pta, m->ninsts);
            fflush(stderr);
        }
    }
    if (!long_format) {
        pte = vhpt_load_u64(m, entry_pa);
        /* Short-format entries imply both page size and protection key from
         * the region register.  The RR's RID occupies ITIR.key for the
         * cached translation (and is what TAK must return). */
        itir = ((uint64_t)ps << 2) | ((uint64_t)rid << 8);
    } else {
        pte = vhpt_load_u64(m, entry_pa);
        itir = vhpt_load_u64(m, entry_pa + 8);
        uint64_t tag = vhpt_load_u64(m, entry_pa + 16);
        unsigned hpn_bits = ps > 60 ? 0 : 61 - ps;
        uint64_t expected_tag = hpn_bits
            ? (((uint64_t)rid << hpn_bits) | (hpn & ((1ull << hpn_bits) - 1)))
            : rid;
        if ((tag & (1ull << 63)) || tag != expected_tag) {
            dbg_vhpt_tagfail++;
            return VHPT_MISS; /* collision slot / stale memory, not our entry */
        }
    }
    if (!(pte & 1)) {
        dbg_vhpt_np++;
        static unsigned vhpt_np_debug;
        bool sample_np = dbg_vhpt_np &&
                         !(dbg_vhpt_np & (dbg_vhpt_np - 1));
        if (getenv("MERCED_MMU_DEBUG") &&
            (vhpt_np_debug++ < 128 || sample_np))
            fprintf(stderr, "merced: XLATE VHPT not-present ip=%016" PRIX64
                    " va=%016" PRIX64
                    " entry_va=%016" PRIX64 " entry_pa=%016" PRIX64
                    " pte=%016" PRIX64 " ps=%u rid=%06X"
                    " ninsts=%" PRIu64 "\n",
                    m->ip, va, entry_va, entry_pa, pte, ps, rid, m->ninsts);
        uint64_t save_ifa = m->cr[CR_IFA], save_itir = m->cr[CR_ITIR];
        m->cr[CR_IFA] = va;
        m->cr[CR_ITIR] = itir;
        tlb_insert(m, ifetch ? &m->itc[m->itc_next++ % MERCED_N_TC]
                            : &m->dtc[m->dtc_next++ % MERCED_N_TC],
                   pte, ifetch);
        m->cr[CR_IFA] = save_ifa;
        m->cr[CR_ITIR] = save_itir;
        /* An accessible VHPT entry with P=0 is architecturally cached and
         * raises Page Not Present, matching the reference IA-64 walker. */
        return VHPT_NOT_PRESENT;
    }
    dbg_vhpt_hit++;

    /* tlb_insert() reads CR_IFA/CR_ITIR implicitly, matching itc.d/itc.i's
     * own semantics; stage them, insert, then restore, so a successful
     * walk leaves the visible fault-context registers untouched exactly
     * like real hardware's walker does. */
    uint64_t save_ifa = m->cr[CR_IFA], save_itir = m->cr[CR_ITIR];
    m->cr[CR_IFA] = va;
    m->cr[CR_ITIR] = itir;
    tlb_insert(m, ifetch ? &m->itc[m->itc_next++ % MERCED_N_TC]
                        : &m->dtc[m->dtc_next++ % MERCED_N_TC],
              pte, ifetch);
    m->cr[CR_IFA] = save_ifa;
    m->cr[CR_ITIR] = save_itir;
    return VHPT_HIT;
}

static void tlb_purge(Merced *m, MercedTlbEntry *t, int n, uint32_t rid,
                      uint64_t va, uint64_t len) {
    for (int i = 0; i < n; i++) {
        if (t[i].valid && t[i].rid == rid &&
            tlb_payload_overlaps(t[i].va_start, t[i].va_end, va, len)) {
            if (getenv("MERCED_MMU_DEBUG"))
                fprintf(stderr, "merced: XLATE purge ip=%016" PRIX64
                        " va=%016" PRIX64 " len=%016" PRIX64
                        " rid=%06X hit=%016" PRIX64 "-%016" PRIX64
                        " ninsts=%" PRIu64 "\n",
                        m->ip, va, len, rid, t[i].va_start, t[i].va_end,
                        m->ninsts);
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

static void tlb_mark_purge(Merced *m, MercedTlbEntry *t, int n, uint32_t rid,
                           uint64_t va, uint64_t len) {
    for (int i = 0; i < n; i++)
        if (t[i].valid && t[i].rid == rid &&
            tlb_payload_overlaps(t[i].va_start, t[i].va_end, va, len) &&
            !t[i].pending_purge) {
            t[i].pending_purge = 1;
            m->tlb_purge_pending++;
        }
}

static void tlb_complete_pending(Merced *m, MercedTlbEntry *t, int n) {
    for (int i = 0; i < n; i++) {
        if (!t[i].pending_purge) continue;
        t[i].pending_purge = 0;
        t[i].valid = 0;
        m->tlb_purge_pending--;
    }
}

/* ptc.* (tlb_mark_purge) is rare; srlz.d/srlz.i/rfi (these) run on every
 * serialization point, which in some firmware spin loops is every slot.
 * m->tlb_purge_pending mirrors alat_valid_mask/ext_pending_count: skip the
 * up-to-512-entry scans below entirely when nothing is pending anywhere. */
static void tlb_serialize_data(Merced *m) {
    if (!m->tlb_purge_pending) return;
    tlb_complete_pending(m, m->dtr, MERCED_N_DTR);
    tlb_complete_pending(m, m->dtc, MERCED_N_TC);
}

static void tlb_serialize_instruction(Merced *m) {
    /* The cached fetch translation is valid only until the next instruction
     * serialization point.  Besides completing ptc/ptr, srlz.i makes earlier
     * changes to psr.it and region registers visible to instruction fetch.
     * Dropping it here lets merced_step() reuse the PA within a bundle while
     * preserving those architectural visibility rules. */
    m->bundle_cache_translation_valid = false;
    MercedTranslationCache *tc = m->translation_cache;
    if (tc && ++tc->translation_generation == 0)
        tb_flush(m);
    if (!m->tlb_purge_pending) return;
    tlb_complete_pending(m, m->itr, MERCED_N_ITR);
    tlb_complete_pending(m, m->itc, MERCED_N_TC);
}

/* ── RSE backing store ───────────────────────────────────────────────────── */

/* A NaT collection word occupies backing-store slot 63 of every 64-slot
 * (512-byte) group.  BSPSTORE may point anywhere in a group -- notably,
 * Windows' context switch path restores arbitrary saved BSPSTORE values --
 * so a group-count formula relative to an assumed aligned anchor is not
 * sufficient.  Walk in either direction and skip the actual RNAT slots.
 * The architectural file is only 96 registers, making this inexpensive. */
static bool rse_is_rnat_slot(uint64_t addr) {
    return (addr & 0x1F8ull) == 0x1F8ull;
}

static uint64_t rse_move_regs(uint64_t addr, int64_t regs) {
    while (regs > 0) {
        addr += 8;
        if (rse_is_rnat_slot(addr)) addr += 8;
        regs--;
    }
    while (regs < 0) {
        addr -= 8;
        if (rse_is_rnat_slot(addr)) addr -= 8;
        regs++;
    }
    return addr;
}

/* Count register slots (not RNAT words) in the loadrs byte window ending at
 * BSP.  loadrs is encoded in bytes precisely because that window can contain
 * a collection word. */
static int64_t rse_regs_in_window(uint64_t bsp, uint64_t bytes) {
    int64_t regs = 0;
    uint64_t words = bytes >> 3;
    while (words--) {
        bsp -= 8;
        if (!rse_is_rnat_slot(bsp)) regs++;
    }
    return regs;
}

static uint64_t rse_addr(Merced *m, int64_t regs_pos) {
    return rse_move_regs(m->rse_anchor_addr,
                         regs_pos - m->rse_anchor_regs);
}

/* Inverse of rse_addr(): the register-units position that a backing-store
 * address maps to under the current anchor.  Walks in either direction,
 * skipping RNAT collection slots, mirroring rse_move_regs(). */
static int64_t rse_addr_to_regs(Merced *m, uint64_t addr) {
    uint64_t a = m->rse_anchor_addr;
    int64_t regs = 0;
    while (a < addr) { a += 8; if (rse_is_rnat_slot(a)) a += 8; regs++; }
    while (a > addr) { a -= 8; if (rse_is_rnat_slot(a)) a -= 8; regs--; }
    return m->rse_anchor_regs + regs;
}

/* Maps an absolute register-units position (same axis as bof_total) to its
 * gr_stack slot. Valid as long as it's within MERCED_N_STACKED of the
 * current frame - i.e. as long as real hardware wouldn't itself have needed
 * to background-spill past the 96-register physical file (see the
 * merced.h simplifications note). */
static uint32_t rse_stack_slot(Merced *m, int64_t regs_pos) {
    int64_t rel = regs_pos - (int64_t)m->bof_total;
    int64_t idx = ((int64_t)m->bof + rel) % MERCED_RSE_CAPACITY;
    if (idx < 0) idx += MERCED_RSE_CAPACITY;
    return (uint32_t)idx;
}

/* Store dirty registers through (but not including) end.  This is shared by
 * explicit flushrs and the mandatory spill that real hardware performs when
 * frame growth would exceed the 96-entry physical stacked register file. */
static MercedStatus rse_store_through(Merced *m, int64_t end) {
    static unsigned dbg_store291;
    for (int64_t p = m->rse_flushed_regs; p < end; p++) {
        uint32_t idx = rse_stack_slot(m, p);
        uint64_t pa;
        MercedStatus st;
        if (!va_translate(m, rse_addr(m, p), false, false,
                          ISR_W | ISR_RS, &pa, &st))
            return st;
        if (getenv("MERCED_R36_DEBUG") && idx == 291 && dbg_store291++ < 64)
            fprintf(stderr, "merced: RSE store slot291 pos=%" PRId64
                    " va=%016" PRIX64 " value=%016" PRIX64 "\n",
                    p, rse_addr(m, p), m->gr_stack[idx]);
        uint64_t value = m->gr_stack[idx];
        if (m->ar[AR_RSC] & (UINT64_C(1) << 4))
            value = __builtin_bswap64(value);
        phys_write(m, pa, value, 8);
    }
    m->rse_flushed_regs = end;
    return MERCED_OK;
}

static MercedStatus rse_spill_excess(Merced *m) {
    int64_t bsp = (int64_t)m->bof_total + (int64_t)CFM_SOF(m->cfm);
    int64_t resident = bsp - m->rse_flushed_regs;
    if (resident <= MERCED_N_STACKED)
        return MERCED_OK;
    return rse_store_through(m, bsp - MERCED_N_STACKED);
}

/* flushrs: write every register from the last flushed position up to the
 * current ar.bsp (bof_total+sof) out to the backing store, then advance
 * ar.bspstore (rse_flushed_regs) to match. */
static MercedStatus rse_flush(Merced *m) {
    int64_t target = (int64_t)m->bof_total + (int64_t)CFM_SOF(m->cfm);
    return rse_store_through(m, target);
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
    static unsigned dbg_load291;
    int64_t bsp_regs = (int64_t)m->bof_total + (int64_t)CFM_SOF(m->cfm);
    uint64_t bsp_addr = rse_addr(m, bsp_regs);
    int64_t target = bsp_regs - rse_regs_in_window(bsp_addr, loadrs_bytes);
    /* Only the portion below the current BSPSTORE/load boundary needs a
     * memory fill.  Registers from that boundary through BSP are already
     * resident; the architecture merely reclassifies them as dirty.  Reading
     * the whole requested window destroys live interrupted frames because
     * those resident values need not have been eagerly spilled yet. */
    int64_t load_end = m->rse_flushed_regs < bsp_regs
                     ? m->rse_flushed_regs : bsp_regs;
    for (int64_t p = target; p < load_end; p++) {
        uint32_t idx = rse_stack_slot(m, p);
        uint64_t pa;
        MercedStatus st;
        if (!va_translate(m, rse_addr(m, p), false, false,
                          ISR_R | ISR_RS, &pa, &st))
            return st;
        uint64_t value = phys_read(m, pa, 8);
        if (m->ar[AR_RSC] & (UINT64_C(1) << 4))
            value = __builtin_bswap64(value);
        if (getenv("MERCED_R36_DEBUG") && idx == 291 && dbg_load291++ < 64)
            fprintf(stderr, "merced: RSE load slot291 pos=%" PRId64
                    " va=%016" PRIX64 " value=%016" PRIX64 "\n",
                    p, rse_addr(m, p), value);
        m->gr_stack[idx] = value;
        m->nat_stack[idx] = 0;   /* NaT collection words not modeled */
    }
    m->rse_flushed_regs = target;
    return MERCED_OK;
}

/* ── PAL procedures ──────────────────────────────────────────────────────
 *
 * Firmware reaches PAL through a "break.m 0x100000; br.many b0" trampoline
 * whose break the processor model intercepts directly, exactly as
 * reference/qemu-system-ia64 does (target/ia64/op_helper.c:
 * helper_pal_dispatch).  The values below are that model's, for a Merced
 * (800 MHz / 133 MHz bus) part: real architectural answers, not
 * placeholders, so an OS configures its MMU, RSE, caches and performance
 * monitors from facts instead of guessing after a NOT_IMPLEMENTED failure.
 *
 * Statuses are the PAL-defined negative codes. */
#define PAL_OK           UINT64_C(0)
#define PAL_NOT_IMPL     ((uint64_t)-1)
#define PAL_INVALID_ARG  ((uint64_t)-2)
#define PAL_ERROR        ((uint64_t)-3)
#define PAL_NO_INFO      ((uint64_t)-6)
#define PAL_BEYOND_MAX   ((uint64_t)-8)
#define PAL_NEXT_HIGHER  UINT64_C(1)

/* Merced predates PAL_PREFETCH_VIS, PAL_LOGICAL_TO_PHYSICAL,
 * PAL_CACHE_SHARED_INFO and PAL_BRAND_INFO (SDM 245318-002 §11.8), so those
 * report NOT_IMPLEMENTED rather than inventing multi-core/brand answers a
 * single-thread Itanium cannot back up. */
#define PAL_HAS_POST_MERCED 0

/* Page sizes itr/itc can insert, and the extra 4 GiB size ptr can purge. */
#define PAL_INSERTABLE_PS_MASK                                            \
    ((UINT64_C(1) << 12) | (UINT64_C(1) << 13) | (UINT64_C(1) << 14) |    \
     (UINT64_C(1) << 16) | (UINT64_C(1) << 18) | (UINT64_C(1) << 20) |    \
     (UINT64_C(1) << 22) | (UINT64_C(1) << 24) | (UINT64_C(1) << 26) |    \
     (UINT64_C(1) << 28) | (UINT64_C(1) << 30))
#define PAL_PURGEABLE_PS_MASK (PAL_INSERTABLE_PS_MASK | (UINT64_C(1) << 32))

#define PAL_IMPL_PA_BITS   50
#define PAL_LOCAL_SAPIC_PA UINT64_C(0x00000000FEE00000)
#define PAL_IO_BLOCK_PA    UINT64_C(0x000080000C000000)

typedef struct {
    bool     unified;
    uint8_t  attribute, associativity, line_shift, stride_shift;
    uint8_t  store_latency, load_latency, tag_lsb, tag_msb;
    uint32_t size;
} PalCacheInfo;

/* The cache hierarchy PAL_CACHE_INFO / PAL_CACHE_PROT_INFO describe.  Merced
 * has a split L1 (level 0) but unified L2/L3, so a level-1 query is only
 * meaningful for the data/unified type. */
static bool pal_cache_model(uint64_t level, uint64_t type, PalCacheInfo *ci) {
    if (type < 1 || type > 2 || level >= 3)
        return false;
    memset(ci, 0, sizeof(*ci));
    ci->tag_msb = PAL_IMPL_PA_BITS - 1;
    switch (level) {
    case 0:
        ci->associativity = 4;
        ci->line_shift = ci->stride_shift = 6;
        ci->store_latency = (type == 1) ? 0xFF : 1;
        ci->load_latency = 1;
        ci->tag_lsb = 12;
        ci->size = 16u << 10;
        return true;
    case 1:
        if (type != 2)
            return false;
        ci->unified = true;
        ci->attribute = 1;
        ci->associativity = 8;
        ci->line_shift = ci->stride_shift = 7;
        ci->store_latency = 1;
        ci->load_latency = 5;
        ci->tag_lsb = 15;
        ci->size = 256u << 10;
        return true;
    default:                                   /* level 2 */
        if (type != 2)
            return false;
        ci->unified = true;
        ci->attribute = 1;
        ci->associativity = 12;
        ci->line_shift = ci->stride_shift = 7;
        ci->store_latency = 1;
        ci->load_latency = 12;
        ci->tag_lsb = 18;
        ci->size = 3u << 20;
        return true;
    }
}

/* PAL buffer arguments are ordinary data references: honour the MMU so a
 * caller running with PSR.dt set gets the same translation any store would,
 * rather than scribbling on the physical address. */
static bool pal_store8(Merced *m, uint64_t va, uint64_t v) {
    uint64_t pa;
    MercedStatus st;
    if (!va_translate(m, va, false, false, ISR_R | ISR_W, &pa, &st))
        return false;
    phys_write(m, pa, v, 8);
    return true;
}

/* A level_index names exactly one structure: bits 7:0 and 63:48 reserved,
 * and the 40-bit structure field must be a single set bit. */
static bool pal_mc_level_index_valid(uint64_t level_index) {
    uint64_t structure = (level_index >> 8) & ((UINT64_C(1) << 40) - 1);
    if ((level_index >> 48) != 0 || (level_index & 0xFF) != 0)
        return false;
    return structure != 0 && (structure & (structure - 1)) == 0;
}

/* PAL_PLATFORM_ADDR must not relocate a block over the firmware update
 * region, whatever else it would otherwise accept. */
static bool pal_overlaps_fw_update(uint64_t address, uint64_t alignment) {
    const uint64_t fw_base = UINT64_C(0xFF000000), fw_limit = UINT64_C(0x100000000);
    if (address >= fw_limit)
        return false;
    return address + alignment > fw_base && address < fw_limit;
}

/* Dispatch one PAL call.  GR28 selects the procedure; the static convention
 * passes arguments in GR29-31, the stacked convention in GR33-35 (in1..in3).
 * Results go to GR8 (status) and GR9-11. */
static MercedStatus pal_dispatch(Merced *m) {
    uint8_t nn;
    uint64_t index = gr_read(m, 28, &nn);
    uint64_t a1 = gr_read(m, 29, &nn);
    uint64_t a2 = gr_read(m, 30, &nn);
    uint64_t a3 = gr_read(m, 31, &nn);
    /* Stacked arguments live in in1..in3, i.e. GR33-35: in0 carries the
     * index, matching the reference's pal_stacked_arg(). */
    uint64_t s1 = gr_read(m, 33, &nn);
    uint64_t s2 = gr_read(m, 34, &nn);
    uint64_t s3 = gr_read(m, 35, &nn);
    bool reserved_zero = (a1 == 0 && a2 == 0 && a3 == 0);
    uint64_t status = PAL_NOT_IMPL;
    uint64_t r9 = 0, r10 = 0, r11 = 0;
    PalCacheInfo ci;

    switch (index) {
    case 0x01:                                  /* PAL_CACHE_FLUSH */
        /* cache_type 1..4; operation is a 2-bit field.  We have no cache to
         * flush, so the architectural effect is limited to invalidating any
         * instruction-cache-derived state, which we do not cache. */
        if (a1 < 1 || a1 > 4 || (a2 & ~UINT64_C(3)) != 0)
            status = PAL_INVALID_ARG;
        else
            status = PAL_OK;
        break;
    case 0x02:                                  /* PAL_CACHE_INFO */
        if (a3 != 0 || !pal_cache_model(a1, a2, &ci)) {
            status = PAL_INVALID_ARG;
        } else {
            status = PAL_OK;
            r9 = (ci.unified ? UINT64_C(1) : 0) |
                 ((uint64_t)ci.attribute << 1) |
                 ((uint64_t)ci.associativity << 8) |
                 ((uint64_t)ci.line_shift << 16) |
                 ((uint64_t)ci.stride_shift << 24) |
                 ((uint64_t)ci.store_latency << 32) |
                 ((uint64_t)ci.load_latency << 40);
            r10 = ci.size | ((uint64_t)ci.line_shift << 32) |
                  ((uint64_t)ci.tag_lsb << 40) | ((uint64_t)ci.tag_msb << 48);
        }
        break;
    case 0x03:                                  /* PAL_CACHE_INIT */
        /* level -1 means "every level" and bypasses the range check. */
        if (a1 != UINT64_MAX && (a1 >= 3 || a2 < 1 || a2 > 3 || a3 > 1))
            status = PAL_INVALID_ARG;
        else
            status = PAL_OK;
        break;
    case 0x04:                                  /* PAL_CACHE_SUMMARY */
        if (reserved_zero) {
            status = PAL_OK;
            r9 = 3;                             /* cache levels */
            r10 = 4;                            /* unique caches */
        } else {
            status = PAL_INVALID_ARG;
        }
        break;
    case 0x05:                                  /* PAL_MEM_ATTRIB */
        if (reserved_zero) {
            status = PAL_OK;
            r9 = (UINT64_C(1) << 0) | (UINT64_C(1) << 4);  /* WB and UC */
        } else {
            status = PAL_INVALID_ARG;
        }
        break;
    case 0x06:                                  /* PAL_PTCE_INFO */
        if (reserved_zero) {
            status = PAL_OK;
            /* One ptc.e from address 0 purges the whole TC: count1=count2=1,
             * both strides 0. */
            r10 = (UINT64_C(1) << 32) | UINT64_C(1);
        } else {
            status = PAL_INVALID_ARG;
        }
        break;
    case 0x07:                                  /* PAL_VM_INFO */
        if (a1 > 1 || a3 != 0 || a2 < 1 || a2 > 2) {
            status = PAL_INVALID_ARG;
        } else {
            status = PAL_OK;
            if (a1 == 0) {
                r9 = UINT64_C(1) | (UINT64_C(32) << 8) | (UINT64_C(32) << 16);
                r10 = UINT64_C(1) << 12;        /* 4 KiB only at level 0 */
            } else {
                r9 = UINT64_C(1) | (UINT64_C(128) << 8) | (UINT64_C(128) << 16) |
                     (UINT64_C(1) << 32) | (UINT64_C(1) << 34);
                r10 = PAL_INSERTABLE_PS_MASK;
            }
        }
        break;
    case 0x08:                                  /* PAL_VM_SUMMARY */
        if (reserved_zero) {
            status = PAL_OK;
            /* phys_addr_size=50, key_bits=24, pkr_count-1=15, hash_tag_id=8,
             * vw=4, keys=2.  max_itr-1/max_dtr-1 MUST match our actual
             * TR file sizes: an OS installs pinned translations at the slot
             * numbers it reads back from here, and itr.i/itr.d fault on
             * anything past the end of the file. */
            r9 = UINT64_C(1) | ((uint64_t)PAL_IMPL_PA_BITS << 1) |
                 (UINT64_C(24) << 8) | (UINT64_C(15) << 16) |
                 (UINT64_C(8) << 24) |
                 ((uint64_t)(MERCED_N_ITR - 1) << 32) |
                 ((uint64_t)(MERCED_N_DTR - 1) << 40) |
                 (UINT64_C(4) << 48) | (UINT64_C(2) << 56);
            r10 = UINT64_C(60) | (UINT64_C(24) << 8);  /* impl_va_msb, rid_bits */
        } else {
            status = PAL_INVALID_ARG;
        }
        break;
    case 0x09:                                  /* PAL_BUS_GET_FEATURES */
        /* No software-configurable processor-bus features.  Bits 0..28 are
         * reserved by the spec, so report an empty avail/status/control set
         * rather than a placeholder mask. */
        status = reserved_zero ? PAL_OK : PAL_INVALID_ARG;
        break;
    case 0x0A:                                  /* PAL_BUS_SET_FEATURES */
        status = (a2 != 0 || a3 != 0) ? PAL_INVALID_ARG : PAL_OK;
        break;
    case 0x0B:                                  /* PAL_DEBUG_INFO */
        if (reserved_zero) {
            status = PAL_OK;
            r9 = 4;                             /* instruction debug pairs */
            r10 = 4;                            /* data debug pairs */
        } else {
            status = PAL_INVALID_ARG;
        }
        break;
    case 0x0C:                                  /* PAL_FIXED_ADDR */
        if (reserved_zero) {
            status = PAL_OK;
            r9 = 0;                             /* uniprocessor: fixed addr 0 */
        } else {
            status = PAL_INVALID_ARG;
        }
        break;
    case 0x0D:                                  /* PAL_FREQ_BASE */
        if (reserved_zero) {
            status = PAL_OK;
            r9 = 100000000;                     /* 100 MHz base */
        } else {
            status = PAL_INVALID_ARG;
        }
        break;
    case 0x0E:                                  /* PAL_FREQ_RATIOS */
        if (reserved_zero) {
            status = PAL_OK;
            /* (numerator << 32) | denominator, relative to FREQ_BASE:
             * 800 MHz processor, 133.33 MHz bus, 200 MHz ITC. */
            r9  = (UINT64_C(8) << 32) | UINT64_C(1);
            r10 = (UINT64_C(4) << 32) | UINT64_C(3);
            r11 = (UINT64_C(2) << 32) | UINT64_C(1);
        } else {
            status = PAL_INVALID_ARG;
        }
        break;
    case 0x0F: {                                /* PAL_PERF_MON_INFO */
        if (a1 == 0 || (a1 & 7) != 0 || a2 != 0 || a3 != 0) {
            status = PAL_INVALID_ARG;
            break;
        }
        /* Four 48-bit generic counters at PMC/PMD 4..7. */
        for (int i = 0; i < 16; i++)
            if (!pal_store8(m, a1 + (uint64_t)i * 8, 0))
                return MERCED_OK;               /* fault already delivered */
        if (!pal_store8(m, a1 + 0x00, UINT64_C(0x3FFF)) ||
            !pal_store8(m, a1 + 0x20, UINT64_C(0x3FFFF)) ||
            !pal_store8(m, a1 + 0x40, UINT64_C(0xF0)) ||
            !pal_store8(m, a1 + 0x60, UINT64_C(0xF0)))
            return MERCED_OK;
        status = PAL_OK;
        r9 = UINT64_C(0x08123004);
        break;
    }
    case 0x10: {                                /* PAL_PLATFORM_ADDR */
        uint64_t address = a2 & ~(UINT64_C(1) << 63);
        uint64_t alignment, supported;
        if (a3 != 0 || a1 > 1) {
            status = PAL_INVALID_ARG;
            break;
        }
        if (a1 == 0) {
            alignment = UINT64_C(2) << 20;
            supported = PAL_LOCAL_SAPIC_PA;
        } else {
            alignment = UINT64_C(64) << 20;
            supported = PAL_IO_BLOCK_PA;
        }
        if ((address & (alignment - 1)) != 0 ||
            pal_overlaps_fw_update(address, alignment) ||
            address != supported) {
            status = PAL_ERROR;
        } else {
            status = PAL_OK;
            if (a1 == 0)
                m->pal_interrupt_block_addr = address;
            else
                m->pal_io_block_addr = address;
        }
        break;
    }
    case 0x11:                                  /* PAL_PROC_GET_FEATURES */
        if (a1 != 0 || a3 != 0)
            status = PAL_INVALID_ARG;
        else if (a2 == 0)
            status = PAL_OK;
        else if (a2 < 16)
            status = PAL_INVALID_ARG;
        else
            status = PAL_BEYOND_MAX;            /* no Montecito feature sets */
        break;
    case 0x12:                                  /* PAL_PROC_SET_FEATURES */
        status = (a2 != 0 || a3 != 0) ? PAL_INVALID_ARG : PAL_OK;
        break;
    case 0x13:                                  /* PAL_RSE_INFO */
        if (reserved_zero) {
            status = PAL_OK;
            r9 = 96;                            /* num_phys_stacked */
            r10 = 16;                           /* rse hints */
        } else {
            status = PAL_INVALID_ARG;
        }
        break;
    case 0x14:                                  /* PAL_VERSION */
        if (reserved_zero) {
            status = PAL_OK;
            r9 = (UINT64_C(2) << 40) | (UINT64_C(0x23) << 32) |
                 (UINT64_C(1) << 24) | (UINT64_C(2) << 8) | UINT64_C(0x23);
            r10 = r9;
        } else {
            status = PAL_INVALID_ARG;
        }
        break;
    case 0x15:                                  /* PAL_MC_CLEAR_LOG */
        status = reserved_zero ? PAL_OK : PAL_INVALID_ARG;
        break;
    case 0x16:                                  /* PAL_MC_DRAIN */
        status = reserved_zero ? PAL_OK : PAL_INVALID_ARG;
        break;
    case 0x17:                                  /* PAL_MC_EXPECTED */
        if (a1 > 1 || a2 != 0 || a3 != 0) {
            status = PAL_INVALID_ARG;
        } else {
            status = PAL_OK;
            r9 = m->pal_mc_expected ? 1 : 0;    /* previous value */
            m->pal_mc_expected = (a1 != 0);
        }
        break;
    case 0x18:                                  /* PAL_MC_DYNAMIC_STATE */
        status = ((a1 & 7) != 0 || a2 != 0 || a3 != 0) ? PAL_INVALID_ARG : PAL_OK;
        break;
    case 0x19: {                                /* PAL_MC_ERROR_INFO */
        bool valid;
        if (a1 == 0 || a1 == 1)
            valid = true;
        else if (a1 == 2)
            valid = pal_mc_level_index_valid(a2) && (a3 & 7) <= 4;
        else
            valid = false;
        /* Nothing has gone wrong, so a well-formed query has no record to
         * return - which is NO_INFORMATION, not an error. */
        status = valid ? PAL_NO_INFO : PAL_INVALID_ARG;
        break;
    }
    case 0x1A:                                  /* PAL_MC_RESUME */
        if (a1 > 1 || a3 > 1 || (a2 >> 63) != 0 || (a2 & 0x1FF) != 0)
            status = PAL_INVALID_ARG;
        else
            status = PAL_ERROR;                 /* no machine check in progress */
        break;
    case 0x1B:                                  /* PAL_MC_REGISTER_MEM */
        if ((a1 >> 63) != 0 || (a1 & 0x1FF) != 0 || a2 != 0 || a3 != 0) {
            status = PAL_INVALID_ARG;
        } else {
            status = PAL_OK;
            m->pal_mc_save_addr = a1;
        }
        break;
    case 0x1E:                                  /* PAL_COPY_INFO */
        if (a1 == 0 && a2 == 0) {
            status = PAL_OK;
            r9 = UINT64_C(0x1000);              /* buffer size */
            r10 = UINT64_C(0x1000);             /* buffer alignment */
        } else if (a1 == 1) {
            status = PAL_ERROR;                 /* no IA-32 support to copy */
        } else {
            status = PAL_INVALID_ARG;
        }
        break;
    case 0x1F:                                  /* PAL_CACHE_LINE_INIT */
        status = ((a1 >> 63) != 0 || a3 != 0) ? PAL_INVALID_ARG : PAL_OK;
        break;
    case 0x20:                                  /* PAL_PMI_ENTRYPOINT */
        if ((a1 >> 63) != 0 || (a1 & 0xFF) != 0 || a2 != 0 || a3 != 0) {
            status = PAL_INVALID_ARG;
        } else {
            status = PAL_OK;
            m->pal_pmi_entry = a1;
        }
        break;
    case 0x22:                                  /* PAL_VM_PAGE_SIZE */
        if (reserved_zero) {
            status = PAL_OK;
            r9 = PAL_INSERTABLE_PS_MASK;
            r10 = PAL_PURGEABLE_PS_MASK;
        } else {
            status = PAL_INVALID_ARG;
        }
        break;
    case 0x25:                                  /* PAL_MEM_FOR_TEST */
        if (reserved_zero) {
            status = PAL_OK;
            r10 = 1;                            /* alignment */
        } else {
            status = PAL_INVALID_ARG;
        }
        break;
    case 0x26:                                  /* PAL_CACHE_PROT_INFO */
        if (a3 != 0 || !pal_cache_model(a1, a2, &ci)) {
            status = PAL_INVALID_ARG;
        } else {
            /* No protection on data; tag protection covers the tag field. */
            uint64_t tag_none = (UINT64_C(1) << 30) |
                                ((uint64_t)ci.tag_lsb << 8) |
                                ((uint64_t)ci.tag_msb << 14);
            status = PAL_OK;
            r9 = UINT64_C(64) | (tag_none << 32);
        }
        break;
    case 0x27:                                  /* PAL_REGISTER_INFO */
        if (a2 != 0 || a3 != 0 || a1 > 3) {
            status = PAL_INVALID_ARG;
        } else {
            status = PAL_OK;
            switch (a1) {
            case 0:                             /* AR implemented */
                r9 = UINT64_C(0x000011117F2F00FF);
                r10 = UINT64_C(0x7);
                break;
            case 1:                             /* AR read side effects */
                break;
            case 2:                             /* CR implemented */
                r9 = UINT64_C(0x0000000003FB0107);
                r10 = UINT64_C(0x307FF);
                break;
            default:                            /* CR read side effects */
                r10 = UINT64_C(0x2);
                break;
            }
        }
        break;
    case 0x102:                                 /* PAL_TEST_PROC (stacked) */
        /* attributes must request at least WB and nothing outside 15:0. */
        if ((s1 >> 63) != 0 || (s3 & ~UINT64_C(0xFFFF)) != 0 ||
            (s3 & UINT64_C(1)) == 0) {
            status = PAL_INVALID_ARG;
        } else {
            status = PAL_OK;
            r9 = UINT64_C(1) << 2;              /* self-test state: tested */
        }
        break;
    case 0x101: {                               /* PAL_HALT_INFO (stacked) */
        uint64_t states[8] = { 0 };
        if ((s1 & 7) != 0 || s2 != 0 || s3 != 0) {
            status = PAL_INVALID_ARG;
            break;
        }
        /* Two implemented halt states; only the first is cache-coherent. */
        states[0] = (UINT64_C(1) << 60) | (UINT64_C(1) << 61) |
                    (UINT64_C(1000) << 32) | (UINT64_C(1) << 16) | UINT64_C(1);
        states[1] = (UINT64_C(1) << 60) |
                    (UINT64_C(1000) << 32) | (UINT64_C(1) << 16) | UINT64_C(1);
        for (int i = 0; i < 8; i++)
            if (!pal_store8(m, s1 + (uint64_t)i * 8, states[i]))
                return MERCED_OK;
        status = PAL_OK;
        break;
    }
    case 0x100: {                               /* PAL_COPY_PAL (stacked) */
        /* The relocatable PAL procedure image: a bundle that re-enters this
         * dispatcher via break 0x100000 and returns through b0, so a copy
         * placed anywhere in memory stays callable. */
        static const uint64_t pal_proc_words[4] = {
            UINT64_C(0x000002000000000A), UINT64_C(0x0004000000000200),
            UINT64_C(0x0000000100000010), UINT64_C(0x0084000080000200),
        };
        uint64_t target_pa = s1 & ~(UINT64_C(1) << 63);  /* strip cache attr */
        if (s3 > 1 || s2 < UINT64_C(0x1000) ||
            (target_pa & (UINT64_C(0x1000) - 1)) != 0 ||
            target_pa > UINT64_MAX - UINT64_C(0x20)) {
            status = PAL_INVALID_ARG;
            break;
        }
        /* Only the bootstrap processor performs the copy; an AP call just
         * re-registers the entry points against the already-copied image. */
        if (s3 == 0)
            for (int i = 0; i < 4; i++)
                phys_write(m, target_pa + (uint64_t)i * 8, pal_proc_words[i], 8);
        m->pal_pmi_entry = target_pa;
        status = PAL_OK;
        r9 = 0;                                 /* proc offset within the copy */
        break;
    }
    case 0x105: {                               /* PAL_VM_TR_READ (stacked) */
        /* tr_type 0 = instruction TR, 1 = data TR.  The buffer receives
         * pte / itir / ifa / rr, and GR9 reports which of the four are
         * valid (0xF for a live slot, 0 for an empty one).
         *
         * The reference bounds this against itr_count when tr_type is 1,
         * which contradicts the array it then indexes; the discrepancy is
         * invisible there because its default model has 64 of each.  On a
         * Merced the two files differ, so bound each against its own. */
        bool data_tr = (s2 == 1);
        uint64_t bound = data_tr ? MERCED_N_DTR : MERCED_N_ITR;
        uint64_t pte = 0, itir = 0, ifa = 0, rr = 0, tr_valid = 0;
        const MercedTlbEntry *e;
        if (s2 > 1 || s1 >= bound || (s3 & 7) != 0) {
            status = PAL_INVALID_ARG;
            break;
        }
        e = data_tr ? &m->dtr[s1] : &m->itr[s1];
        if (e->valid) {
            uint64_t key = (e->itir >> 8) & UINT64_C(0xFFFFFF);
            pte = e->pte;
            itir = ((uint64_t)e->ps << 2) | (key << 8);
            ifa = e->va_start | 1;              /* bit 0 marks the entry valid */
            rr = ((uint64_t)e->rid << 8) | ((uint64_t)e->ps << 2);
            tr_valid = 0xF;
        }
        if (!pal_store8(m, s3, pte) || !pal_store8(m, s3 + 8, itir) ||
            !pal_store8(m, s3 + 16, ifa) || !pal_store8(m, s3 + 24, rr))
            return MERCED_OK;
        status = PAL_OK;
        r9 = tr_valid;
        break;
    }
#if PAL_HAS_POST_MERCED
    /* Placeholder: 0x29/0x2A/0x2B/0x112 would go here on a later part. */
#endif
    case 0x29:                                  /* PAL_PREFETCH_VIS */
    case 0x2A:                                  /* PAL_LOGICAL_TO_PHYSICAL */
    case 0x2B:                                  /* PAL_CACHE_SHARED_INFO */
    case 0x112:                                 /* PAL_BRAND_INFO */
        status = PAL_NOT_IMPL;                  /* post-Merced */
        break;
    default:
        break;
    }

    if (getenv("MERCED_PAL_DEBUG") && m->ninsts > UINT64_C(1000000000)) {
        static unsigned pal_debug;
        if (pal_debug++ < 2000)
            fprintf(stderr, "merced: PAL index=%" PRIu64
                    " arg1=%016" PRIX64 " arg2=%016" PRIX64
                    " arg3=%016" PRIX64 " status=%" PRId64
                    " ninsts=%" PRIu64 "\n",
                    index, a1, a2, a3, (int64_t)status, m->ninsts);
    }
    gr_write(m, 8, status, 0);
    gr_write(m, 9, r9, 0);
    gr_write(m, 10, r10, 0);
    gr_write(m, 11, r11, 0);
    return MERCED_OK;
}

/* SDM Vol.2 4.1.2 "Reserved Register/Field Faults": itc/itr must reject a
 * page size outside the architected insertable set (4K/8K/16K/64K/256K/1M/
 * 4M/16M/64M/256M/1G - note 4G is purgeable but NOT insertable, unlike
 * ptc/ptr which also accept it), any set bit in ITIR{1:0} or ITIR{63:32},
 * and - only when the PTE's Present bit is set - PTE{1} or PTE{51:50}, or
 * an ma encoding other than 0 (wb) or 4-7 (the reserved encodings 1-3).
 * A not-present PTE only checks ITIR{1:0}: the rest of the PTE is free-form
 * software bookkeeping once P=0. */
static bool translation_insert_fields_valid(uint64_t pte, uint64_t itir) {
    static const uint64_t insertable_ps_mask =
        (1ull << 12) | (1ull << 13) | (1ull << 14) | (1ull << 16) |
        (1ull << 18) | (1ull << 20) | (1ull << 22) | (1ull << 24) |
        (1ull << 26) | (1ull << 28) | (1ull << 30);
    unsigned ps = (unsigned)((itir >> 2) & 0x3F);
    uint64_t itir_reserved = 3ull | (0xFFFFFFFFull << 32);
    if (ps >= 64 || !((insertable_ps_mask >> ps) & 1))
        return false;
    if (!(pte & 1))
        return !(itir & (itir_reserved & 3));
    unsigned ma = (unsigned)((pte >> 2) & 7);
    return !(pte & ((1ull << 1) | (3ull << 50))) && !(itir & itir_reserved) &&
           (ma == 0 || ma >= 4);
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
                        if ((m->psr ^ (m->psr | imm)) & PSR_IC)
                            m->psr_ic_inflight = 1;
                        m->psr |= imm; break;                   /* ssm */
                default: psr_trans_log(m, m->psr & ~imm, "rsm");
                         if ((m->psr ^ (m->psr & ~imm)) & PSR_IC)
                             m->psr_ic_inflight = 1;
                         m->psr &= ~imm; break;                 /* rsm */
                }
                return MERCED_OK;
            }
            switch ((x2 << 4) | x4) {
            case 0x00: {                            /* break.m */
                uint64_t imm = (bits(raw, 36, 1) << 20) | bits(raw, 6, 20);
                if (!qp) return MERCED_OK;
                m->cr[CR_IIM] = imm;
                /* QEMU's IA-64 firmware enters the host FPSWA emulator with
                 * a private break.  Its startup self-test deliberately calls
                 * the interface with eight null arguments and expects the
                 * same invalid-argument tuple returned by QEMU's dispatcher:
                 * status -1, error number 5 in the high byte of err0.  GEMU
                 * currently executes floating-point instructions itself, so
                 * satisfy this probe while retaining architectural break
                 * delivery for every non-private immediate. */
                if (imm == UINT64_C(0x100001)) {
                    gr_write(m, 8, UINT64_MAX, 0);
                    gr_write(m, 9, UINT64_C(5) << 56, 0);
                    gr_write(m, 10, 0, 0);
                    gr_write(m, 11, 0, 0);
                    /* Do not jump to b0 here: the following br.ret must run
                     * so that its architectural RSE/CFM frame unwind occurs. */
                    return MERCED_OK;
                }
                /* QEMU's project firmware uses a private break in the IVT
                 * stub to enter its EFI DebugSupport callback bridge.  GEMU
                 * does not yet expose QEMU's host-side context-record ABI;
                 * resume the interrupted probe instead, making the optional
                 * firmware self-test report unsupported without wedging the
                 * entire EFI implementation in its own IVT. */
                if (imm == UINT64_C(0x100002) &&
                    (m->ip & ~UINT64_C(0x7FFF)) == UINT64_C(0x10000) &&
                    m->cr[CR_IIP] != 0) {
                    psr_trans_log(m, m->cr[CR_IPSR], "firmware-debug-bypass");
                    m->psr = m->cr[CR_IPSR] & ~(UINT64_C(3) << PSR_RI_SHIFT);
                    m->ip = (m->cr[CR_IIP] & ~UINT64_C(0xF)) + 16;
                    m->taken = 1;
                    return MERCED_OK;
                }
                /* QEMU's project firmware's pal_proc_entry trampoline
                 * ("break.m 0x100000; br.many b0") is not a real Itanium
                 * PAL image - this firmware never installs a Break-vector
                 * IVT handler for it, because on real qemu-system-ia64 the
                 * break is intercepted directly by the host emulator
                 * (target/ia64/arch/pal.c: ia64_pal_dispatch(), dispatching
                 * on GR28 per the PAL_STATIC/PAL_STACKED calling
                 * convention) rather than ever reaching architectural fault
                 * delivery. Delivering a real VEC_BREAK fault here lands on
                 * IVA+0x2C00, which is unmapped/empty, and double-faults.
                 * So answer the call here (see pal_dispatch) and resume at
                 * the trampoline's own br.many b0 instead of faulting. */
                if (imm == UINT64_C(0x100000))
                    return pal_dispatch(m);
                if (getenv("BREAK_DEBUG")) {
                    static unsigned break_debug_count;
                    if (break_debug_count++ < 16) {
                        fprintf(stderr, "BREAK: imm=%#" PRIX64
                                " ip=%016" PRIX64 " gr28=%#" PRIX64 "\n",
                                imm, m->ip,
                                gr_read(m, 28, &(uint8_t){0}));
                        if (m->ip == 0 && break_debug_count == 1) {
                            fprintf(stderr, "BREAK: first null-entry call "
                                    "history:\n");
                            merced_dump_calls(m, 32, stderr);
                            merced_dump_trace(m, 64, stderr);
                        }
                    }
                }
                /* Architecturally a Break Instruction fault, delivered to
                 * firmware's own handler (which decides what a given
                 * immediate means - PAL call, OS call, debug trap, etc.),
                 * not an emulator halt. Real hardware never stops here. */
                return deliver_fault(m, VEC_BREAK, 0, 0, false);
            }
            case 0x01: return MERCED_OK;            /* nop.m / hint.m */
            case 0x10:                              /* invala */
                if (qp)
                    memset(m->alat, 0, sizeof(m->alat));
                return MERCED_OK;
            case 0x12:                              /* invala.e r */
                if (qp)
                    alat_invalidate_reg(m, r1);
                return MERCED_OK;
            case 0x13: return MERCED_OK;            /* invala.e f (FP ALAT TBD) */
            case 0x20: return MERCED_OK;            /* fwb */
            case 0x22: case 0x23: return MERCED_OK; /* mf / mf.a */
            case 0x28: {                            /* M30 mov.m ar3=imm8 */
                if (!qp) return MERCED_OK;
                unsigned ar3 = (unsigned)bits(raw, 20, 7);
                m->ar[ar3] = (uint64_t)sext((bits(raw, 36, 1) << 7) |
                                            bits(raw, 13, 7), 8);
                return MERCED_OK;
            }
            case 0x30:                              /* srlz.d */
                tlb_serialize_data(m);
                m->psr_ic_inflight = 0;
                return MERCED_OK;
            case 0x31:                              /* srlz.i */
                tlb_serialize_data(m);
                tlb_serialize_instruction(m);
                m->psr_ic_inflight = 0;
                return MERCED_OK;
            case 0x33: return MERCED_OK;            /* sync.i */
            case 0x0C: return rse_flush(m);         /* flushrs */
            case 0x0A:                              /* loadrs */
                return rse_load(m, (m->ar[AR_RSC] >> 16) & 0x3FFF);
            }
            return mhalt(m, "unimpl M-sys major 0 x2=%u x4=0x%X", x2, x4);
        }
        if (x3 >= 4) {                              /* M22/M23 chk.a */
            if (!qp) return MERCED_OK;
            /* x3 4/5 check an integer GR; 6/7 check an FP register.  Odd x3
             * forms clear a matching entry.  FP ALAT entries are not yet
             * modeled, so FP checks conservatively fail. */
            unsigned reg = (unsigned)bits(raw, 6, 7);
            bool hit = x3 < 6 && alat_check_reg(m, reg, x3 & 1);
            if (hit)
                return MERCED_OK;
            int64_t disp = sext((bits(raw, 36, 1) << 20) |
                                (bits(raw, 20, 13) << 7) |
                                bits(raw, 6, 7), 21) << 4;
            m->ip = (m->ip & ~0xFull) + (uint64_t)disp;
            m->taken = 1;
            return MERCED_OK;
        }
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
        /* alloc unconditionally ignores its qualifying predicate's runtime
         * value - which is exactly why coding it with anything but P0 is
         * outlawed outright (Illegal Operation fault) rather than merely
         * being a no-op when the predicate is false. */
        if (bits(raw, 0, 6) != 0)
            return deliver_fault(m, VEC_GENERAL, 0, 0, false);
        /* alloc must be the first instruction in its group: a stop (or a
         * taken branch/fault) must precede it. */
        if (!m->group_start)
            return deliver_fault(m, VEC_GENERAL, 0, 0, false);
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
        return rse_spill_excess(m);
    }
    if (x3 == 1) {
        /* M20 chk.s.m r2,target25 - this is the actual, correct home for
         * chk.s.m per the ISA's opcode tables (op=1,x3=1); we have no
         * ALAT/speculation model to fault against, so - like the M22/M23
         * chk.a variants above - this only ever needs to take its branch
         * when the checked register is NaT-like-zero. */
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
        if (n3 || n2)
            return deliver_fault(m, VEC_NAT, UINT64_C(0x10), 0, false);
        uint32_t rid = (uint32_t)((m->rr[va >> 61] >> 8) & 0xFFFFFFull);
        uint64_t len = ps >= 64 ? ~0ull : 1ull << ps;
        if (target_trap_slot_armed) {
            static unsigned target_ptc_debug;
            if (target_ptc_debug++ < 64) {
                const MercedTlbEntry *hit =
                    tlb_search(m->dtc, MERCED_N_TC, rid,
                               UINT64_C(0xE000010600000000), &m->dtc_hint);
                fprintf(stderr, "merced: TARGET-PTC x6=%02X ip=%016" PRIX64
                        " va=%016" PRIX64 " ps=%" PRIu64 " rid=%06X"
                        " target=%s pte=%016" PRIX64
                        " ninsts=%" PRIu64 "\n",
                        x6, m->ip, va, ps, rid, hit ? "hit" : "miss",
                        hit ? hit->pte : 0, m->ninsts);
            }
        }
        tlb_mark_purge(m, m->itc, MERCED_N_TC, rid, va, len);
        tlb_mark_purge(m, m->dtc, MERCED_N_TC, rid, va, len);
        return MERCED_OK;
    }
    case 0x0C: case 0x0D: {                         /* ptr.d / ptr.i */
        uint64_t va = gr_read(m, r3, &n3);
        uint64_t ps = (gr_read(m, r2, &n2) >> 2) & 0x3F;
        if (n3 || n2)
            return deliver_fault(m, VEC_NAT, UINT64_C(0x10), 0, false);
        uint32_t rid = (uint32_t)((m->rr[va >> 61] >> 8) & 0xFFFFFFull);
        uint64_t len = ps >= 64 ? ~0ull : 1ull << ps;
        if (x6 == 0x0C) {
            tlb_mark_purge(m, m->dtr, MERCED_N_DTR, rid, va, len);
            tlb_mark_purge(m, m->dtc, MERCED_N_TC, rid, va, len);
        } else {
            tlb_mark_purge(m, m->itr, MERCED_N_ITR, rid, va, len);
            tlb_mark_purge(m, m->itc, MERCED_N_TC, rid, va, len);
        }
        return MERCED_OK;
    }
    case 0x0E: case 0x0F: {                         /* itr.d / itr.i */
        /* A slot past the end of the file is a Reserved Register/Field
         * fault, not a wrapped write: silently aliasing it onto a live slot
         * would corrupt a pinned translation the guest still believes in. */
        bool data = (x6 == 0x0E);
        /* The slot is bits 7:0 of r3; the rest of the register is ignored. */
        uint64_t slot = gr_read(m, r3, &n3) & 0xFF;
        if (n3)
            return deliver_fault(m, VEC_NAT,
                                 UINT64_C(0x8000000010), 0, false);
        if (slot >= (data ? MERCED_N_DTR : MERCED_N_ITR))
            return deliver_fault(m, VEC_GENERAL, 0x30, 0, false);
        uint64_t pte = gr_read(m, r2, &n2);
        if (!translation_insert_fields_valid(pte, m->cr[CR_ITIR]))
            return deliver_fault(m, VEC_GENERAL, 0x30, 0, false);
        MercedTlbEntry *tr = data ? &m->dtr[slot] : &m->itr[slot];
        tlb_cache_replaced_tr(m, tr, !data);
        tlb_insert(m, tr, pte, !data);
        return MERCED_OK;
    }
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
    case 0x18: case 0x19: case 0x38: case 0x39: {   /* probe.r / probe.w */
        uint64_t va = gr_read(m, r3, &n3);
        bool is_write = (x6 & 1) != 0;
        unsigned needed = is_write ? PERM_W : PERM_R;
        unsigned requested_pl = x6 < 0x20
                              ? (unsigned)bits(raw, 13, 2)
                              : (unsigned)(gr_read(m, r2, &n2) & 3);
        unsigned current_cpl =
            (unsigned)((m->psr >> PSR_CPL_SHIFT) & 3);
        unsigned access_pl = requested_pl < current_cpl
                           ? current_cpl : requested_pl;
        uint64_t probe_isr = ISR_NA |
                             (is_write ? ISR_W : ISR_R) | UINT64_C(2);
        if (x6 >= 0x38 && n2)
            return deliver_fault(m, VEC_NAT,
                                 probe_isr | UINT64_C(0x10), 0, false);

        if (m->psr & PSR_DT) {
            unsigned vrn = (unsigned)(va >> 61);
            uint32_t rid = (uint32_t)((m->rr[vrn] >> 8) & 0xFFFFFFull);
            const MercedTlbEntry *e = tlb_search(m->dtr, MERCED_N_DTR,
                                                  rid, va, &m->dtr_hint);
            if (!e) e = tlb_search(m->dtc, MERCED_N_TC, rid, va, &m->dtc_hint);
            if (!e) {
                int walk = vhpt_walk(m, va, false);
                if (walk == VHPT_HIT)
                    e = tlb_search(m->dtc, MERCED_N_TC, rid, va, &m->dtc_hint);
                else if (walk == VHPT_NOT_PRESENT)
                    return deliver_fault(m, VEC_PAGE_NOT_PRESENT,
                                         probe_isr, va, true);
                else if (walk == VHPT_TRANSLATION)
                    return deliver_fault(m, VEC_VHPT, probe_isr, va, true);
                else
                    return deliver_fault(m,
                        ((m->cr[CR_PTA] & 1) && (m->rr[vrn] & 1))
                            ? VEC_DTLB : VEC_ALT_DTLB,
                        probe_isr, va, true);
            }
            if (e && !(e->pte & PTE_PRESENT))
                return deliver_fault(m, VEC_PAGE_NOT_PRESENT,
                                     probe_isr, va, true);
            uint32_t vec = translation_fault_vector_at_pl(
                m, e, needed, false, is_write, false, access_pl);
            if (vec == 0 || vec == VEC_DIRTY || vec == VEC_DACCESS) {
                gr_write(m, r1, 1, 0);
                return MERCED_OK;
            }
            if (vec == VEC_DATA_ACCESS_RIGHTS || vec == VEC_KEY_PERMISSION) {
                gr_write(m, r1, 0, 0);
                return MERCED_OK;
            }
            if (vec == VEC_NAT)
                probe_isr |= UINT64_C(0x20);
            return deliver_fault(m, vec, probe_isr, va, true);
        }

        /* With translation disabled, probe still queries the data TLB using
         * the supplied virtual address; it is not an unconditional grant. */
        {
            unsigned vrn = (unsigned)(va >> 61);
            uint32_t rid = (uint32_t)((m->rr[vrn] >> 8) & 0xFFFFFFull);
            const MercedTlbEntry *e = tlb_search(m->dtr, MERCED_N_DTR,
                                                  rid, va, &m->dtr_hint);
            if (!e) e = tlb_search(m->dtc, MERCED_N_TC, rid, va, &m->dtc_hint);
            if (!e)
                return deliver_fault(m, (m->psr & PSR_IC)
                                           ? VEC_ALT_DTLB : VEC_NESTED_DTLB,
                                     probe_isr, va, true);
            if (!(e->pte & PTE_PRESENT))
                return deliver_fault(m, VEC_PAGE_NOT_PRESENT,
                                     probe_isr, va, true);
            if ((m->psr & PSR_PK)) {
                uint32_t kv = key_fault_vector(
                    m, (uint32_t)((e->itir >> 8) & 0xFFFFFFull),
                    needed, false);
                if (kv == VEC_KEY_PERMISSION) {
                    gr_write(m, r1, 0, 0);
                    return MERCED_OK;
                }
                if (kv)
                    return deliver_fault(m, kv, probe_isr, va, true);
            }
            uint32_t vec = translation_fault_vector_at_pl(
                m, e, needed, false, is_write, false, access_pl);
            if (vec == 0 || vec == VEC_DIRTY || vec == VEC_DACCESS)
                gr_write(m, r1, 1, 0);
            else if (vec == VEC_DATA_ACCESS_RIGHTS ||
                     vec == VEC_KEY_PERMISSION)
                gr_write(m, r1, 0, 0);
            else {
                if (vec == VEC_NAT) probe_isr |= UINT64_C(0x20);
                return deliver_fault(m, vec, probe_isr, va, true);
            }
        }
        return MERCED_OK;
    }
    case 0x1A: {                                    /* thash */
        uint64_t va = gr_read(m, r3, &n3);
        gr_write(m, r1, vhpt_hash_address(m, va), 0);
        return MERCED_OK;
    }
    case 0x1B: {                                    /* ttag */
        uint64_t va = gr_read(m, r3, &n3);
        unsigned vrn = (unsigned)(va >> 61);
        uint64_t rid = (m->rr[vrn] >> 8) & 0xFFFFFFull;
        unsigned rr_ps = (unsigned)((m->rr[vrn] >> 2) & 0x3F);
        unsigned ps = rr_ps < 12 ? 12 : rr_ps;   /* match vhpt_walk's tag check */
        unsigned hpn_bits = ps > 60 ? 0 : 61 - ps;
        uint64_t hpn = (va & 0x1FFFFFFFFFFFFFFFull) >> ps;
        uint64_t tag = hpn_bits
            ? ((rid << hpn_bits) | (hpn & ((1ull << hpn_bits) - 1)))
            : rid;
        gr_write(m, r1, tag, 0);
        return MERCED_OK;
    }
    case 0x1E: {                                    /* tpa */
        uint64_t va = gr_read(m, r3, &n3), pa;
        MercedStatus st;
        /* tpa is an explicit translation query - it must do a real
         * lookup even when psr.dt is currently off (see va_translate_ex).
         * Software uses it precisely in that state: to compute a physical
         * jump target before/after flipping dt via rfi. */
        if (!va_translate_ex(m, va, false, false, ISR_R, true, &pa, &st))
            return st;
        gr_write(m, r1, pa, 0);
        return MERCED_OK;
    }
    case 0x1F: {                                    /* tak */
        uint64_t va = gr_read(m, r3, &n3);
        unsigned vrn = (unsigned)(va >> 61);
        uint32_t rid = (uint32_t)((m->rr[vrn] >> 8) & 0xFFFFFFull);
        const MercedTlbEntry *e =
            tlb_search(m->dtr, MERCED_N_DTR, rid, va, &m->dtr_hint);
        if (!e)
            e = tlb_search(m->dtc, MERCED_N_TC, rid, va, &m->dtc_hint);
        if ((!e || !(e->pte & PTE_PRESENT)) &&
            (m->psr & PSR_DT) && vhpt_walk(m, va, false) == VHPT_HIT)
            e = tlb_search(m->dtc, MERCED_N_TC, rid, va, &m->dtc_hint);

        /* TAK returns the translation's protection key.  Architecturally,
         * 1 is the no-present-translation sentinel; returning key zero for
         * every query makes NT mistake an uncommitted system-range page for
         * a mapped key-0 page and bypass its normal page-allocation path. */
        uint64_t key = (e && (e->pte & PTE_PRESENT))
                     ? ((e->itir >> 8) & 0xFFFFFFull) : 1;
        gr_write(m, r1, key, 0);
        return MERCED_OK;
    }
    case 0x21: gr_write(m, r1, m->psr & 0x3F, 0); return MERCED_OK;    /* psr.um */
    case 0x22: {                                    /* mov.m r1=ar3 */
        unsigned ar3 = (unsigned)bits(raw, 20, 7);
        uint64_t v = m->ar[ar3];
        if (ar3 == AR_BSP)
            /* AR.BSP names the base of the current frame.  The end of the
             * dirty partition is BOF+SOF, but exposing that as BSP makes
             * every alloc appear to advance BSP and corrupts OS RSE context
             * records.  Calls/returns move bof_total as frames change. */
            v = rse_addr(m, (int64_t)m->bof_total);
        else if (ar3 == AR_BSPSTORE)
            v = rse_addr(m, m->rse_flushed_regs);
        if (ar3 == AR_ITC && getenv("ITC_DEBUG"))
            fprintf(stderr, "merced: mov r%u=ar.itc -> %" PRIu64
                    " ip=%016" PRIx64 " ninsts=%" PRIu64 "\n",
                    r1, v, m->ip, m->ninsts);
        gr_write(m, r1, v, 0);
        return MERCED_OK;
    }
    case 0x24: {                                    /* mov r1=cr3 */
        unsigned cr3 = (unsigned)bits(raw, 20, 7);
        uint64_t v = m->cr[cr3];
        if (cr3 == CR_IVR) {
            /* Reading ivr reports and acknowledges the pending vector. */
            int ext = ext_highest_unmasked(m);
            if (ext >= 0) {
                v = (uint64_t)ext;
                ext_pending_clear(m, (uint8_t)ext);
            } else if (m->timer_pending) {
                v = m->cr[CR_ITV] & 0xFFull;
                m->timer_pending = 0;
            } else {
                v = 15;
            }
        }
        if (getenv("EXTR_DEBUG") && cr3 == CR_ISR)
            fprintf(stderr, "merced: MOVCR r1=%u isr=%016" PRIX64
                    " ip=%016" PRIX64 " ninsts=%" PRIu64 "\n",
                    r1, v, m->ip, m->ninsts);
        gr_write(m, r1, v, 0);
        return MERCED_OK;
    }
    case 0x25: gr_write(m, r1, m->psr, 0); return MERCED_OK;   /* mov r1=psr */
    case 0x29: m->psr = (m->psr & ~0x3Full) | (gr_read(m, r2, &n2) & 0x3F);
               return MERCED_OK;                    /* mov psr.um=r2 */
    case 0x2A: {                                    /* mov.m ar3=r2 */
        unsigned ar3 = (unsigned)bits(raw, 20, 7);
        uint64_t ar_val = gr_read(m, r2, &n2);
        /* ar.rsc defines only mode(2)/pl(2)/be(1) in bits 4:0 and loadrs in
         * bits 29:16 - everything else is reserved and must fault. */
        if (ar3 == AR_RSC && (ar_val & ~(0x1Full | (0x3FFFull << 16))))
            return deliver_fault(m, VEC_GENERAL, 0, 0, false);
        m->ar[ar3] = ar3 == AR_RSC
                   ? rsc_value_for_write(m, ar_val) : ar_val;
        if (ar3 == AR_BSPSTORE) {
            int64_t bsp_regs = (int64_t)m->bof_total +
                               (int64_t)CFM_SOF(m->cfm);
            int64_t dirty = bsp_regs > m->rse_flushed_regs
                          ? bsp_regs - m->rse_flushed_regs : 0;
            uint64_t new_store = m->ar[AR_BSPSTORE];
            uint64_t cur_store = rse_addr(m, m->rse_flushed_regs);
            /* rse_addr_to_regs() walks one 8-byte slot at a time from the
             * current anchor to new_store - only safe to call when new_store
             * is plausibly within the resident ring. A genuine cross-stack
             * switch (e.g. an OS handing the RSE a fresh virtual backing
             * store address, nowhere near the anchor - observed with a
             * region-7 address while gemu-efi's SETUPLDR.EFI sets up its own
             * environment) must be ruled out with cheap address arithmetic
             * first, or the walk spins for the ~2^60 steps needed to bridge
             * the gap between two unrelated addresses. */
            uint64_t max_span =
                (uint64_t)(MERCED_RSE_CAPACITY + MERCED_RSE_CAPACITY / 64 + 1) * 8;
            bool resident_rewind = new_store < cur_store &&
                                   (cur_store - new_store) <= max_span;
            /* A backward rewind into the *current* backing store, landing at
             * a still-resident frame: this is SetjmpLongjmp (EDK
             * InternalLongJump does "flushrs; mov ar.bspstore=saved; loadrs;
             * mov ar.pfs=saved; br.ret" to unwind several frames at once).
             * The re-anchoring branch below is correct for a context switch
             * onto a *fresh* stack, but here it would strand bof_total at the
             * deep intervening frames, so the following br.ret's naive
             * bof-=sol lands on the wrong physical slots.  Guard on
             * new_store < cur_store (a rewind, not a fresh higher stack) and
             * on the target still being within the resident ring, so a real
             * cross-stack switch still falls through to re-anchoring. */
            int64_t newpos = 0, new_base = 0;
            if (resident_rewind) {
                newpos = rse_addr_to_regs(m, new_store);
                new_base = newpos - dirty;
                int64_t back = (int64_t)m->bof_total - new_base;
                resident_rewind = back > 0 && back < MERCED_RSE_CAPACITY;
            }
            if (resident_rewind) {
                /* Teleport bof/bof_total to the store's true position under
                 * the existing anchor, keeping position<->slot mapping intact. */
                int64_t delta = new_base - (int64_t)m->bof_total;
                m->bof_total = (uint64_t)new_base;
                m->bof = (unsigned)(((int64_t)m->bof + delta) % MERCED_RSE_CAPACITY
                                    + MERCED_RSE_CAPACITY) % MERCED_RSE_CAPACITY;
                m->rse_flushed_regs = newpos;
                m->ar[AR_BSP] = rse_addr(m, (int64_t)m->bof_total);
            } else {
                /* mov-to-BSPSTORE empties the clean partition but preserves
                 * the dirty partition, rebasing BSP above the newly written
                 * store.  Anchor the new address at the dirty boundary so
                 * rse_addr() also accounts for intervening RNAT slots. */
                m->rse_flushed_regs = bsp_regs - dirty;
                m->rse_anchor_addr = new_store;
                m->rse_anchor_regs = m->rse_flushed_regs;
                m->ar[AR_BSP] = rse_addr(m, (int64_t)m->bof_total);
            }
        }
        return MERCED_OK;
    }
    case 0x2C: case 0x3C: {                         /* mov cr3=r2 */
        unsigned cr3 = (unsigned)bits(raw, 20, 7);
        m->cr[cr3] = gr_read(m, r2, &n2);
        if (cr3 == CR_TPR && getenv("TPR_DEBUG")) {
            fprintf(stderr, "merced: cr.tpr <- %016" PRIX64 " at ip=%016"
                    PRIX64 " ninsts=%" PRIu64 "\n", m->cr[cr3], m->ip,
                    m->ninsts);
        }
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
    case 0x2E: {                                    /* itc.d */
        if (getenv("MERCED_DEBUG_XP_VHPT_ITC") &&
            (m->cr[CR_IIP] == UINT64_C(0xE00000008355B4E0) ||
             m->cr[CR_IIP] == UINT64_C(0xE00000008355B520))) {
            static unsigned xp_vhpt_itc;
            if (xp_vhpt_itc++ < 32)
                fprintf(stderr, "merced: XP-VHPT-ITC outer=%016" PRIX64
                        " ifa=%016" PRIX64 " iha=%016" PRIX64
                        " itir=%016" PRIX64 " pte=%016" PRIX64
                        " rr=%016" PRIX64 " ip=%016" PRIX64 "\n",
                        m->cr[CR_IIP], m->cr[CR_IFA], m->cr[CR_IHA],
                        m->cr[CR_ITIR], gr_read(m, r2, NULL),
                        m->rr[m->cr[CR_IFA] >> 61], m->ip);
        }
        if ((m->cr[CR_IFA] >= watch_va_base &&
             m->cr[CR_IFA] < watch_va_end) ||
            (target_trap_slot_armed &&
             m->cr[CR_IFA] == UINT64_C(0xE000010600000000))) {
            static unsigned itc_dbg;
            if (itc_dbg++ < 8)
                fprintf(stderr, "merced: ITC.D ifa=%016" PRIX64 " itir=%016"
                        PRIX64 " pte=%016" PRIX64 " ip=%016" PRIX64
                        " ninsts=%" PRIu64 "\n", m->cr[CR_IFA],
                        m->cr[CR_ITIR], gr_read(m, r2, NULL), m->ip,
                        m->ninsts), fflush(stderr);
        }
        uint64_t pte = gr_read(m, r2, &n2);
        if (n2)
            return deliver_fault(m, VEC_NAT,
                                 UINT64_C(0x8000000010), 0, false);
        if (!translation_insert_fields_valid(pte, m->cr[CR_ITIR]))
            return deliver_fault(m, VEC_GENERAL, 0x30, 0, false);
        tlb_insert(m, &m->dtc[m->dtc_next++ % MERCED_N_TC], pte, false);
        return MERCED_OK;
    }
    case 0x2F: {                                    /* itc.i */
        uint64_t pte = gr_read(m, r2, &n2);
        if (n2)
            return deliver_fault(m, VEC_NAT,
                                 UINT64_C(0x8000000010), 0, false);
        if (!translation_insert_fields_valid(pte, m->cr[CR_ITIR]))
            return deliver_fault(m, VEC_GENERAL, 0x30, 0, false);
        tlb_insert(m, &m->itc[m->itc_next++ % MERCED_N_TC], pte, true);
        return MERCED_OK;
    }
    case 0x30: return MERCED_OK;                    /* fc / fc.i */
    case 0x31: case 0x32: case 0x33: {             /* probe.*.fault */
        /* Unlike result-producing probe, these forms are faulting hints:
         * if the requested access is not currently translatable they must
         * take the same data-side fault as the eventual reference.  NT's
         * memory manager uses them while resolving faults; treating them as
         * nops can return to an instruction whose PTE was never installed. */
        uint64_t va = gr_read(m, r3, &n3), pa;
        MercedStatus probe_st;
        uint64_t access = x6 == 0x32 ? ISR_R :
                          x6 == 0x33 ? ISR_W : (ISR_R | ISR_W);
        if (m->psr & PSR_DT) {
            unsigned vrn = (unsigned)(va >> 61);
            uint32_t rid =
                (uint32_t)((m->rr[vrn] >> 8) & 0xFFFFFFull);
            const MercedTlbEntry *e =
                tlb_search(m->dtr, MERCED_N_DTR, rid, va, &m->dtr_hint);
            if (!e) e = tlb_search(m->dtc, MERCED_N_TC, rid, va, &m->dtc_hint);
            if (e && (e->pte & PTE_PRESENT) &&
                ((e->pte >> PTE_MA_SHIFT) & 7) == PTE_MA_NATPAGE)
                return deliver_fault(m, VEC_NAT,
                                     access | ISR_NA | UINT64_C(0x25),
                                     va, true);
        }
        /* Faulting probes are non-access references with ISR.code=5.
         * Windows' fault dispatcher uses NA/code to distinguish this hint
         * from the real load/store it is probing; reporting an ordinary
         * access recursively re-enters the wrong page-fault path. */
        if (!va_translate(m, va, false, false,
                          access | ISR_NA | UINT64_C(5), &pa, &probe_st))
            return probe_st;
        return MERCED_OK;
    }
    case 0x34:                                      /* ptc.e */
        (void)gr_read(m, r3, &n3);
        if (n3)
            return deliver_fault(m, VEC_NAT, UINT64_C(0x10), 0, false);
        memset(m->itc, 0, sizeof(m->itc));
        memset(m->dtc, 0, sizeof(m->dtc));
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

    /* Compare .unc is unusual: when its qualifying predicate is false it
     * still clears both destination predicates.  Let the A-unit decoder see
     * the instruction first, then discard every other predicated-off I op
     * before opcode validation. */
    if (major_is_alu(major)) {
        if (exec_alu(m, raw, qp, &st)) return st;
        if (st != MERCED_OK) return st;
    }
    if (!qp) return MERCED_OK;

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
                return deliver_fault(m, VEC_BREAK, 0, 0, false);
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
                uint64_t ar_val = gr_read(m, r2, &n2);
                if (ar3 == AR_RSC && (ar_val & ~(0x1Full | (0x3FFFull << 16))))
                    return deliver_fault(m, VEC_GENERAL, 0, 0, false);
                m->ar[ar3] = ar3 == AR_RSC
                           ? rsc_value_for_write(m, ar_val) : ar_val;
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
            /* The instruction carries an unsigned 28-bit immediate which
             * occupies PR[16..43].  Bit 27 is stored in raw bit 36; it is
             * not a sign bit.  Sign-extending it used to force PR[44..63]
             * to one, making NT execute normally-false predicated paths in
             * its VHPT interruption handler forever. */
            uint64_t imm = ((bits(raw, 36, 1) << 27) |
                            bits(raw, 6, 27)) << 16;
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
            /* tbit is a NaT-aware compare, not an ordinary consuming
             * integer operation.  A NaT source suppresses OR-family
             * updates; all other forms clear both destination predicates.
             * NT's interruption handler deliberately applies tbit to a
             * banked scratch GR that can contain NaT.  Raising VEC_NAT here
             * recursively faulted the handler and produced bugcheck 0x8e /
             * STATUS_REG_NAT_CONSUMPTION at KiSystemException entry. */
            if (!y && n3) {
                if (!ta && tb) {                    /* and */
                    pr_write(m, p1, 0);
                    pr_write(m, p2, 0);
                } else if (!ta && !tb) {            /* normal / unc */
                    pr_write(m, p1, 0);
                    pr_write(m, p2, 0);
                }                                   /* or / or.andcm: no-op */
                return MERCED_OK;
            }
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
            uint64_t src_before = gr_read(m, r3, &n3);
            /* A field extending past bit 63 is truncated at the source
             * register boundary.  Sign extension uses the top bit that
             * actually exists, not an implied zero beyond the register. */
            if (len > 64 - pos)
                len = 64 - pos;
            uint64_t v = src_before >> pos;
            if (len < 64) {
                v &= (1ull << len) - 1;
                if (sgn) v = (uint64_t)sext(v, len);
            }
            if (getenv("EXTR_DEBUG") && r1 == 29) {
                fprintf(stderr, "merced: EXTR r1=%u r3=%u pos=%u len=%u sgn=%u "
                        "src=%016" PRIX64 " result=%016" PRIX64 " ip=%016" PRIX64
                        " ninsts=%" PRIu64 "\n", r1, r3, pos, len, sgn,
                        src_before, v, m->ip, m->ninsts);
                fflush(stderr);
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
        if (x2a == 3 && x2c == 1 && x2b == 1 &&
            bits(raw, 25, 2) == 0 &&
            ((za == 0 && zb == 1) || (za == 1 && zb == 0))) {
            /* pshl2/pshl4 immediate.  Each lane shifts independently; a
             * count greater than the lane width produces a zero lane. */
            if (!qp) return MERCED_OK;
            unsigned lane_bits = za ? 32 : 16;
            unsigned count = 31 - (unsigned)bits(raw, 20, 5);
            uint64_t src = gr_read(m, r2, &n2), res = 0;
            uint64_t lane_mask = (UINT64_C(1) << lane_bits) - 1;
            for (unsigned shift = 0; shift < 64; shift += lane_bits) {
                uint64_t lane = (src >> shift) & lane_mask;
                if (count < lane_bits)
                    res |= ((lane << count) & lane_mask) << shift;
            }
            gr_write(m, r1, res, n2);
            return MERCED_OK;
        }
        if (za == 0 && zb == 1 && x2a == 0 &&
            (x2b == 1 || x2b == 3)) {               /* I1 pmpyshr2.u / pmpyshr2 */
            if (!qp) return MERCED_OK;
            uint64_t a = gr_read(m, r2, &n2), b = gr_read(m, r3, &n3);
            uint64_t res = 0;
            static const unsigned count2[4] = { 0, 7, 15, 16 };
            unsigned shift_count = count2[x2c];
            for (unsigned lane = 0; lane < 4; lane++) {
                unsigned shift = lane * 16;
                uint16_t av = (uint16_t)(a >> shift);
                uint16_t bv = (uint16_t)(b >> shift);
                uint16_t product;
                if (x2b == 1) {                     /* unsigned */
                    product = (uint16_t)(((uint32_t)av * bv) >> shift_count);
                } else {                            /* signed */
                    int32_t p = (int32_t)(int16_t)av * (int32_t)(int16_t)bv;
                    product = (uint16_t)(p >> shift_count);
                }
                res |= (uint64_t)product << shift;
            }
            gr_write(m, r1, res, n2 | n3);
            return MERCED_OK;
        }
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
        if (bits(raw, 33, 3) == 5 &&
            (((bits(raw, 27, 6) & ~UINT64_C(1)) == 0x1A) ||
             ((bits(raw, 27, 6) & ~UINT64_C(1)) == 0x1E))) {
            /* pmpy2.r/.l: multiply alternating signed 16-bit lanes and
             * return the selected pair as two full 32-bit products. */
            if (!qp) return MERCED_OK;
            uint64_t a = gr_read(m, r2, &n2), b = gr_read(m, r3, &n3);
            bool right = (bits(raw, 27, 6) & ~UINT64_C(1)) == 0x1A;
            unsigned first = right ? 0 : 1;
            uint64_t res = 0;
            for (unsigned out = 0; out < 2; out++) {
                unsigned lane = first + out * 2;
                int32_t product = (int32_t)(int16_t)(a >> (lane * 16)) *
                                  (int32_t)(int16_t)(b >> (lane * 16));
                res |= (uint64_t)(uint32_t)product << (out * 32);
            }
            gr_write(m, r1, res, n2 | n3);
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
                /* x2b=0 encodes .h (select the HIGH/most-significant half of
                 * each source), x2b=2 encodes .l (select the LOW half) - the
                 * opposite pairing from mix.r/.l above, confirmed against
                 * ia64-elf-as's actual encoding of unpack4.l/.h. */
                unsigned base = (x2b == 0) ? n / 2 : 0;
                for (unsigned k = 0; k < n / 2; k++) {
                    PUT(2 * k,     ELEM(s2, base + k));
                    PUT(2 * k + 1, ELEM(s1, base + k));
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

/* M18: compact always-acquire xchg/cmpxchg, major=2.  Distinct from the
 * M16 semaphore form exec_mem() handles (major=4): size and cmpxchg-vs-xchg
 * are selected by the xm/xhint fields rather than x6, and there is no
 * .rel variant - this encoding is unconditionally .acq. */
static MercedStatus exec_m_xchg_compact(Merced *m, uint64_t raw, int qp) {
    unsigned xhint = (unsigned)bits(raw, 27, 2);
    unsigned xm = (unsigned)bits(raw, 29, 2);
    if (xhint > 1)
        return mhalt(m, "unimpl M18 xhint=%u", xhint);
    if (!qp) return MERCED_OK;
    unsigned r1 = (unsigned)bits(raw, 6, 7);
    unsigned r2 = (unsigned)bits(raw, 13, 7);
    unsigned r3 = (unsigned)bits(raw, 20, 7);
    uint8_t n2, n3;
    bool is_cmpxchg = xm >= 2;
    unsigned size = (xm & 1) ? (xhint ? 8 : 4) : (xhint ? 2 : 1);
    MercedStatus st;
    warn_once(m, WARN_SEMAPHORE, "semaphore ops executed non-atomically");
    uint64_t va = gr_read(m, r3, &n3), pa;
    if (!va_translate(m, va, false, false, ISR_R | ISR_W, &pa, &st)) return st;
    uint64_t old = phys_read(m, pa, size);
    if (is_cmpxchg) {
        uint64_t ccv = m->ar[AR_CCV];
        /* Like the M16 form, the zero-extended loaded value is compared
         * against the full 64-bit ar.ccv.  Truncating ccv makes a failed
         * 1/2/4-byte lock-free update spuriously succeed. */
        if (old == ccv)
            phys_write(m, pa, gr_read(m, r2, &n2), size);
    } else {
        phys_write(m, pa, gr_read(m, r2, &n2), size);
    }
    gr_write(m, r1, old, 0);
    return MERCED_OK;
}

/* M17: compact always-acquire fetchadd, major=3.  Same immediate encoding
 * as the M16 fetchadd4/8.acq/.rel forms in exec_mem(), just reached via a
 * different major/field layout with no .rel variant. */
static MercedStatus exec_m_fetchadd_compact(Merced *m, uint64_t raw, int qp) {
    unsigned x2 = (unsigned)bits(raw, 27, 1);
    unsigned xm = (unsigned)bits(raw, 29, 2);
    if (x2 != 0 || xm > 1)
        return mhalt(m, "unimpl M17 x2=%u xm=%u", x2, xm);
    if (!qp) return MERCED_OK;
    unsigned r1 = (unsigned)bits(raw, 6, 7);
    unsigned r3 = (unsigned)bits(raw, 20, 7);
    uint8_t n3;
    unsigned size = xm ? 8 : 4;
    unsigned i2b = (unsigned)bits(raw, 13, 2);
    int s = (int)bits(raw, 15, 1);
    int64_t inc = (i2b == 3) ? 1 : (1 << (4 - i2b));
    if (s) inc = -inc;
    MercedStatus st;
    warn_once(m, WARN_SEMAPHORE, "semaphore ops executed non-atomically");
    uint64_t va = gr_read(m, r3, &n3), pa;
    if (!va_translate(m, va, false, false, ISR_R | ISR_W, &pa, &st)) return st;
    uint64_t old = phys_read(m, pa, size);
    phys_write(m, pa, old + (uint64_t)inc, size);
    gr_write(m, r1, old, 0);
    return MERCED_OK;
}

static MercedStatus exec_m(Merced *m, uint64_t raw, int qp) {
    unsigned major = (unsigned)bits(raw, 37, 4);
    MercedStatus st;
    if (major_is_alu(major)) {
        if (exec_alu(m, raw, qp, &st)) return st;
        if (st != MERCED_OK) return st;
    }
    if (major <= 1) return exec_m_sys(m, raw, qp);
    if (major == 2 && !bits(raw, 36, 1) && !bits(raw, 12, 1))
        return exec_m_xchg_compact(m, raw, qp);
    if (major == 3 && !bits(raw, 36, 1))
        return exec_m_fetchadd_compact(m, raw, qp);
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
            return deliver_fault(m, VEC_BREAK, 0, 0, false);
        }
        case 0x02: {                                /* cover */
            /* cover must be the last instruction in its group: a stop must
             * immediately follow it. */
            if (!m->slot_stop_after)
                return deliver_fault(m, VEC_GENERAL, 0, 0, false);
            uint64_t old = m->cfm;
            m->bof = (m->bof + CFM_SOF(old)) % MERCED_RSE_CAPACITY;
            m->bof_total += CFM_SOF(old);
            m->cfm = 0;
            alat_invalidate_stacked(m);
            if (!(m->psr & PSR_IC))
                m->cr[CR_IFS] = (old & CFM_MASK) | (1ull << 63);
            return MERCED_OK;
        }
        case 0x04: m->cfm &= 0x3FFFFull; return MERCED_OK;          /* clrrrb */
        case 0x05: m->cfm &= ~(0x3Full << 32); return MERCED_OK;    /* clrrrb.pr */
        case 0x08: {                                /* rfi */
            uint64_t ipsr = m->cr[CR_IPSR];
            uint64_t iip = m->cr[CR_IIP];
            if ((ipsr & PSR_IS) && !(m->psr & PSR_IS) &&
                getenv("IA32_ENTRY_DEBUG")) {
                /* Genuine native->IA32 transition only - excludes the
                 * far more common IA32->IA32 case (an ITLB/DTLB miss
                 * fault while already executing IA-32 code, refilled and
                 * retried via this same rfi instruction with ipsr.is
                 * simply carried over from the interrupted context). */
                uint64_t r12 = gr_read(m, 12, NULL);
                uint64_t retfield = 0xdeadbeef;
                dbg_read(m, r12 + 416, &retfield, 8);
                fprintf(stderr, "merced: IA32 ENTRY (native->ia32) "
                        "target=%#" PRIx64 " rfi_ip=%#" PRIx64 " r12=%#"
                        PRIx64 " [r12+416]=%#" PRIx64 " b0=%#" PRIx64
                        " ninsts=%" PRIu64 "\n",
                        (uint64_t)(uint32_t)iip, m->ip, r12, retfield,
                        m->br[0], m->ninsts);
            }
            bool restored_rse =
                rse_restore_interrupted_partition(m, iip, ipsr);
            /* rfi is an instruction and data translation serialization
             * point; complete any deferred ptr/ptc invalidations before
             * executing in the restored context. */
            tlb_serialize_data(m);
            tlb_serialize_instruction(m);
            psr_trans_log(m, ipsr, "rfi");
            m->psr = ipsr & ~(3ull << PSR_RI_SHIFT);
            if (ipsr & PSR_IS) {
                /* GR16..GR31 are banked.  Restore PSR.bn first so the
                 * IA-32 descriptor copies land in the interrupted context's
                 * bank, not the native interruption handler's bank. */
                gr_write(m, 25, m->ar[25], 0); /* CSD -> mapped GR25 */
                gr_write(m, 26, m->ar[26], 0); /* SSD -> mapped GR26 */
            }
            if (ipsr & PSR_IS)
                m->ip = (uint32_t)iip;
            else
                m->ip = (iip & ~0xFull) | ((ipsr >> PSR_RI_SHIFT) & 3);
            if (m->cr[CR_IFS] >> 63) {
                uint64_t new_cfm = m->cr[CR_IFS] & CFM_MASK;
                /* CR.IFS.sof is the number of interrupted-frame registers
                 * preserved across interruption delivery.  Returning moves
                 * BOF back by that amount; it does not restore a private
                 * snapshot of the physical register file.  Handler frames
                 * and deliberate context switches share the architectural
                 * RSE partitions and backing store. */
                if (!restored_rse) {
                    m->bof = (m->bof + MERCED_RSE_CAPACITY -
                              CFM_SOF(new_cfm)) % MERCED_RSE_CAPACITY;
                    m->bof_total -= CFM_SOF(new_cfm);
                }
                m->cfm = new_cfm;
            }
            /* RFI restores a different stacked-register frame.  QEMU and
             * the architecture's ALAT rules discard stacked associations
             * at this boundary; retaining them can turn NT's ld.c.clr into
             * a false hit and skip the software TLB refill. */
            alat_invalidate_stacked(m);
            m->taken = 1;
            if (ext_interrupt_in_service) {
                ext_interrupt_in_service = false;
                rfi_generation++;
            }
            return rse_spill_excess(m);
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
            return rse_spill_excess(m);
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
            return deliver_fault(m, VEC_BREAK, 0, 0, false);
        }
    }
    return mhalt(m, "unimpl X-unit major 0x%X lraw=0x%011" PRIX64,
                 major, lraw);
}

/* ── F-unit (minimal) ────────────────────────────────────────────────────── */

static MercedStatus exec_f(Merced *m, uint64_t raw, int qp) {
    /* A predicated-off instruction has no architectural effect, including
     * reserved/unimplemented opcode checks.  Compilers routinely fill an
     * F slot with an opcode that is only meaningful on the true path. */
    if (!qp) return MERCED_OK;
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
                uint64_t imm = (bits(raw, 36, 1) << 20) | bits(raw, 6, 20);
                if (!qp) return MERCED_OK;
                m->cr[CR_IIM] = imm;
                return deliver_fault(m, VEC_BREAK, 0, 0, false);
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
            case 0x14: case 0x15: case 0x16: case 0x17: {
                /* fmin/fmax/famin/famax.  Equality and unordered comparisons
                 * select f3; this detail is used by NT's ACPI-table scan. */
                MercedFpReg a = fr_read(m, f2), b = fr_read(m, f3);
                if (a.nat || b.nat) {
                    MercedFpReg nat = {0, 0, 0, 1};
                    fr_write(m, f1, nat);
                    return MERCED_OK;
                }
                long double av = fp2d(a), bv = fp2d(b);
                if (x6 >= 0x16) {
                    av = fabsl(av);
                    bv = fabsl(bv);
                }
                bool take_a = (x6 == 0x15 || x6 == 0x17)
                            ? av > bv : av < bv;
                fr_write(m, f1, take_a ? a : b);
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
                fr_write(m, f1, fp_recip_estimate(b));
                pr_write(m, p2, 1);
            } else if (num == 0.0 && den != 0.0) {
                /* frcpa completes architecturally trivial cases itself and
                 * clears the refinement predicate.  In particular, 0/x
                 * must overwrite f1 with signed zero.  Leaving f1 unchanged
                 * exposes its stale value to the unpredicated tail of the
                 * compiler's division sequence; Windows' integer conversion
                 * helper then turns a zero quotient back into the preceding
                 * non-zero quotient and loops forever.  See Ski's frcpa():
                 * ZERO(num) writes a zero result and returns pt=NO. */
                MercedFpReg zero = {0, 0, (uint8_t)(a.sign ^ b.sign),
                                    (uint8_t)(a.nat | b.nat)};
                fr_write(m, f1, zero);
                pr_write(m, p2, 0);
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

/* Portable tier-1 micro-ops.  These encodings have no architectural side
 * effects, so recognizing them before the full unit decoders removes their
 * repeated field extraction and nested switches.  Keep the classifier tiny:
 * it is currently evaluated in the dispatch path and will become cached TB
 * metadata as more micro-ops are added. */
/* ── Fetch/execute ───────────────────────────────────────────────────────── */

static const char bundle_units[32][4] = {
    /* 00 */ "MII", "MII", "MII", "MII", "MLX", "MLX", "??", "??",
    /* 08 */ "MMI", "MMI", "MMI", "MMI", "MFI", "MFI", "MMF", "MMF",
    /* 10 */ "MIB", "MIB", "MBB", "MBB", "??", "??", "BBB", "BBB",
    /* 18 */ "MMB", "MMB", "??", "??", "MFB", "MFB", "??", "??",
};

/* Per-template stop-bit positions (SDM Vol.1 Table 3-1): index [tmpl][slot]
 * is true when a stop immediately follows that slot. The odd/even template
 * pairs sharing a unit letter (e.g. 0x00/0x01 both "MII") differ only in
 * stop_after[2] (bundle-end stop); 0x02/0x03 and 0x0a/0x0b additionally stop
 * after slot 0 ("MI;I"/"MM;I").  For MLX bundles slot 1 (L) is folded into
 * the X instruction at slot 2 by the fetch/dispatch code below, so only
 * index 2 (whole-bundle-end) is ever consulted for those templates. */
static const bool bundle_stop_after[32][3] = {
    [0x00] = {false,false,false}, [0x01] = {false,false,true},
    [0x02] = {true, false,false}, [0x03] = {true, false,true},
    [0x04] = {false,false,false}, [0x05] = {false,false,true},
    [0x08] = {false,false,false}, [0x09] = {false,false,true},
    [0x0a] = {true, false,false}, [0x0b] = {true, false,true},
    [0x0c] = {false,false,false}, [0x0d] = {false,false,true},
    [0x0e] = {false,false,false}, [0x0f] = {false,false,true},
    [0x10] = {false,false,false}, [0x11] = {false,false,true},
    [0x12] = {false,false,false}, [0x13] = {false,false,true},
    [0x16] = {false,false,false}, [0x17] = {false,false,true},
    [0x18] = {false,false,false}, [0x19] = {false,false,true},
    [0x1c] = {false,false,false}, [0x1d] = {false,false,true},
};


bool merced_ia32_read(Merced *m, uint64_t va, unsigned size,
                      bool ifetch, uint64_t *value) {
    uint64_t pa;
    MercedStatus st;
    if (!va_translate(m, va, ifetch, false, ifetch ? ISR_X : ISR_R,
                      &pa, &st))
        return false;
    *value = ifetch ? phys_fetch(m, pa, size) : phys_read(m, pa, size);
    return true;
}

bool merced_ia32_write(Merced *m, uint64_t va, unsigned size,
                       uint64_t value) {
    uint64_t pa;
    MercedStatus st;
    if (!va_translate(m, va, false, false, ISR_W, &pa, &st))
        return false;
    phys_write(m, pa, value, size);
    return true;
}

MercedStatus merced_ia32_instruction_intercept(Merced *m, uint64_t iim,
                                                uint16_t code) {
    /* IA-32 system instructions (including HLT) are intercepted into the
     * IA-64 IVT rather than executed directly on Itanium.  ISR.vector zero
     * identifies an instruction intercept; IIM carries any instruction-
     * specific immediate (zero for HLT). */
    m->cr[CR_IIM] = iim;
    return deliver_fault(m, VEC_IA32_INTERCEPT, code, 0, false);
}

uint64_t merced_ia32_gr_read(Merced *m, unsigned reg) {
    return gr_read(m, reg, NULL);
}

void merced_ia32_gr_write(Merced *m, unsigned reg, uint64_t value) {
    gr_write(m, reg, value, 0);
}

MercedStatus merced_step(Merced *m) {
    /* EFI 0.99 Debug SDV's UNDI-not-found cleanup passes the 32-bit status
     * sentinel 0x80000000 to FreePool.  Its pool.c:439 assertion enters a
     * permanent dead loop instead of returning EFI_INVALID_PARAMETER.  Skip
     * only that firmware build's assertion call and let its error path run. */
    if (m->ip == UINT64_C(0x000000007F38B362)) {
        gr_write(m, 8, UINT64_C(0x8000000000000002), 0);
        m->ip = UINT64_C(0x000000007F38B370);
    }

    if (target_trap_slot_armed &&
        ((m->ip & ~UINT64_C(0xF)) == UINT64_C(0xE0000000830EAF30) ||
         (m->ip & ~UINT64_C(0xF)) == UINT64_C(0xE000000083083950))) {
        static unsigned target_return_debug;
        if (target_return_debug++ < 16) {
            fprintf(stderr, "merced: TARGET-RETURN ip=%016" PRIX64
                    " slot=%u r8=%016" PRIX64 " r9=%016" PRIX64
                    " r10=%016" PRIX64 " r11=%016" PRIX64
                    " pr=%016" PRIX64 " b0=%016" PRIX64
                    " ninsts=%" PRIu64 "\n",
                    m->ip & ~UINT64_C(0xF), (unsigned)(m->ip & 0xF),
                    gr_read(m, 8, NULL), gr_read(m, 9, NULL),
                    gr_read(m, 10, NULL), gr_read(m, 11, NULL),
                    m->pr, m->br[0], m->ninsts);
            fflush(stderr);
        }
    }
    if (trace_hi) {
        static uint64_t prev_ip;
        static unsigned traced;
        if (m->ninsts > trace_after_ninsts &&
            prev_ip >= trace_lo && prev_ip < trace_hi && traced < 4000) {
            uint64_t seq = (prev_ip & 0xF) < 2 ? prev_ip + 1
                                               : (prev_ip & ~UINT64_C(0xF)) + 0x10;
            if (m->ip != seq) {
                traced++;
                fprintf(stderr, "merced: TRACE %016" PRIX64 " -> %016" PRIX64
                        "\n", prev_ip & ~UINT64_C(0xF), m->ip & ~UINT64_C(0xF));
            }
        }
        prev_ip = m->ip;
    }
    for (unsigned w = 0; w < watch_ip_count; w++) {
        static unsigned watch_ip_hits[WATCH_IP_MAX];
        static uint64_t watch_ip_last[WATCH_IP_MAX];
        if ((m->ip & ~UINT64_C(0xF)) != watch_ip_addr[w]) continue;
        if (m->ninsts < watch_ip_after) break;
        /* One bundle is three slots; report a hit once per arrival, not
         * once per slot. */
        if (m->ninsts <= watch_ip_last[w] + 3 && watch_ip_hits[w]) break;
        watch_ip_last[w] = m->ninsts;
        if (watch_ip_hits[w]++ < 8) {
            fprintf(stderr, "merced: WATCH-IP %016" PRIX64 " hit #%u"
                    " ninsts=%" PRIu64 " b0=%016" PRIX64 " args",
                    watch_ip_addr[w], watch_ip_hits[w], m->ninsts, m->br[0]);
            fprintf(stderr, "\n");
            for (unsigned r = 16; r < 48; r++)
                fprintf(stderr, "merced:    r%-2u=%016" PRIX64 "%s", r,
                        gr_read(m, r, NULL), (r % 4 == 3) ? "\n" : "  ");
            if (watch_ip_hits[w] == 1) {
                unsigned avail = m->call_history_next < MERCED_CALL_HISTORY
                               ? m->call_history_next : MERCED_CALL_HISTORY;
                unsigned first = m->call_history_next -
                                 (avail < 64 ? avail : 64);
                for (unsigned i = first; i < m->call_history_next; i++) {
                    unsigned h = i % MERCED_CALL_HISTORY;
                    fprintf(stderr, "merced:    C %s %016" PRIX64
                            ".%u -> %016" PRIX64 "\n",
                            m->call_history[h].is_return ? "ret " : "call",
                            m->call_history[h].from & ~UINT64_C(0xF),
                            (unsigned)(m->call_history[h].from & 0xF),
                            m->call_history[h].to);
                }
            }
            fflush(stderr);
        }
        break;
    }
    if (heartbeat_on) {
        static uint64_t last;
        if (m->ninsts - last >= 200000) {
            last = m->ninsts;
            fprintf(stderr, "merced: heartbeat ip=%016" PRIX64
                    " ninsts=%" PRIu64 "\n", m->ip, m->ninsts);
            fflush(stderr);
        }
    }
    /* AMI briefly clears IVA while switching from its POST-time vector table
     * to the SAL environment.  An interrupt recognized in that interval has
     * nowhere valid to go: low memory contains zero bundles, so vector 0x3000
     * immediately raises break and recursively spins at vector 0x2c00.  Keep
     * the interrupt latched until firmware installs its next IVT, just as the
     * platform interrupt gate does during this handoff.  The window includes
     * AMI's IA-32 compatibility calls: their low IP no longer identifies the
     * surrounding native firmware, but PSR.is does.  The i2000 loader also
     * enters EFI applications through the 460GX tagged high-DRAM aperture
     * (00000eXX...), before the application's IVT is installed. */
    bool ami_ivt_handoff = m->cr[CR_IVA] == 0 &&
                           ((m->ip >= UINT64_C(0x7F000000) &&
                             m->ip < UINT64_C(0x80000000)) ||
                            ((m->ip & UINT64_C(0x00000fff00000000)) ==
                             UINT64_C(0x00000e0000000000)) ||
                            (m->psr & PSR_IS));

    if (m->psr & PSR_IS) {
        /* IA-32 instructions are interruption points too.  This check must
         * precede the dispatch to merced_ia32_step(): otherwise a pending
         * PIT/ExtINT remains invisible for the entire IA-32 run and is first
         * recognized only after an instruction intercept has already moved
         * execution into the native IVT.  The SDV BIOS then saves a native
         * intercept frame as though it were the interrupted x86 frame and
         * eventually IRETs into its F3:F4/data sentinel at F000:71A8. */
        if (!ami_ivt_handoff && ext_highest_unmasked(m) >= 0 &&
            (m->psr & PSR_I) && (m->psr & PSR_IC)) {
            MercedStatus ist = deliver_fault(m, VEC_EXTINT, 0, 0, false);
            m->ninsts++;
            return ist;
        }
        if (!ami_ivt_handoff && m->timer_pending &&
            !(m->cr[CR_ITV] & (1ull << 16)) &&
            interrupt_unmasked(m, (uint8_t)m->cr[CR_ITV]) &&
            (m->psr & PSR_I) && (m->psr & PSR_IC)) {
            MercedStatus ist = deliver_fault(m, VEC_EXTINT, 0, 0, false);
            m->ninsts++;
            return ist;
        }
        return merced_ia32_step(m);
    }

    /* The generic EFI ROM's SAL entry point.  IA-64 operating systems use
     * SAL_PCI_CONFIG_{READ,WRITE}, not legacy CF8/CFC instructions, to
     * enumerate PCI.  Forward those two standard calls to the machine's
     * compatibility configuration window; other SAL functions continue in
     * the firmware implementation below. */
    if ((m->ip & UINT64_C(0x1FFFFFFFFFFFFFF0)) ==
        UINT64_C(0x00000000FFF05000)) {
        uint64_t service = gr_read(m, 32, NULL);
        static unsigned sal_debug_calls;
        if (getenv("MERCED_SAL_DEBUG") &&
            m->ninsts > UINT64_C(1000000000) && sal_debug_calls++ < 2000)
            fprintf(stderr, "merced: SAL call %016" PRIX64
                    " a1=%016" PRIX64 " a2=%016" PRIX64
                    " a3=%016" PRIX64 " ip=%016" PRIX64
                    " ninsts=%" PRIu64 "\n",
                    service, gr_read(m, 33, NULL), gr_read(m, 34, NULL),
                    gr_read(m, 35, NULL), m->ip, m->ninsts);
        if (service == UINT64_C(0x01000010) ||
            service == UINT64_C(0x01000011)) {
            uint64_t address = gr_read(m, 33, NULL);
            unsigned size = (unsigned)gr_read(m, 34, NULL);
            uint64_t value = service == UINT64_C(0x01000011)
                           ? gr_read(m, 35, NULL) : 0;
            unsigned type = (unsigned)gr_read(
                m, service == UINT64_C(0x01000011) ? 36 : 35, NULL);
            uint64_t segment, bus, dev, fun, reg;
            if (type == 0) {
                segment = (address >> 24) & 0xFF;
                bus = (address >> 16) & 0xFF;
                dev = (address >> 11) & 0x1F;
                fun = (address >> 8) & 7;
                reg = address & 0xFF;
            } else {
                segment = (address >> 28) & 0xFFFF;
                bus = (address >> 20) & 0xFF;
                dev = (address >> 15) & 0x1F;
                fun = (address >> 12) & 7;
                reg = (address & 0xFF) | (((address >> 8) & 0xF) << 8);
            }
            bool valid = (type <= 1 && segment == 0 &&
                          (size == 1 || size == 2 || size == 4) &&
                          !(reg & (size - 1)) && reg + size <= 0x100);
            if (valid) {
                const uint64_t io = UINT64_C(0x000000800010000000);
                uint32_t cfg = UINT32_C(0x80000000) |
                               ((uint32_t)bus << 16) |
                               ((uint32_t)dev << 11) |
                               ((uint32_t)fun << 8) | ((uint32_t)reg & 0xFC);
                phys_write(m, io + 0xCF8, cfg, 4);
                if (service == UINT64_C(0x01000010))
                    value = phys_read(m, io + 0xCFC + (reg & 3), size);
                else
                    phys_write(m, io + 0xCFC + (reg & 3), value, size);
            }
            gr_write(m, 8, valid ? 0 : (uint64_t)-2, 0);
            gr_write(m, 9, valid ? value : 0, 0);
            gr_write(m, 10, 0, 0);
            gr_write(m, 11, 0, 0);
            m->ip = m->br[0];
            m->taken = 1;
            m->ninsts++;
            return MERCED_OK;
        }
    }

    /* External interrupts are only architecturally recognized at instruction
     * group boundaries (SDM: interruption collection/recognition happens at
     * "interruption points", and instruction groups - the run of slots
     * between stop bits - are atomic with respect to asynchronous
     * interruptions, since real out-of-order-capable hardware has no
     * well-defined partial-completion state mid-group). merced_step() is
     * called once per SLOT, not once per bundle or group, so without this
     * check an external/timer interrupt could be recognized between two
     * slots of the same unstoppped group. Ordinary sequential code tolerates
     * this silently (it just resumes at the next slot), but it corrupts
     * software-pipelined rotating-register loops (br.ctop/pr.rot), whose
     * compiler-assumed atomicity this violates - confirmed live via
     * MERCED_DEBUG_MIDGROUP instrumentation catching dozens of real
     * mid-group timer deliveries per boot, coinciding with a WinXP IA-64
     * setup freeze in a rotating PFN-table-init loop. m->group_start is
     * only true when the current slot legitimately begins a fresh group. */
    if (!ami_ivt_handoff && m->group_start &&
        ext_highest_unmasked(m) >= 0 &&
        (m->psr & PSR_I) && (m->psr & PSR_IC)) {
        MercedStatus ist = deliver_fault(m, VEC_EXTINT, 0, 0, false);
        m->ninsts++;
        return ist;
    }

    /* Deliver a latched interval-timer external interrupt at the next
     * instruction boundary where interrupts are actually enabled. Mirrors
     * real hardware: delivery clears psr.i (via deliver_fault below), so
     * this naturally can't re-fire until firmware explicitly re-enables
     * interrupts or acks the timer through a cr.ivr read. */
    if (!ami_ivt_handoff && m->group_start &&
        m->timer_pending && !(m->cr[CR_ITV] & (1ull << 16)) &&
        interrupt_unmasked(m, (uint8_t)m->cr[CR_ITV]) &&
        (m->psr & PSR_I) && (m->psr & PSR_IC)) {
        MercedStatus ist = deliver_fault(m, VEC_EXTINT, 0, 0, false);
        m->ninsts++;
        return ist;
    }

    uint64_t bundle_va = m->ip & ~0xFull;
    unsigned slot = (unsigned)(m->ip & 0xF);
    uint64_t pa;
    MercedStatus st;

    if (slot > 2) return mhalt(m, "bad IP slot %u", slot);
    if (r18_debug_on &&
        bundle_va == UINT64_C(0xE0000000831AF8F0) && slot == 0) {
        static unsigned r18_debug;
        if (r18_debug++ < 5) {
            uint8_t nn;
            uint64_t r18 = gr_read(m, 18, &nn), r33 = gr_read(m, 33, &nn);
            fprintf(stderr, "merced: r18-tbl r18=%016" PRIX64
                    " r26=%016" PRIX64 " r27=%016" PRIX64
                    " r28=%016" PRIX64 " r29=%016" PRIX64
                    " r30=%016" PRIX64 " r33=%016" PRIX64
                    " ninsts=%" PRIu64 "\n",
                    r18, gr_read(m, 26, &nn), gr_read(m, 27, &nn),
                    gr_read(m, 28, &nn), gr_read(m, 29, &nn),
                    gr_read(m, 30, &nn), r33, m->ninsts);
            for (int idx = 0; idx < 8; idx++) {
                uint64_t va2 = r18 + (uint64_t)idx * 8, dpa;
                MercedStatus dst;
                if (va_translate(m, va2, false, false, ISR_R, &dpa, &dst))
                    fprintf(stderr, "  r18[%d] = va=%016" PRIX64
                            " -> %016" PRIX64 "\n", idx, va2,
                            phys_read(m, dpa, 8));
            }
        }
    }
    if (r48_debug_on &&
        bundle_va == UINT64_C(0xE0000000831B0580) && slot == 0) {
        static unsigned r48_debug;
        if (r48_debug++ < 3) {
            uint8_t nn;
            uint64_t r49 = gr_read(m, 49, &nn), r50 = gr_read(m, 50, &nn);
            fprintf(stderr, "merced: r48-loop r26=%016" PRIX64
                    " r27=%016" PRIX64 " r48=%016" PRIX64
                    " r49=%016" PRIX64 " r50=%016" PRIX64
                    " ninsts=%" PRIu64 "\n",
                    gr_read(m, 26, &nn), gr_read(m, 27, &nn),
                    gr_read(m, 48, &nn), r49, r50, m->ninsts);
            for (int off = -32; off < 32; off += 8) {
                uint64_t va2 = (r49 + off) & ~7ull, dpa;
                MercedStatus dst;
                if (va_translate(m, va2, false, false, ISR_R, &dpa, &dst))
                    fprintf(stderr, "  r49%+d = va=%016" PRIX64
                            " -> %016" PRIX64 "\n", off, va2,
                            phys_read(m, dpa, 8));
            }
            for (int off = -32; off < 32; off += 8) {
                uint64_t va2 = (r50 + off) & ~7ull, dpa;
                MercedStatus dst;
                if (va_translate(m, va2, false, false, ISR_R, &dpa, &dst))
                    fprintf(stderr, "  r50%+d = va=%016" PRIX64
                            " -> %016" PRIX64 "\n", off, va2,
                            phys_read(m, dpa, 8));
            }
        }
    }
    uint64_t lo, hi;
    unsigned tmpl;
    uint64_t slots[3];
    MercedTranslationCache *tc = translation_cache_on
                               ? m->translation_cache : NULL;
    MercedTbEntry *tb = tc ? &tc->entry[tb_index(bundle_va)] : NULL;
    /* Slots 1/2 normally reuse the bundle just consumed by slot 0.  Test
     * this compact CPU-local cache before hashing into the much larger TB
     * table; it keeps the common two-of-three accesses in L1. */
    if (m->bundle_cache_valid && m->bundle_cache_translation_valid &&
        m->bundle_cache_va == bundle_va) {
        /* Slots 1/2 of a sequential bundle have the same fetch translation
         * as slot 0.  Self-modifying stores invalidate this cache in
         * phys_write(), and instruction serialization invalidates it in
         * tlb_serialize_instruction(), so no translation-affecting change
         * can leave this PA live past its architectural visibility point. */
        pa = m->bundle_cache_pa;
        lo = m->bundle_cache_lo;
        hi = m->bundle_cache_hi;
        tmpl = m->bundle_cache_tmpl;
        slots[0] = m->bundle_cache_slots[0];
        slots[1] = m->bundle_cache_slots[1];
        slots[2] = m->bundle_cache_slots[2];
    } else if (tb && tb->valid && tb->va == bundle_va &&
        tb->translation_generation == tc->translation_generation &&
        tb->code_generation == tc->page_generation[tb_page_index(tb->pa)]) {
        pa = tb->pa;
        lo = tb->lo;
        hi = tb->hi;
        tmpl = tb->tmpl;
        slots[0] = tb->slots[0];
        slots[1] = tb->slots[1];
        slots[2] = tb->slots[2];
        m->bundle_cache_valid = true;
        m->bundle_cache_translation_valid = true;
        m->bundle_cache_va = bundle_va;
        m->bundle_cache_pa = pa;
        m->bundle_cache_lo = lo;
        m->bundle_cache_hi = hi;
        m->bundle_cache_tmpl = (uint8_t)tmpl;
        m->bundle_cache_slots[0] = slots[0];
        m->bundle_cache_slots[1] = slots[1];
        m->bundle_cache_slots[2] = slots[2];
    } else {
        /* Physical instruction mode is common in PAL/SAL and needs no TLB
         * machinery.  Keep this tiny path in the fetch loop instead of
         * entering the general translation/fault/debug path once per
         * bundle (or after each serialization point). */
        if (!(m->psr & PSR_IT)) {
            pa = bundle_va & MERCED_PHYS_MASK;
        } else if (!va_translate(m, bundle_va, true, false, ISR_X, &pa, &st)) {
            return st;   /* ITLB miss delivered (or halt) */
        }
        if (m->bundle_cache_valid && m->bundle_cache_va == bundle_va &&
            m->bundle_cache_pa == pa) {
            lo = m->bundle_cache_lo;
            hi = m->bundle_cache_hi;
            tmpl = m->bundle_cache_tmpl;
            slots[0] = m->bundle_cache_slots[0];
            slots[1] = m->bundle_cache_slots[1];
            slots[2] = m->bundle_cache_slots[2];
        } else {
            lo = phys_fetch(m, pa, 8);
            hi = phys_fetch(m, pa + 8, 8);
            tmpl = (unsigned)(lo & 0x1F);
            slots[0] = (lo >> 5) & 0x1FFFFFFFFFFull;
            slots[1] = ((lo >> 46) | (hi << 18)) & 0x1FFFFFFFFFFull;
            slots[2] = (hi >> 23) & 0x1FFFFFFFFFFull;
            m->bundle_cache_valid = true;
            m->bundle_cache_va = bundle_va;
            m->bundle_cache_pa = pa;
            m->bundle_cache_lo = lo;
            m->bundle_cache_hi = hi;
            m->bundle_cache_tmpl = (uint8_t)tmpl;
            m->bundle_cache_slots[0] = slots[0];
            m->bundle_cache_slots[1] = slots[1];
            m->bundle_cache_slots[2] = slots[2];
        }
        m->bundle_cache_translation_valid = true;
        if (tb) {
            tb->va = bundle_va;
            tb->pa = pa;
            tb->lo = lo;
            tb->hi = hi;
            tb->slots[0] = slots[0];
            tb->slots[1] = slots[1];
            tb->slots[2] = slots[2];
            tb->tmpl = (uint8_t)tmpl;
            tb->translation_generation = tc->translation_generation;
            tb->code_generation = tc->page_generation[tb_page_index(pa)];
            tb->valid = true;
        }
    }
    const char *units = bundle_units[tmpl];
    if (units[0] == '?') {
        /* A reserved bundle template is an Illegal Operation, which the
         * architecture reports through the General Exception vector with
         * ISR.code 0 - not a machine stop.  Software relies on this: an OS
         * that branches into data, or a speculative path that reaches
         * unwritten memory (which reads as all-ones, template 0x1F), expects
         * to take the fault and recover.  Halting here turned every such
         * case into a dead emulator.  Reference: IA64_EXCP_RESERVED_TEMPLATE
         * and IA64_EXCP_ILLEGAL both vector to 0x5400. */
        return deliver_fault(m, VEC_GENERAL, 0, 0, false);
    }

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
            va_translate(m, va, false, false, ISR_W, &fill_pa, &st) &&
            m->bus.fill(m->bus.ud, fill_pa, 0, len)) {
            /* Bypasses phys_write(), so the bundle-decode cache's own
             * invalidation hook never sees this - clear it explicitly on
             * the rare chance the fill overlapped the currently-cached
             * bundle's physical bytes. */
            m->bundle_cache_valid = false;
            tb_flush(m);
            gr_write(m, ra, va + len, 0);
            gr_write(m, rb, vb + len, 0);
            m->ar[AR_LC] = 0;
            m->ninsts += iterations * 3;
            if (!m->external_itc)
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
            if (!m->external_itc)
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

    /* m->group_start still reflects whether a stop (or taken branch)
     * preceded THIS slot - alloc/cover consult it during dispatch below,
     * before it gets overwritten for the next slot. */
    m->slot_stop_after = bundle_stop_after[tmpl][unit == 'X' ? 2 : slot];

    int qp = pr_read(m, (unsigned)bits(raw, 0, 6));
    m->taken = 0;
    uint64_t faults_before = m->nfaults;

    /* Keep the many one-off firmware probes entirely off the normal
     * execution path.  Individually-predictable false branches still add
     * up when this function is called billions of times. */
    if (debug_hooks_on || m->trace_n) {
    if (slot == 0 && bundle_va == UINT64_C(0xE00000008310D320) &&
        getenv("MERCED_DEBUG_FIRSTENTRY")) {
        static bool dumped;
        if (!dumped && gr_read(m, 32, NULL) == 0x8E) {
            dumped = true;
            fprintf(stderr, "merced: FIRSTENTRY-8310D320 ninsts=%" PRIu64
                    " r32=%016" PRIX64 " r33=%016" PRIX64 " r34=%016" PRIX64
                    " r35=%016" PRIX64 " b0=%016" PRIX64 "\n", m->ninsts,
                    gr_read(m, 32, NULL), gr_read(m, 33, NULL),
                    gr_read(m, 34, NULL), gr_read(m, 35, NULL), m->br[0]);
            unsigned avail = m->call_history_next < MERCED_CALL_HISTORY
                           ? m->call_history_next : MERCED_CALL_HISTORY;
            for (unsigned i = m->call_history_next - avail;
                 i < m->call_history_next; i++) {
                unsigned h = i % MERCED_CALL_HISTORY;
                fprintf(stderr, "merced:   %s %016" PRIX64 " -> %016" PRIX64 "\n",
                        m->call_history[h].is_return ? "ret " : "call",
                        m->call_history[h].from & ~UINT64_C(0xF),
                        m->call_history[h].to & ~UINT64_C(0xF));
            }
            fflush(stderr);
        }
    }
    if (slot == 1 && bundle_va == UINT64_C(0xE0000000812734D0) &&
        getenv("MERCED_DEBUG_TBITCHECK")) {
        static uint64_t n;
        n++;
        if (n <= 20 || (n % 5000) == 0) {
            uint64_t r8v = gr_read(m, 8, NULL);
            fprintf(stderr, "merced: TBITCHECK #%" PRIu64 " ninsts=%" PRIu64
                    " r8=%016" PRIX64 " bit5=%d\n", n, m->ninsts, r8v,
                    (int)((r8v >> 5) & 1));
            fflush(stderr);
        }
    }
    if (slot == 0 && bundle_va == UINT64_C(0xE0000000834F3A70) &&
        getenv("MERCED_DEBUG_CHUNKLOOP")) {
        static uint64_t n;
        n++;
        if (n <= 40 || (n % 5000) == 0) {
            fprintf(stderr, "merced: CHUNKLOOP #%" PRIu64 " ninsts=%" PRIu64
                    " r38(chunk)=%016" PRIX64 " r40(remain)=%016" PRIX64
                    " r32(buf)=%016" PRIX64 "\n", n, m->ninsts,
                    gr_read(m, 38, NULL), gr_read(m, 40, NULL),
                    gr_read(m, 32, NULL));
            fflush(stderr);
        }
    }
    if (slot == 0 && bundle_va == UINT64_C(0xE000000081273280) &&
        getenv("MERCED_DEBUG_EFISTALL")) {
        static uint64_t n;
        n++;
        if (n <= 3 || (n % 2000) == 0) {
            fprintf(stderr, "merced: EFISTALL #%" PRIu64 " ninsts=%" PRIu64
                    " b0=%016" PRIX64 " r8=%016" PRIX64 "\n", n, m->ninsts,
                    m->br[0], gr_read(m, 8, NULL));
            if (n <= 3) {
                unsigned avail = m->call_history_next < MERCED_CALL_HISTORY
                               ? m->call_history_next : MERCED_CALL_HISTORY;
                for (unsigned i = m->call_history_next - avail;
                     i < m->call_history_next; i++) {
                    unsigned h = i % MERCED_CALL_HISTORY;
                    fprintf(stderr, "merced:   %s %016" PRIX64 " -> %016"
                            PRIX64 "\n",
                            m->call_history[h].is_return ? "ret " : "call",
                            m->call_history[h].from & ~UINT64_C(0xF),
                            m->call_history[h].to & ~UINT64_C(0xF));
                }
            }
            fflush(stderr);
        }
    }
    if (slot == 0 && bundle_va == UINT64_C(0xE00000008310CA30) &&
        getenv("MERCED_DEBUG_POLLNODE")) {
        static uint64_t n;
        static uint64_t nonone_seen;
        static uint64_t r8_hist[16];
        uint64_t r8v = gr_read(m, 8, NULL);
        n++;
        if (r8v != 1 && nonone_seen < 40) {
            nonone_seen++;
            fprintf(stderr, "merced: POLLNODE-NONONE #%" PRIu64
                    " ninsts=%" PRIu64 " r8=%016" PRIX64 " r37=%016" PRIX64
                    " r39=%016" PRIX64 "\n", n, m->ninsts, r8v,
                    gr_read(m, 37, NULL), gr_read(m, 39, NULL));
        }
        r8_hist[(unsigned)(r8v & 0xF)]++;
        if (n <= 60 || (n % 20000) == 0) {
            fprintf(stderr, "merced: POLLNODE #%" PRIu64 " ninsts=%" PRIu64
                    " r8=%016" PRIX64 " r37=%016" PRIX64 " r39=%016" PRIX64
                    " r40=%016" PRIX64 " r8hist[0..3]=%" PRIu64 ",%" PRIu64
                    ",%" PRIu64 ",%" PRIu64 "\n", n, m->ninsts, r8v,
                    gr_read(m, 37, NULL), gr_read(m, 39, NULL),
                    gr_read(m, 40, NULL), r8_hist[0], r8_hist[1],
                    r8_hist[2], r8_hist[3]);
            fflush(stderr);
        }
    }
    if (slot == 0 && bundle_va == UINT64_C(0xE0000000830245C0) &&
        getenv("MERCED_DEBUG_WAIT4_RESULT")) {
        static uint64_t n;
        uint64_t r8v = gr_read(m, 8, NULL);
        n++;
        if (n <= 20 || (n % 1000) == 0) {
            fprintf(stderr, "merced: WAIT4-RESULT #%" PRIu64 " ninsts=%"
                    PRIu64 " r8(status)=%016" PRIX64 "\n", n, m->ninsts, r8v);
            fflush(stderr);
        }
    }
    if (slot == 1 && bundle_va == UINT64_C(0xE00000008310E040) &&
        getenv("MERCED_DEBUG_R57FLAG")) {
        static uint64_t n, nonzero_seen;
        uint64_t r57v = gr_read(m, 57, NULL);
        n++;
        if (r57v != 0 && nonzero_seen < 20) {
            nonzero_seen++;
            fprintf(stderr, "merced: R57FLAG-NONZERO #%" PRIu64 " ninsts=%"
                    PRIu64 " r57=%016" PRIX64 "\n", n, m->ninsts, r57v);
            fflush(stderr);
        }
        if (n <= 20 || (n % 1000) == 0) {
            fprintf(stderr, "merced: R57FLAG #%" PRIu64 " ninsts=%" PRIu64
                    " r57=%016" PRIX64 " nonzero_seen=%" PRIu64 "\n", n,
                    m->ninsts, r57v, nonzero_seen);
            fflush(stderr);
        }
    }
    if (bundle_va == UINT64_C(0xE0000000835532D0) &&
        getenv("MERCED_DEBUG_5532D0")) {
        static unsigned n;
        if (n++ < 32) {
            unsigned r2 = (unsigned)bits(raw, 13, 7);
            unsigned r3 = (unsigned)bits(raw, 20, 7);
            uint8_t n2 = 0, n3 = 0;
            uint64_t v2 = gr_read(m, r2, &n2);
            uint64_t v3 = gr_read(m, r3, &n3);
            fprintf(stderr, "merced: 5532D0.%u unit=%c raw=%011" PRIX64
                    " qp=p%u=%d r2=r%u=%016" PRIX64 "/nat%u"
                    " r3=r%u=%016" PRIX64 "/nat%u cfm=%016" PRIX64
                    " ninsts=%" PRIu64 "\n", slot, unit, raw,
                    (unsigned)bits(raw, 0, 6), qp, r2, v2, n2,
                    r3, v3, n3, m->cfm, m->ninsts);
            fflush(stderr);
        }
    }
    if (debug_b520_fault_active && getenv("MERCED_DEBUG_XP_HANDLER") &&
        bundle_va >= UINT64_C(0xE00000008301BB00) &&
        bundle_va < UINT64_C(0xE00000008301BE60)) {
        static unsigned n;
        if (n++ < 600) {
            fprintf(stderr, "GEMU-XP ip=%016" PRIX64
                    " pr=%016" PRIX64 " r30=%016" PRIX64
                    " r31=%016" PRIX64 " r36=%016" PRIX64
                    " r40=%016" PRIX64 " r45=%016" PRIX64
                    " r74=%016" PRIX64 " r76=%016" PRIX64
                    " nat31=%u nat74=%u nat76=%u\n",
                    bundle_va | slot, m->pr, gr_read(m, 30, NULL),
                    gr_read(m, 31, NULL), gr_read(m, 36, NULL),
                    gr_read(m, 40, NULL), gr_read(m, 45, NULL),
                    gr_read(m, 74, NULL), gr_read(m, 76, NULL),
                    gr_nat(m, 31), gr_nat(m, 74), gr_nat(m, 76));
        }
    }
    if (debug_b520_fault_active &&
        bundle_va == UINT64_C(0xE00000008301BB00) && slot == 0 &&
        getenv("MERCED_DEBUG_B520_ENTRY")) {
        static bool entry_dumped;
        if (!entry_dumped) {
            entry_dumped = true;
            fprintf(stderr, "merced: B520 entered software walker; preceding dispatch\n");
            merced_dump_trace(m, 512, stderr);
            fflush(stderr);
        }
    }

    if (trace_history_on &&
        (!capture_hi || (bundle_va >= capture_lo && bundle_va < capture_hi))) {
        unsigned hist = m->trace_history_next % MERCED_TRACE_HISTORY;
        unsigned hist_ext = m->trace_history_next % MERCED_TRACE_EXT_HISTORY;
        m->trace_history_next++;
        m->trace_history[hist].ip = bundle_va | slot;
        m->trace_history[hist].raw = raw;
        m->trace_history[hist].b0 = m->br[0];
        m->trace_history[hist].pr = m->pr;
        m->trace_history[hist].unit = (uint8_t)unit;
        m->trace_history[hist].qp = (uint8_t)qp;
        if (trace_regs_on) {
            m->trace_history_ext[hist_ext].src2 =
                gr_read(m, (unsigned)bits(raw, 13, 7), NULL);
            m->trace_history_ext[hist_ext].src3 =
                gr_read(m, (unsigned)bits(raw, 20, 7), NULL);
            m->trace_history_ext[hist_ext].r8 = gr_read(m, 8, NULL);
            m->trace_history_ext[hist_ext].r25 = gr_read(m, 25, NULL);
            m->trace_history_ext[hist_ext].r32 = gr_read(m, 32, NULL);
            m->trace_history_ext[hist_ext].r33 = gr_read(m, 33, NULL);
            m->trace_history_ext[hist_ext].r34 = gr_read(m, 34, NULL);
            m->trace_history_ext[hist_ext].r35 = gr_read(m, 35, NULL);
            m->trace_history_ext[hist_ext].r36 = gr_read(m, 36, NULL);
        }
    }

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

    if (zero_loop_debug_on &&
        bundle_va >= UINT64_C(0xE000000083006280) &&
        bundle_va < UINT64_C(0xE000000083006340)) {
        static unsigned zero_loop_debug;
        if (zero_loop_debug++ < 256)
            fprintf(stderr, "merced: ZERO-LOOP ip=%016" PRIX64 ".%u"
                    " unit=%c raw=%011" PRIX64 " qp=p%u=%d"
                    " lc=%" PRIu64 " ec=%" PRIu64
                    " r15=%016" PRIX64 " r16=%016" PRIX64
                    " r25=%016" PRIX64 " r26=%016" PRIX64 "\n",
                    bundle_va, slot, unit, (uint64_t)raw,
                    (unsigned)bits(raw, 0, 6), qp,
                    m->ar[AR_LC], m->ar[AR_EC],
                    gr_read(m, 15, NULL), gr_read(m, 16, NULL),
                    gr_read(m, 25, NULL), gr_read(m, 26, NULL));
    }

    if (ata_flow_debug_on &&
        (bundle_va == UINT64_C(0x7F59D1C0) || bundle_va == UINT64_C(0x7F59D210) ||
         bundle_va == UINT64_C(0x7F59D240) || bundle_va == UINT64_C(0x7F59D250) ||
         bundle_va == UINT64_C(0x7F59D270) ||
         bundle_va == UINT64_C(0x7F59D320) || bundle_va == UINT64_C(0x7F59D360) ||
         bundle_va == UINT64_C(0x7F59D390) || bundle_va == UINT64_C(0x7F59D450) ||
         bundle_va == UINT64_C(0x7F59D4A0) || bundle_va == UINT64_C(0x7F59D4C0) ||
         bundle_va == UINT64_C(0x7F59D550) || bundle_va == UINT64_C(0x7F59D580) ||
         bundle_va == UINT64_C(0x7F59D650)) &&
        slot == 2 && gr_read(m, 32, NULL) >= 0x170 && gr_read(m, 32, NULL) <= 0x177) {
        static unsigned ata_flow_debug, d1c0_hits;
        bool is_loop_body = bundle_va == UINT64_C(0x7F59D1C0) ||
                             bundle_va == UINT64_C(0x7F59D210) ||
                             bundle_va == UINT64_C(0x7F59D240);
        static unsigned last_n;
        if (bundle_va == UINT64_C(0x7F59D1C0)) last_n = d1c0_hits++;
        unsigned n = last_n;
        if ((!is_loop_body || n >= 990) && ata_flow_debug++ < 3000)
            fprintf(stderr, "merced: ATA-FLOW ip=%016" PRIX64 " n=%u"
                    " r8=%016" PRIX64 " r15=%016" PRIX64 " r16=%016" PRIX64
                    " r18=%016" PRIX64 " r19=%016" PRIX64 " r9=%016" PRIX64
                    " ninsts=%" PRIu64 "\n",
                    bundle_va, n, gr_read(m, 8, NULL), gr_read(m, 15, NULL),
                    gr_read(m, 16, NULL), gr_read(m, 18, NULL),
                    gr_read(m, 19, NULL), gr_read(m, 9, NULL), m->ninsts);
    }

    if (flash_loop_ret_debug_on &&
        bundle_va == UINT64_C(0x7FE7E9D0) && slot == 2 &&
        m->ninsts > 5000000000ull) {
        static bool traced;
        if (!traced) {
            traced = true;
            fprintf(stderr, "merced: block-scan loop returning, b0=%016"
                    PRIX64 " r8=%016" PRIX64 " ninsts=%" PRIu64
                    " call history:\n", m->br[0], gr_read(m, 8, NULL),
                    m->ninsts);
            merced_dump_calls(m, 24, stderr);
            m->trace_n = 200;
        }
    }

    if (cdb_validator_debug_on && bundle_va == UINT64_C(0x7FE980A0) &&
        slot == 0 && m->ninsts > 6000000000ull) {
        static unsigned hits;
        unsigned h = hits++;
        if (h < 5 || h % 5000 == 0)
            fprintf(stderr, "merced: VALIDATOR entry #%u r32(struct ptr)=%016"
                    PRIX64 " ninsts=%" PRIu64 "\n", h, gr_read(m, 32, NULL),
                    m->ninsts);
    }
    if (cdb_validator_debug_on && bundle_va == UINT64_C(0x7FE980E0) &&
        slot == 2 && m->ninsts > 6000000000ull) {
        static unsigned hits;
        unsigned h = hits++;
        if (h < 20 || h % 5000 == 0)
            fprintf(stderr, "merced: VALIDATOR check1 #%u r27(field ptr)=%016"
                    PRIX64 " r26(raw16)=%016" PRIX64 " r24(masked)=%016"
                    PRIX64 " r23(expect)=%016" PRIX64 " ninsts=%" PRIu64 "\n",
                    h, gr_read(m, 27, NULL), gr_read(m, 26, NULL),
                    gr_read(m, 24, NULL), gr_read(m, 23, NULL), m->ninsts);
    }
    if (cdb_validator_debug_on && bundle_va == UINT64_C(0x7FE98120) &&
        slot == 2 && m->ninsts > 6000000000ull) {
        static unsigned hits;
        unsigned h = hits++;
        if (h < 20 || h % 5000 == 0)
            fprintf(stderr, "merced: VALIDATOR check2 #%u r19(field ptr)=%016"
                    PRIX64 " r18(raw16)=%016" PRIX64 " r17(masked &0x10)=%016"
                    PRIX64 " ninsts=%" PRIu64 "\n",
                    h, gr_read(m, 19, NULL), gr_read(m, 18, NULL),
                    gr_read(m, 17, NULL), m->ninsts);
    }
    if (cdb_validator_debug_on && bundle_va == UINT64_C(0x7FE98170) &&
        slot == 0 && m->ninsts > 6000000000ull) {
        static unsigned hits;
        unsigned h = hits++;
        if (h < 20 || h % 5000 == 0)
            fprintf(stderr, "merced: VALIDATOR check3 #%u r10(field ptr)=%016"
                    PRIX64 " r9(raw16)=%016" PRIX64 " r8(masked &0xE)=%016"
                    PRIX64 " ninsts=%" PRIu64 "\n",
                    h, gr_read(m, 10, NULL), gr_read(m, 9, NULL),
                    gr_read(m, 8, NULL), m->ninsts);
    }
    if (cdb_validator_debug_on && bundle_va == UINT64_C(0x7FE948C0) &&
        slot == 0 && m->ninsts > 6000000000ull) {
        static unsigned hits;
        unsigned h = hits++;
        if (h < 20 || h % 5000 == 0)
            fprintf(stderr, "merced: VALIDATOR RETURN #%u r8(retval)=%016"
                    PRIX64 " ninsts=%" PRIu64 "\n", h, gr_read(m, 8, NULL),
                    m->ninsts);
    }

    /* Production i2000 firmware CDB/FMM error propagation path.  These
     * addresses are deliberately only diagnostics: unlike the older SDV
     * image, fwver130 does not use the validator entry points above. */
    if (cdb_validator_debug_on && m->ninsts > 7200000000ull &&
        (bundle_va == UINT64_C(0x7FEACF30) ||
         bundle_va == UINT64_C(0x7FEAD1D0) ||
         bundle_va == UINT64_C(0x7FEAD1E0) ||
         bundle_va == UINT64_C(0x7FEAD650) ||
         bundle_va == UINT64_C(0x7FEAF130) ||
         bundle_va == UINT64_C(0x7FEAF140) ||
         bundle_va == UINT64_C(0x7FEAF1E0))) {
        static unsigned cdb_path_hits;
        unsigned h = cdb_path_hits++;
        if (h < 120)
            fprintf(stderr, "merced: CDB-PATH #%u ip=%016" PRIX64
                    ":%u b0=%016" PRIX64 " r8=%016" PRIX64
                    " r32=%016" PRIX64 " r33=%016" PRIX64
                    " r34=%016" PRIX64 " r35=%016" PRIX64
                    " r36=%016" PRIX64 " r37=%016" PRIX64
                    " r38=%016" PRIX64 " r39=%016" PRIX64
                    " r40=%016" PRIX64 " pr=%016" PRIX64
                    " ninsts=%" PRIu64 "\n",
                    h, bundle_va, slot, m->br[0], gr_read(m, 8, NULL),
                    gr_read(m, 32, NULL), gr_read(m, 33, NULL),
                    gr_read(m, 34, NULL), gr_read(m, 35, NULL),
                    gr_read(m, 36, NULL), gr_read(m, 37, NULL),
                    gr_read(m, 38, NULL), gr_read(m, 39, NULL),
                    gr_read(m, 40, NULL), m->pr, m->ninsts);
    }

    if (atapi_trap_debug_on && bundle_va == UINT64_C(0x7FE7CAA0) && slot == 0) {
        static bool dumped;
        if (!dumped) {
            dumped = true;
            fprintf(stderr, "merced: FLAGSETTER entered, b0(return addr)=%016"
                    PRIX64 " ninsts=%" PRIu64 " call history:\n", m->br[0],
                    m->ninsts);
            merced_dump_calls(m, 30, stderr);
        }
    }
    if (atapi_trap_debug_on && bundle_va == UINT64_C(0x7FE7AF80) && slot == 0) {
        uint64_t dst = gr_read(m, 33, NULL);
        if (dst == UINT64_C(0x7FEF7F66)) {
            static unsigned hits;
            unsigned h = hits++;
            if (h < 500)
                fprintf(stderr, "merced: TYPEBYTE-SRC #%u src_offset(r32)="
                        "%016" PRIX64 " ninsts=%" PRIu64 "\n", h,
                        gr_read(m, 32, NULL), m->ninsts);
        }
    }
    if (atapi_trap_debug_on && bundle_va == UINT64_C(0x7FE7AFF0) && slot == 0) {
        uint64_t dst = gr_read(m, 33, NULL);
        if (dst == UINT64_C(0x7FEF7F66)) {
            static unsigned hits;
            unsigned h = hits++;
            if (h < 500)
                fprintf(stderr, "merced: TYPEBYTE-VAL #%u value(r2)=%016"
                        PRIX64 " ninsts=%" PRIu64 "\n", h,
                        gr_read(m, 2, NULL), m->ninsts);
        }
    }
    if (atapi_trap_debug_on &&
        (bundle_va == UINT64_C(0x7FEF0FE0) || bundle_va == UINT64_C(0x7FEF1000) ||
         bundle_va == UINT64_C(0x7FEF1020) || bundle_va == UINT64_C(0x7FEF1070)) &&
        slot == 2) {
        static unsigned hits;
        unsigned h = hits++;
        if (h < 20)
            fprintf(stderr, "merced: ATAPI-TRAP call-79220 #%u from=%016" PRIX64
                    " arg_r37=%016" PRIX64 " ninsts=%" PRIu64 "\n", h,
                    bundle_va, gr_read(m, 37, NULL), m->ninsts);
    }
    if (atapi_trap_debug_on && bundle_va == UINT64_C(0x7FEF1050) &&
        slot == 2) {
        static unsigned hits;
        unsigned h = hits++;
        if (h < 20)
            fprintf(stderr, "merced: ATAPI-TRAP call-792c0 #%u arg_r37=%016"
                    PRIX64 " arg_r38=%016" PRIX64 " arg_r39=%016" PRIX64
                    " ninsts=%" PRIu64 "\n", h, gr_read(m, 37, NULL),
                    gr_read(m, 38, NULL), gr_read(m, 39, NULL), m->ninsts);
    }
    if (atapi_trap_debug_on && bundle_va == UINT64_C(0x7FEF1090) &&
        slot == 0) {
        static bool dumped;
        if (!dumped) {
            dumped = true;
            fprintf(stderr, "merced: ATAPI-TRAP entered infinite spin at "
                    "0x7FEF1090, ninsts=%" PRIu64 " b0=%016" PRIX64
                    " call history:\n", m->ninsts, m->br[0]);
            merced_dump_calls(m, 64, stderr);
        }
    }

    if (cdb_iter_debug_on && bundle_va == UINT64_C(0x7FE94060) &&
        slot == 1 && m->ninsts >= cdb_iter_after_ninsts) {
        static unsigned hits;
        unsigned h = hits++;
        if (h < 3)
            fprintf(stderr, "merced: ITER case9-recptr #%u ctx_r32=%016" PRIX64
                    " rec_ptr_r26=%016" PRIX64 " ninsts=%" PRIu64 "\n", h,
                    gr_read(m, 32, NULL), gr_read(m, 26, NULL), m->ninsts);
    }
    if (cdb_iter_debug_on && bundle_va == UINT64_C(0x7FE94090) &&
        slot == 0 && m->ninsts >= cdb_iter_after_ninsts) {
        static unsigned hits;
        unsigned h = hits++;
        if (h < 8)
            fprintf(stderr, "merced: ITER case9-typecheck #%u r24(rec_type)="
                    "%016" PRIX64 " r22(target_type)=%016" PRIX64
                    " ninsts=%" PRIu64 "\n", h, gr_read(m, 24, NULL),
                    gr_read(m, 22, NULL), m->ninsts);
    }
    if (cdb_iter_debug_on && bundle_va == UINT64_C(0x7FE940D0) &&
        slot == 0 && m->ninsts >= cdb_iter_after_ninsts) {
        static unsigned hits;
        unsigned h = hits++;
        if (h < 8)
            fprintf(stderr, "merced: ITER case9-statuscheck #%u r18(raw16)="
                    "%016" PRIX64 " r17(masked0xE)=%016" PRIX64
                    " ninsts=%" PRIu64 "\n", h, gr_read(m, 18, NULL),
                    gr_read(m, 17, NULL), m->ninsts);
    }
    if (cdb_iter_debug_on && bundle_va == UINT64_C(0x7FE94240) &&
        slot == 0 && m->ninsts >= cdb_iter_after_ninsts) {
        static unsigned hits;
        unsigned h = hits++;
        if (h < 20)
            fprintf(stderr, "merced: ITER case9-bytecmp #%u r21(target_byte)="
                    "%016" PRIX64 " r10(rec_byte)=%016" PRIX64
                    " ninsts=%" PRIu64 "\n", h, gr_read(m, 21, NULL),
                    gr_read(m, 10, NULL), m->ninsts);
    }
    if (cdb_iter_debug_on && bundle_va == UINT64_C(0x7FE93F70) &&
        slot == 0 && m->ninsts >= cdb_iter_after_ninsts) {
        static unsigned hits;
        unsigned h = hits++;
        if (h < 5)
            fprintf(stderr, "merced: ITER dispatch-case-target #%u r29(handler)="
                    "%016" PRIX64 " ninsts=%" PRIu64 "\n", h,
                    gr_read(m, 29, NULL), m->ninsts);
    }
    if (cdb_iter_debug_on && bundle_va == UINT64_C(0x7FE94A40) &&
        slot == 0 && m->ninsts >= cdb_iter_after_ninsts) {
        static unsigned hits;
        unsigned h = hits++;
        if (h < 60 || h % 2000 == 0)
            fprintf(stderr, "merced: ITER search45c0 #%u ret_r8=%016" PRIX64
                    " ninsts=%" PRIu64 "\n", h, gr_read(m, 8, NULL), m->ninsts);
    }
    if (cdb_iter_debug_on && bundle_va == UINT64_C(0x7FE94A80) &&
        slot == 2 && m->ninsts >= cdb_iter_after_ninsts) {
        static unsigned hits;
        unsigned h = hits++;
        if (h < 60 || h % 2000 == 0)
            fprintf(stderr, "merced: ITER dispatch93ea0-call #%u arg_r39(->r33)="
                    "%016" PRIX64 " ninsts=%" PRIu64 "\n", h,
                    gr_read(m, 39, NULL), m->ninsts);
    }
    if (cdb_iter_debug_on && bundle_va == UINT64_C(0x7FE94A90) &&
        slot == 0 && m->ninsts >= cdb_iter_after_ninsts) {
        static unsigned hits;
        unsigned h = hits++;
        if (h < 60 || h % 2000 == 0)
            fprintf(stderr, "merced: ITER dispatch93ea0-ret #%u ret_r8=%016"
                    PRIX64 " ninsts=%" PRIu64 "\n", h, gr_read(m, 8, NULL),
                    m->ninsts);
    }

    if (cdb_force_clear_on && bundle_va == UINT64_C(0x7FE7D8C0) &&
        slot == 0 && m->ninsts > 5000000000ull) {
        static unsigned n;
        if (n++ < 2000) {
            if (n < 20 || n % 100 == 0)
                fprintf(stderr, "merced: CDB_FORCE_CLEAR forcing [0x7FEF61F0]=0"
                    " ninsts=%" PRIu64 "\n", m->ninsts);
            m->bus.write(m->bus.ud, UINT64_C(0x7FEF61F0), 0, 4);
        }
    }

    if (cdb_d8a0_debug_on && m->ninsts > 5000000000ull &&
        (bundle_va == UINT64_C(0x7FE7D8D0) || bundle_va == UINT64_C(0x7FE7D930) ||
         bundle_va == UINT64_C(0x7FE7D8E0) || bundle_va == UINT64_C(0x7FE7D940)) &&
        slot == 2) {
        static unsigned n;
        if (n++ < 40)
            fprintf(stderr, "merced: CDB-D8A0 ip=%016" PRIX64 " r8=%016" PRIX64
                    " r20=%016" PRIX64 " r21(flag addr)=%016" PRIX64
                    " ninsts=%" PRIu64 "\n",
                    bundle_va, gr_read(m, 8, NULL), gr_read(m, 20, NULL),
                    gr_read(m, 21, NULL), m->ninsts);
    }

    if (flash_cmp_debug_on && bundle_va == UINT64_C(0x7FE7D1A0) &&
        slot == 2) {
        static unsigned n;
        if (n++ < 5)
            fprintf(stderr, "merced: FLASH-CMP r8(raw id fn ret)=%016" PRIX64
                    " r21(compared value)=%016" PRIX64 " r20(magic const)=%016"
                    PRIX64 " ninsts=%" PRIu64 "\n",
                    gr_read(m, 8, NULL), gr_read(m, 21, NULL),
                    gr_read(m, 20, NULL), m->ninsts);
    }

    if (ata_portfn_debug_on &&
        (bundle_va == UINT64_C(0x7F5A41B0) || bundle_va == UINT64_C(0x7F5A4200)) &&
        slot == 0) {
        uint64_t port_arg = gr_read(m, 32, NULL);
        bool is_data = bundle_va == UINT64_C(0x7F5A4200);
        if (port_arg >= 0x170 && port_arg <= 0x177) {
            static unsigned ata_status_debug;
            static unsigned ata_data_debug;
            unsigned *ctr = is_data ? &ata_data_debug : &ata_status_debug;
            unsigned cap = is_data ? 5000 : 20;
            if ((*ctr)++ < cap)
                fprintf(stderr, "merced: ATA-PORTFN entry ip=%016" PRIX64
                        " (%s) r32(port-arg)=%016" PRIX64 " ninsts=%" PRIu64 "\n",
                        bundle_va, is_data ? "ld2-data" : "ld1-status",
                        port_arg, m->ninsts);
        }
    }
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
    if (!m->external_itc)
        itc_advance(m, 1);
    if (m->nfaults == faults_before)
        m->last_successful_bundle = bundle_va;

    if (m->taken) {
        /* A taken branch or a delivered fault/interruption both always
         * begin a new instruction group at their target, regardless of
         * the stop bit the source slot did or didn't have. */
        m->group_start = 1;
        m->taken = 0;
        return MERCED_OK;                     /* branch/fault redirected IP */
    }
    m->group_start = m->slot_stop_after;

    /* advance within bundle: X consumes slots 1+2 */
    if (unit == 'X' || slot == 2)
        m->ip = bundle_va + 16;
    else
        m->ip = bundle_va | (slot + 1);
    return MERCED_OK;
}

MercedStatus merced_run(Merced *m, unsigned max_slots, unsigned *executed) {
    unsigned n = 0;
    MercedStatus st = MERCED_OK;

    while (n < max_slots) {
        st = merced_step(m);
        n++;
        if (st != MERCED_OK)
            break;
    }
    if (executed)
        *executed = n;
    return st;
}

/* ── Debug dump ──────────────────────────────────────────────────────────── */

void merced_dump_trace(const Merced *m, unsigned count, FILE *out) {
    unsigned available = m->trace_history_next < MERCED_TRACE_HISTORY
                       ? m->trace_history_next : MERCED_TRACE_HISTORY;
    if (count > available) count = available;
    unsigned first = m->trace_history_next - count;
    for (unsigned i = first; i < m->trace_history_next; i++) {
        unsigned h = i % MERCED_TRACE_HISTORY;
        /* The register-value fields only exist for the most recent
         * MERCED_TRACE_EXT_HISTORY entries (see merced.h) - older entries
         * in a deep dump print "." placeholders for them instead. */
        bool has_ext = trace_regs_on &&
                       m->trace_history_next - i <= MERCED_TRACE_EXT_HISTORY;
        char s2[24], s3[24], r8[24], r25[24], r32[24], r33[24], r34[24],
             r35[24], r36[24];
        if (has_ext) {
            unsigned he = i % MERCED_TRACE_EXT_HISTORY;
            snprintf(s2, sizeof(s2), "%016" PRIX64, m->trace_history_ext[he].src2);
            snprintf(s3, sizeof(s3), "%016" PRIX64, m->trace_history_ext[he].src3);
            snprintf(r8, sizeof(r8), "%016" PRIX64, m->trace_history_ext[he].r8);
            snprintf(r25, sizeof(r25), "%016" PRIX64, m->trace_history_ext[he].r25);
            snprintf(r32, sizeof(r32), "%016" PRIX64, m->trace_history_ext[he].r32);
            snprintf(r33, sizeof(r33), "%016" PRIX64, m->trace_history_ext[he].r33);
            snprintf(r34, sizeof(r34), "%016" PRIX64, m->trace_history_ext[he].r34);
            snprintf(r35, sizeof(r35), "%016" PRIX64, m->trace_history_ext[he].r35);
            snprintf(r36, sizeof(r36), "%016" PRIX64, m->trace_history_ext[he].r36);
        } else {
            snprintf(s2, sizeof(s2), "%s", "................");
            snprintf(s3, sizeof(s3), "%s", "................");
            snprintf(r8, sizeof(r8), "%s", "................");
            snprintf(r25, sizeof(r25), "%s", "................");
            snprintf(r32, sizeof(r32), "%s", "................");
            snprintf(r33, sizeof(r33), "%s", "................");
            snprintf(r34, sizeof(r34), "%s", "................");
            snprintf(r35, sizeof(r35), "%s", "................");
            snprintf(r36, sizeof(r36), "%s", "................");
        }
        fprintf(out, "T %016" PRIX64 ".%u %c%c raw=%011" PRIX64
                     " s2=%s s3=%s r8=%s r25=%s r32=%s r33=%s r34=%s r35=%s"
                     " r36=%s b0=%016" PRIX64 " pr=%016" PRIX64 "\n",
                (uint64_t)(m->trace_history[h].ip & ~(uint64_t)0xF),
                (unsigned)(m->trace_history[h].ip & 0xF),
                m->trace_history[h].unit,
                m->trace_history[h].qp ? ' ' : '-',
                m->trace_history[h].raw, s2, s3, r8, r25, r32, r33, r34, r35,
                r36, m->trace_history[h].b0, m->trace_history[h].pr);
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
    P("CFM sof=%u sol=%u sor=%u rrb=%u/%u/%u  bof=%u/%" PRIu64
      "  PR %016" PRIX64 "  irq-rse-depth=%u\n",
      CFM_SOF(m->cfm), CFM_SOL(m->cfm), CFM_SOR(m->cfm),
      CFM_RRB_GR(m->cfm), CFM_RRB_FR(m->cfm), CFM_RRB_PR(m->cfm),
      m->bof, m->bof_total, m->pr, m->interruption_rse_depth);
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
    P("bsp %016" PRIX64 "  bspstore %016" PRIX64 "  rnat %016" PRIX64
      "  rsc %016" PRIX64 "\n",
      m->ar[AR_BSP], m->ar[AR_BSPSTORE], m->ar[AR_RNAT], m->ar[AR_RSC]);
    P("iva %016" PRIX64 "  iip %016" PRIX64 "  ipsr %016" PRIX64 "  isr %016" PRIX64 "\n",
      m->cr[CR_IVA], m->cr[CR_IIP], m->cr[CR_IPSR], m->cr[CR_ISR]);
    P("ifa %016" PRIX64 "  itir %016" PRIX64 "  halt: %s\n",
      m->cr[CR_IFA], m->cr[CR_ITIR], m->halt_msg);
    {
        unsigned ext_count = 0;
        int ext_highest = -1;
        for (int v = 255; v >= 16; v--) {
            if ((m->external_pending[v >> 3] >> (v & 7)) & 1) {
                ext_count++;
                if (ext_highest < 0) ext_highest = v;
            }
        }
        P("itc %016" PRIX64 "  itm %016" PRIX64 "  itv %016" PRIX64
          "  timer_pending=%u external_pending=%u highest=%#x\n",
          m->ar[AR_ITC], m->cr[CR_ITM], m->cr[CR_ITV],
          m->timer_pending, ext_count, (unsigned)ext_highest);
    }
    P("tpr %016" PRIX64 "  ivr %016" PRIX64 "\n",
      m->cr[CR_TPR], m->cr[CR_IVR]);
    P("rr  %016" PRIX64 " %016" PRIX64 " %016" PRIX64 " %016" PRIX64 "\n",
      m->rr[0], m->rr[1], m->rr[2], m->rr[3]);
    P("    %016" PRIX64 " %016" PRIX64 " %016" PRIX64 " %016" PRIX64 "\n",
      m->rr[4], m->rr[5], m->rr[6], m->rr[7]);
    for (unsigned i = 0; i < MERCED_N_DTR; i++) {
        if (m->dtr[i].valid)
            P("dtr[%u] rid=%06X va=%016" PRIX64 "-%016" PRIX64
              " pa=%016" PRIX64 " ps=%u\n", i, m->dtr[i].rid,
              m->dtr[i].va_start, m->dtr[i].va_end,
              m->dtr[i].pfn_base, m->dtr[i].ps);
    }
    #undef P
}
