/*
 * Intel Itanium (Merced) IA-64 interpreter.
 *
 * Encodings from the Itanium SDM rev 2.1, cross-checked against HP Ski's
 * machine-generated tables (reference/ski/src/encodings/encoding.opcode,
 * encoding.format, encoding.imm). See merced.h for modeling simplifications.
 */

#include "merced.h"
#include "merced_fp.h"
#include "merced_msr.h"
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
#define getenv(name) (__extension__({                                   \
    /* one lookup per call site; after that a load and a branch */          \
    static const char *env_value_;                                          \
    static bool env_done_;                                                  \
    if (!env_done_) { env_value_ = merced_cached_getenv(name); env_done_ = true; } \
    env_value_; }))

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

/* ── PSR / CFM / AR / CR layout ──────────────────────────────────────────── */

#define PSR_BE   (1ull << 1)
#define PSR_UP   (1ull << 2)
#define PSR_AC   (1ull << 3)
#define PSR_MFL  (1ull << 4)
#define PSR_MFH  (1ull << 5)
#define PSR_IC   (1ull << 13)
#define PSR_I    (1ull << 14)
#define PSR_DT   (1ull << 17)
#define PSR_DFL  (1ull << 18)
#define PSR_DFH  (1ull << 19)
#define PSR_PP   (1ull << 21)
#define PSR_DI   (1ull << 22)
#define PSR_SI   (1ull << 23)
#define PSR_RT   (1ull << 27)
#define PSR_MC   (1ull << 35)
#define PSR_IS   (1ull << 34)
#define PSR_DA   (1ull << 38)
#define PSR_DD   (1ull << 39)
#define PSR_IT   (1ull << 36)
#define PSR_SS   (1ull << 40)
#define PSR_TB   (1ull << 26)
#define PSR_LP   (1ull << 25)
#define PSR_DB   (1ull << 24)
#define PSR_ID   (1ull << 37)
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
#define ISR_IR (1ull << 38) /* incomplete register frame during an RSE fill */
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
    VEC_GENERAL = 0x5400, VEC_DISABLED_FP = 0x5500,
    VEC_NAT = 0x5600, VEC_SPEC = 0x5700, VEC_UNALIGNED = 0x5A00,
    VEC_DEBUG = 0x5900, VEC_UNSUPPORTED = 0x5B00, VEC_FP_FAULT = 0x5C00,
    VEC_FP_TRAP = 0x5D00,
    VEC_IA32_INTERCEPT = 0x6A00,
};

/* one-shot warning ids */
enum {
    WARN_FLUSHRS = 1u << 0, WARN_PTC = 1u << 1, WARN_PROBE = 1u << 2,
    WARN_SEMAPHORE = 1u << 3, WARN_LFETCH_FAULT = 1u << 4,
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
    return (m->bof + idx) % MERCED_RSE_CAPACITY;
}

/* Set by a write to a stacked register beyond the current frame, an Illegal
 * Operation fault the instruction delivers once it has finished. */
static bool oof_write_pending;

/* gr_read_inl()/gr_write_inl() are the accessors' bodies, forced inline for
 * fast_op(); everything else calls the out-of-line gr_read()/gr_write(). */
static inline __attribute__((always_inline))
uint64_t gr_read_inl(Merced *m, unsigned r, uint8_t *nat) {
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

static uint64_t gr_read(Merced *m, unsigned r, uint8_t *nat) {
    return gr_read_inl(m, r, nat);
}

/* Floating-point registers share the ALAT with the integer ones, keyed from
 * here by their physical number. */
#define ALAT_FP_KEY 128u

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
        if (m->alat[i].reg >= 32 && m->alat[i].reg < ALAT_FP_KEY)
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

static inline __attribute__((always_inline))
void gr_write_inl(Merced *m, unsigned r, uint64_t v, uint8_t nat) {
    if (r == 0) return;   /* writes to r0 fault on HW; ignore here */
    /* Ordinary GR writes do not invalidate an advanced-load entry for that
     * register.  The ALAT tracks memory conflicts, not the current GR value:
     * ld.c.nc must preserve an intervening computed value when no conflicting
     * store occurred. */
    if (r < 16) { m->gr_static[r] = v; m->nat_static[r] = nat; return; }
    if (r < 32) {
        if (m->psr & PSR_BN) {
            m->gr_static[r] = v; m->nat_static[r] = nat;
        }
        else {
            m->gr_bank0[r - 16] = v; m->nat_bank0[r - 16] = nat;
        }
        return;
    }
    if (__builtin_expect(r - 32 >= CFM_SOF(m->cfm), 0)) {
        oof_write_pending = true;
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

static void gr_write(Merced *m, unsigned r, uint64_t v, uint8_t nat) {
    gr_write_inl(m, r, v, nat);
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
/* Value an application-register write leaves behind: the RSC privilege
 * clamp, a BSPSTORE that is 8-byte aligned, a RNAT without its bit 63 and
 * the 6-bit epilog count. */
static uint64_t ar_value_for_write(const Merced *m, unsigned ar3, uint64_t value) {
    switch (ar3) {
    case AR_RSC:      return rsc_value_for_write(m, value);
    case AR_BSPSTORE: return value & ~UINT64_C(7);
    case AR_RNAT:     return value & ~(UINT64_C(1) << 63);
    case AR_EC:       return value & 0x3F;
    default:          return value;
    }
}
/* Which application registers a unit may name (SDM vol. 1, table 3-3): AR0-47
 * belong to the M unit, AR64-66 to the I unit, AR48-63 and AR112-127 are
 * ignored from either and the rest are reserved. */
enum { AR_OK, AR_IGNORED, AR_RESERVED };
static int ar_class(unsigned ar, bool i_form) {
    if ((ar >= 48 && ar <= 63) || ar >= 112) return AR_IGNORED;
    if (i_form) return (ar >= 64 && ar <= 66) ? AR_OK : AR_RESERVED;
    if (ar <= 7) return AR_OK;
    switch (ar) {
    case 16: case 17: case 18: case 19: case 21: case 24: case 25: case 26:
    case 27: case 28: case 29: case 30: case 32: case 36: case 40: case 44:
    case 45:
        return AR_OK;
    default:
        return AR_RESERVED;
    }
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

/* The instruction TLB entry (TR first, then TC) translating va, if any. */
static const MercedTlbEntry *itlb_entry(Merced *m, uint64_t va) {
    uint32_t rid = (uint32_t)((m->rr[va >> 61] >> 8) & 0xFFFFFFull);
    const MercedTlbEntry *e = tlb_search(m->itr, MERCED_N_ITR, rid, va,
                                         &m->itr_hint);
    if (!e)
        e = tlb_search(m->itc, MERCED_N_TC, rid, va, &m->itc_hint);
    return e;
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
static uint64_t phys_read(Merced *m, uint64_t pa, unsigned size);
static void tlb_insert(Merced *m, MercedTlbEntry *e, uint64_t pte,
                       bool instruction);
static bool translation_insert_fields_valid(uint64_t pte, uint64_t itir);
static bool firmware_identity_pa(Merced *m, uint64_t va, uint64_t *pa);
static void xlate_flush(Merced *m);
static bool xlate_cacheable(const Merced *m, uint64_t va);
static uint64_t xlate_kind(const Merced *m, bool ifetch, uint64_t isr_access);
static bool xlate_lookup(Merced *m, uint64_t va, uint64_t kind, uint64_t *pa);
static void xlate_store(Merced *m, uint64_t va, uint64_t kind, uint64_t pa);
static void tlb_serialize_data(Merced *m);
static void tlb_serialize_instruction(Merced *m);

/* Hardware-style VHPT walk, tried on a TLB miss before falling back to the
 * software fault path. */
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

bool merced_external_pending(const Merced *m, uint8_t vector) {
    return ext_pending_test(m, vector);
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
/* Highest in-service vector (16-255), or 0 when none is in service. */
static unsigned in_service_highest(const Merced *m) {
    for (int i = 31; i >= 2; i--)
        if (m->ext_in_service[i])
            return (unsigned)(i * 8 + 31 - __builtin_clz(m->ext_in_service[i]));
    return 0;
}

static bool interrupt_unmasked(Merced *m, uint8_t vector) {
    uint64_t tpr = m->cr[CR_TPR];
    if (vector == 2)
        return true;
    /* ExtINT comes from the 8259 through the LINT0 pin, which the OS can
     * mask with cr.lrr0.m.  Windows does exactly that once its I/O SAPIC
     * driver owns interrupts; delivering the still-ticking PIT anyway
     * storms its kernel with vector 0.  (merced_reset() leaves LRR0 clear,
     * so firmware that never programs it keeps receiving ExtINT.) */
    if (vector == 0)
        return !(tpr & (UINT64_C(1) << 16)) &&
               !(m->cr[CR_LRR0] & (UINT64_C(1) << 16));
    if (tpr & (UINT64_C(1) << 16))
        return false;
    /* An in-service vector masks its own priority class and all lower ones
     * until cr.eoi (SDM Vol.2 5.8.3).  Without this, a level-triggered
     * device that's still asserted re-interrupts its own handler as soon
     * as the OS re-enables psr.i, nesting forever (WinXP Setup). */
    unsigned isv = in_service_highest(m);
    if (isv && (vector & 0xF0u) <= (isv & 0xF0u))
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
    /* Highest vector first, as before, but a byte at a time: this runs on
     * every instruction while an interrupt waits masked. */
    for (int i = 31; i >= 2; i--) {
        unsigned bits = m->external_pending[i];
        while (bits) {
            unsigned b = 31u - (unsigned)__builtin_clz(bits);
            int v = i * 8 + (int)b;
            if (interrupt_unmasked(m, (uint8_t)v))
                return v;
            bits &= ~(1u << b);
        }
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
        d[i] = (uint8_t)merced_bus_read(m, pa, 1);
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

/* Monitor helper: name the PE image (and nearest export) holding va. */
void merced_nt_identify(Merced *m, uint64_t va) {
    nt_identify(m, va, "va", false);
}

static MercedStatus deliver_fault(Merced *m, uint32_t vec, uint64_t isr,
                                  uint64_t ifa, bool set_ifa) {
    if (isr & ISR_RS)
        isr &= ~ISR_ED;
    m->nfaults++;
    /* Interruption delivery is both an instruction- and data-serialization
     * event (SDM Vol. 2, 3.1.4).  In particular, a ptc/ptr issued by a page
     * fault handler must have completed before any nested handler lookup.
     * Leaving the invalidation pending let NT immediately re-hit a cached
     * P=0 VHPT entry and fault forever at "Setup is starting Windows". */
    tlb_serialize_data(m);
    tlb_serialize_instruction(m);
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
        /* IA-32 exit marks enabled low FP state dirty (Vol.2 10.4.2).
         * Include it in saved IPSR as well as the native handler's PSR so
         * lazy context saving cannot discard FR8..FR15 after interruption. */
        if (!(m->psr & PSR_DFL)) m->psr |= PSR_MFL;
    }
    unsigned slot = ia32 ? 0 : (unsigned)(m->ip & 3);
    uint64_t interrupted_iip =
        ia32 ? (uint32_t)m->ip : (m->ip & ~UINT64_C(0xF));
    uint64_t interrupted_ipsr =
        m->psr | ((uint64_t)slot << PSR_RI_SHIFT);
    if (vec == VEC_EXTINT)
        ext_interrupt_in_service = true;
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

/* Platform Management Interrupt. A PMI is latched per vector (0 is the PMI#
 * pin, 1-3 SAL, 4-15 PAL) and taken while PSR.ic is set, whatever PSR.i says.
 * PAL programs the PALE_PMI entry point into MSR 0x571 and its PMI handler
 * reads the pending vectors from MSR 0x624 and writes back the bit it takes,
 * so the latch is write-one-to-clear. Delivery is a PAL-based interruption:
 * IIP/IPSR hold the interrupted state, IFS.v is clear, execution continues
 * in bank 0 at the PALE_PMI physical address with translation off. */
enum { MSR_PALE_PMI = 0x571, MSR_PMI_LATCH = 0x624, PMI_VECTORS = 0xffff };

void merced_pmi_raise(Merced *m, unsigned vector) {
    if (vector >= 16)
        return;
    m->msr[MSR_PMI_LATCH] |= UINT64_C(1) << vector;
    m->msr_written[MSR_PMI_LATCH] = 1;
}

static bool pmi_deliverable(const Merced *m) {
    /* A PMI taken before PAL has registered its entry point is undefined;
     * it stays pending until then. */
    return (m->msr[MSR_PMI_LATCH] & PMI_VECTORS) && (m->psr & PSR_IC) &&
           m->msr[MSR_PALE_PMI];
}

/* A PAL-based interruption: IIP/IPSR hold the interrupted state, IFS.v is
 * clear, and execution continues in bank 0 at the physical address `entry`
 * with translation off. */
static void deliver_pal_interruption(Merced *m, uint64_t entry) {
    bool ia32 = (m->psr & PSR_IS) != 0;
    if (ia32) {
        m->ar[25] = gr_read(m, 25, NULL);
        m->ar[26] = gr_read(m, 26, NULL);
        if (!(m->psr & PSR_DFL)) m->psr |= PSR_MFL;
    }
    unsigned slot = ia32 ? 0 : (unsigned)(m->ip & 3);
    m->cr[CR_IPSR] = m->psr | ((uint64_t)slot << PSR_RI_SHIFT);
    m->cr[CR_IIP] = ia32 ? (uint32_t)m->ip : (m->ip & ~UINT64_C(0xF));
    m->cr[CR_IFS] = 0;
    tlb_serialize_data(m);
    tlb_serialize_instruction(m);
    m->psr &= PSR_UP | PSR_MFL | PSR_MFH | PSR_PK | PSR_MC;
    if (m->cr[CR_DCR] & (1ull << 1))
        m->psr |= PSR_BE;
    if (m->cr[CR_DCR] & (1ull << 0))
        m->psr |= PSR_PP;
    m->psr_ic_inflight = 0;
    m->ip = entry;
    m->taken = 1;
}

static void deliver_pmi(Merced *m) {
    deliver_pal_interruption(m, m->msr[MSR_PALE_PMI]);
}

/* Machine Check Abort into PALE_CHECK, raised by the cache-array model for a
 * tag-parity or data-ECC error.  Merced's PAL_A (fw130) has three stubs under
 * PALE_RESET: 0xFFFFFF90 -> 0xFFFF8120 is PALE_CHECK (ptc.e, saves
 * r30/r31/pr in msr 0x400-0x402 like the PMI stub, treats the event as a
 * reset while msr[0x625] bit 17 is clear, and with the self-test signature
 * in msr[0x684] enters PAL_B + 0x10140 = 0xFFDF2DA0); 0xFFFFFFA0 ->
 * 0xFFFF8680 is PALE_INIT (PAL_B + 0x10130, on to SALE_ENTRY).  The
 * self-test handler (0xFFDF32D0) reads the faulting IP and PSR from IIP /
 * IPSR and rfi's, so they are written as for PMI.  PSR.mc is set: further
 * machine checks are held off until PAL clears it. */
static void deliver_mca(Merced *m) {
    deliver_pal_interruption(m, UINT64_C(0xFFFFFF90));
    m->psr |= PSR_MC;
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

/* ISR.ed describes the translation of the instruction which issued a data
 * reference, not the data translation itself.  When its code-page PTE has
 * ED set, all deferrable data faults report ISR.ed so the OS can choose its
 * deferred-exception path.  NT maps the PFN initialization loop this way;
 * omitting ED made its VHPT handler treat the fault as an ordinary miss and
 * retry the same store forever.  NaT-page consumption is the architectural
 * exception and forces ED clear (va_translate_ex()).  Only worked out when a
 * fault is delivered: it scans the instruction TLB, and every load and
 * store goes through va_translate_ex(). */
static uint64_t code_ed_isr(Merced *m, bool ifetch) {
    return (!ifetch && code_page_ed(m)) ? ISR_ED : 0;
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
    *st = MERCED_OK;
    bool is_rse = !ifetch && (isr_access & ISR_RS) != 0;
    bool on = force ||
              (ifetch ? (m->psr & PSR_IT) != 0
                      : (m->psr & (is_rse ? PSR_RT : PSR_DT)) != 0);
    /* The cache-array model needs each access's attribute (access_uc), which
     * the soft TLB does not keep, so it is bypassed while the model observes.
     * Walks and firmware-owned identity translations are write-back. */
    const bool observed = m->cache.observe;
    if (observed)
        m->cache.access_uc = false;
    if (!on) {
        *pa = va & MERCED_PHYS_MASK;
        if (observed && !ifetch)
            m->cache.access_uc = (va >> 63) != 0;   /* physical: bit 63 is UC */
        return true;
    }
    bool cacheable = xlate_cacheable(m, va);
    uint64_t kind = xlate_kind(m, ifetch, isr_access);
    if (cacheable && !observed && xlate_lookup(m, va, kind, pa))
        return true;
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
                                isr_access | spec_isr | code_ed_isr(m, ifetch), va, true);
            return false;
        } else if (walk == VHPT_TRANSLATION) {
            if (spec && (silent_probe || spec_defers(m, VEC_VHPT))) return false;
            /* The walker was enabled and the translation for the VHPT
             * entry itself was absent.  This is not an ordinary ITLB/DTLB
             * miss: vector 0 lets the OS make its page-table backing
             * resident before retrying the original reference. */
            *st = deliver_fault(m, VEC_VHPT,
                                isr_access | spec_isr | code_ed_isr(m, ifetch), va, true);
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
                            isr_access | spec_isr | code_ed_isr(m, ifetch), va, true);
        return false;
    }
    if (!(e->pte & PTE_PRESENT)) {
        if (spec && (silent_probe || spec_defers(m, VEC_PAGE_NOT_PRESENT))) return false;
        *st = deliver_fault(m, VEC_PAGE_NOT_PRESENT,
                            isr_access | spec_isr | code_ed_isr(m, ifetch), va, true);
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
            uint64_t fault_isr = isr_access | spec_isr | code_ed_isr(m, ifetch);
            if (fvec == VEC_NAT) {
                fault_isr &= ~ISR_ED;
                fault_isr |= UINT64_C(0x20); /* NaT-page consumption code */
            }
            *st = deliver_fault(m, fvec, fault_isr, va, true);
            return false;
        }
    }
    *pa = (e->pfn_base + (lookup_va - e->va_start)) & MERCED_PHYS_MASK;
    if (observed && !ifetch)
        m->cache.access_uc = ((e->pte >> PTE_MA_SHIFT) & 7) != 0;   /* ma 0 = WB */
    if (cacheable) xlate_store(m, va, kind, *pa);
    return true;
}

uint64_t merced_bus_read(Merced *m, uint64_t pa, unsigned size) {
    return m->bus.read(m->bus.ud, pa & MERCED_PHYS_MASK, size);
}

/* A data read.  While the cache-array model observes it may hold the line
 * (write-back accesses only: an uncacheable access bypasses the caches), and
 * the attribute of the translation just made is consumed. */
/* The address the cache model indexes a line by. */
static inline uint64_t model_pa(const Merced *m, uint64_t pa) {
    pa &= MERCED_PHYS_MASK;
    return m->bus.canonical ? m->bus.canonical(m->bus.ud, pa) : pa;
}

static uint64_t phys_read(Merced *m, uint64_t pa, unsigned size) {
    if (__builtin_expect(m->cache.observe, 0)) {
        bool wb = !m->cache.access_uc;
        uint64_t v;
        m->cache.access_uc = false;
        if (wb && merced_cache_load(m, model_pa(m, pa), size, &v))
            return v;
    }
    return merced_bus_read(m, pa, size);
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
static bool translation_cache_on;
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
    debug_hooks_on = trace_history_on;
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
    /* Decoded once when the entry is filled (a pure function of the bundle
     * bits), for merced_run_fast(): per slot, the handler merced_step()'s
     * unit dispatch would reach (none for nops; X-unit slots go to
     * exec_x()), its qualifying predicate register and stop bit; and
     * whether slot 0 may be one of the firmware loop idioms merced_step()
     * special-cases, or the template is reserved. */
    MercedStatus (*fn[3])(Merced *m, uint64_t raw, int qp);
    uint8_t kind[3], qpr[3], stop[3];
    uint8_t slow_slot0, reserved;
    /* For the commonest SLOT_EXEC instructions, a specialized handler in
     * fast_step() (FOP_*, FOP_GENERIC = call fn) and its operands already
     * extracted: register numbers (or predicates, for compares), a flag or
     * size byte, and the sign-extended immediate or branch displacement. */
    uint8_t op[3], a1[3], a2[3], a3[3], x[3];
    int32_t imm[3];
} MercedTbEntry;

enum { SLOT_EXEC, SLOT_NOP, SLOT_X };

enum {
    FOP_GENERIC,
    FOP_ADD,        /* r1 = r2 + r3 + imm (imm 0/1)            A1 add */
    FOP_SUB,        /* r1 = r2 - r3 - imm (imm 0/1)            A1 sub */
    FOP_AND, FOP_ANDCM, FOP_OR, FOP_XOR,  /* r1 = r2 op r3     A1 */
    FOP_SHLADD,     /* r1 = (r2 << imm) + r3                   A2 */
    FOP_ADDI,       /* r1 = imm + r3          A4 adds, A5 addl */
    FOP_SUBI,       /* r1 = imm - r3                           A3 sub */
    FOP_ANDI, FOP_ANDCMI, FOP_ORI, FOP_XORI,  /* r1 = imm op r3  A3 */
    FOP_CMP,        /* p1,p2 = r2|imm rel r3       A6/A8, x = CMPF_* */
    FOP_LD,         /* r1 = [r3], x = size | LDST_* postincrement */
    FOP_ST,         /* [r3] = r2, x = size | LDST_* postincrement */
    FOP_SETF_SIG,   /* f1 = r2 */
    FOP_GETF_SIG,   /* r1 = f2 */
    FOP_EXT,        /* r1 = zxt/sxt r3, x = x6 */
    FOP_BR_COND,    /* ip-relative br.cond */
    FOP_BR_CLOOP,   /* ip-relative br.cloop */
};

enum { CMPF_LT = 0, CMPF_LTU = 1, CMPF_EQ = 2, CMPF_REL = 3,
       CMPF_CMP4 = 4, CMPF_IMM = 8, CMPF_UNC = 16 };
enum { LDST_SIZE = 0x0F, LDST_INC_IMM = 0x10, LDST_INC_REG = 0x20 };

/* Soft TLB: recent successful data/instruction translations, one 4 KiB
 * page per entry (IA-64's smallest page), so ordinary loads and stores skip
 * the TR/TC scans.  Entries are keyed by page, access kind, the PSR bits the
 * permission checks read and the region register; anything else that can
 * change a translation (TLB inserts and purges, PKR writes) bumps
 * xlate_generation, which invalidates every entry at once. */
#define MERCED_XLATE_ENTRIES 4096u

typedef struct {
    uint64_t tag;        /* page VA | kind | XLATE_TAG_VALID */
    uint64_t psr_key;
    uint64_t rr;
    uint64_t pa_page;
    uint32_t generation;
} MercedXlateEntry;

typedef struct {
    MercedTbEntry entry[MERCED_TB_ENTRIES];
    uint32_t page_generation[MERCED_TB_PAGE_SLOTS];
    uint32_t translation_generation;
    MercedXlateEntry xlate[MERCED_XLATE_ENTRIES];
    uint32_t xlate_generation;
    /* merced_request_exit(): stop merced_run_fast() after this slot. */
    bool exit_request;
    /* The TB entry fast_step() last checked against the bundle cache.  It
     * stays right for as long as the bundle cache does: only merced_step()
     * refills that cache or rewrites TB entries, and it clears this. */
    MercedTbEntry *cur_tb;
    /* -accel jit state (merced_jit.c); survives tb_flush(), which empties
     * it instead. */
    struct MercedJit *jit;
} MercedTranslationCache;

struct MercedJit;
static void mj_flush(struct MercedJit *j);
static void mj_destroy(struct MercedJit *j);

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
    struct MercedJit *jit = tc->jit;
    memset(tc, 0, sizeof(*tc));
    tc->jit = jit;
    mj_flush(jit);
    tc->translation_generation = 1;
    tc->xlate_generation = 1;
}

/* Drop every soft-TLB entry: a TLB entry, PKR or region register changed. */
static void xlate_flush(Merced *m) {
    MercedTranslationCache *tc = m->translation_cache;
    if (tc && ++tc->xlate_generation == 0) {
        memset(tc->xlate, 0, sizeof(tc->xlate));
        tc->xlate_generation = 1;
    }
}

#define XLATE_TAG_VALID   UINT64_C(0x800)
#define XLATE_PSR_KEY     (PSR_IT | PSR_DT | PSR_RT | PSR_PK | PSR_DA | PSR_IA | \
                           (UINT64_C(3) << PSR_CPL_SHIFT))

/* Firmware owns some translations while its IVT is installed, depending on
 * IVA and on where it is executing (firmware_identity_pa() and the SAL
 * identity fallback in va_translate_ex()).  Those addresses are never cached:
 * all of them while a firmware IVA is installed, otherwise the ranges that
 * firmware_identity_pa() can claim based on the IP. */
static bool xlate_cacheable(const Merced *m, uint64_t va) {
    uint64_t iva = m->cr[CR_IVA];
    if (iva == 0 || iva == UINT64_C(0x00010000) || iva == UINT64_C(0xFFF80000))
        return false;
    if (va >= UINT64_C(0x00100000) && va < UINT64_C(0x00200000))
        return false;
    return !((va >> 61) == 4 && (va & ((UINT64_C(1) << 61) - 1)) <= UINT32_MAX);
}

/* The access kind, plus whether IVA is in region 0: data references then
 * also search the instruction TLB (va_translate_ex()). */
static uint64_t xlate_kind(const Merced *m, bool ifetch, uint64_t isr_access) {
    return (ifetch ? 1u : 0u) | (isr_access & ISR_R ? 2u : 0u) |
           (isr_access & ISR_W ? 4u : 0u) | (isr_access & ISR_X ? 8u : 0u) |
           (isr_access & ISR_RS ? 16u : 0u) |
           ((m->cr[CR_IVA] >> 61) == 0 ? 32u : 0u);
}

/* The access kind is part of the slot index as well as the tag: with the
 * page alone, a loop loading and storing the same page had its read and
 * write translations evict each other on every access. */
static MercedXlateEntry *xlate_slot(Merced *m, uint64_t va, uint64_t kind) {
    MercedTranslationCache *tc = m->translation_cache;
    return &tc->xlate[((va >> 12) ^ (va >> 61) ^ (kind * 0x9E5u)) &
                      (MERCED_XLATE_ENTRIES - 1)];
}

static bool xlate_lookup(Merced *m, uint64_t va, uint64_t kind, uint64_t *pa) {
    MercedTranslationCache *tc = m->translation_cache;
    if (!tc) return false;
    const MercedXlateEntry *x = xlate_slot(m, va, kind);
    if (x->tag == ((va & ~UINT64_C(0xFFF)) | kind | XLATE_TAG_VALID) &&
        x->generation == tc->xlate_generation &&
        x->psr_key == (m->psr & XLATE_PSR_KEY) &&
        x->rr == m->rr[va >> 61]) {
        *pa = x->pa_page | (va & 0xFFF);
        return true;
    }
    return false;
}

static void xlate_store(Merced *m, uint64_t va, uint64_t kind, uint64_t pa) {
    MercedTranslationCache *tc = m->translation_cache;
    if (!tc) return;
    MercedXlateEntry *x = xlate_slot(m, va, kind);
    x->tag = (va & ~UINT64_C(0xFFF)) | kind | XLATE_TAG_VALID;
    x->generation = tc->xlate_generation;
    x->psr_key = m->psr & XLATE_PSR_KEY;
    x->rr = m->rr[va >> 61];
    x->pa_page = pa & ~UINT64_C(0xFFF);
}

void merced_flush_translation_cache(Merced *m) { tb_flush(m); }

/* Everything a store does besides reaching memory or the cache-array model:
 * the debug watch, ALAT invalidation and dropping decoded code it overwrites. */
static void phys_write_effects(Merced *m, uint64_t pa, uint64_t v, unsigned size) {
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
}

void merced_bus_write(Merced *m, uint64_t pa, uint64_t v, unsigned size) {
    pa &= MERCED_PHYS_MASK;
    phys_write_effects(m, pa, v, size);
    m->bus.write(m->bus.ud, pa, v, size);
}

/* A data write; see phys_read() for the cache-array model. */
static void phys_write(Merced *m, uint64_t pa, uint64_t v, unsigned size) {
    pa &= MERCED_PHYS_MASK;
    phys_write_effects(m, pa, v, size);
    if (__builtin_expect(m->cache.observe, 0)) {
        bool wb = !m->cache.access_uc;
        m->cache.access_uc = false;
        if (wb && merced_cache_store(m, model_pa(m, pa), size, v))
            return;                     /* the L2 took it */
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
    if (m->translation_cache)
        mj_destroy(((MercedTranslationCache *)m->translation_cache)->jit);
    free(m->translation_cache);
    free(m);
}

void merced_reset(Merced *m) {
    MercedBus bus = m->bus;
    uint64_t ninsts = 0;
    uint8_t cpu_revision = m->cpu_revision;
    uint8_t cpu_model = m->cpu_model;
    bool ia32_int10_shim = m->ia32_int10_shim;
    unsigned slot_cycles = m->slot_cycles;
    uint8_t clock_ratio_code = m->clock_ratio_code;
    bool clock_ratio_strapped = m->clock_ratio_strapped;
    bool merced_msrs = m->merced_msrs;
    void *translation_cache = m->translation_cache;
    memset(m, 0, sizeof(*m));
    m->bus = bus;
    m->ninsts = ninsts;
    m->cpu_revision = cpu_revision;
    m->cpu_model = cpu_model;
    m->ia32_int10_shim = ia32_int10_shim;
    m->clock_ratio_code = clock_ratio_code;
    m->clock_ratio_strapped = clock_ratio_strapped;
    m->merced_msrs = merced_msrs;
    m->slot_cycles = slot_cycles ? slot_cycles : 1;
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
     * different consumers disagree about what they want to see here.
     * Windows for Itanium (SETUPLDR.EFI) refuses to proceed on anything it
     * reads as "pre-B3 stepping" - real historical behavior, not an
     * emulator bug - so the machines expose revision 6 (Merced C0). */
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

void merced_set_cycles_per_slot(Merced *m, unsigned cycles) {
    m->slot_cycles = cycles ? cycles : 1;
}

void merced_set_clock_ratio_code(Merced *m, uint8_t code) {
    m->clock_ratio_code = code & 0xf;
    m->clock_ratio_strapped = true;
}

void merced_set_msr_model(Merced *m, bool enabled) {
    m->merced_msrs = enabled;
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
    /* br.ret restores a LESS privileged level saved in PFS.ppl (the
     * level br.call recorded), never a more privileged one.  This is what
     * drops a system call back to user mode after epc promoted it. */
    unsigned ppl = (unsigned)(pfs >> 62) & 3;
    if (ppl > ((m->psr >> PSR_CPL_SHIFT) & 3))
        m->psr = (m->psr & ~(UINT64_C(3) << PSR_CPL_SHIFT)) |
                 ((uint64_t)ppl << PSR_CPL_SHIFT);
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

/* The faults a mov to an application register can take (SDM vol. 3, mov ar):
 * a reserved, read-only (BSP) or mode-restricted (BSPSTORE/RNAT while the RSE
 * is enabled) register is an Illegal Operation, a NaT source a NaT
 * Consumption, a reserved RSC bit a Reserved Register/Field fault and a
 * kernel register, ITC or RUC from a less privileged level a Privileged
 * Register fault.  Returns true when the write must not go ahead; *st is the
 * instruction's outcome. */
static bool ar_write_blocked(Merced *m, unsigned ar, bool i_form,
                             uint64_t value, uint8_t nat, MercedStatus *st) {
    int cls = ar_class(ar, i_form);
    if (cls == AR_RESERVED || ar == AR_BSP ||
        ((ar == AR_BSPSTORE || ar == AR_RNAT) && (m->ar[AR_RSC] & 3u) != 0)) {
        *st = deliver_fault(m, VEC_GENERAL, 0, 0, false);
        return true;
    }
    if (nat) {
        *st = deliver_fault(m, VEC_NAT, UINT64_C(0x10), 0, false);
        return true;
    }
    if (cls == AR_IGNORED) {
        *st = MERCED_OK;
        return true;
    }
    if (((m->psr >> PSR_CPL_SHIFT) & 3) != 0 && (ar <= 7 || ar == 44 || ar == 45)) {
        *st = deliver_fault(m, VEC_GENERAL, 0x20, 0, false);
        return true;
    }
    /* ar.rsc defines only mode(2)/pl(2)/be(1) in bits 4:0 and loadrs in
     * bits 29:16; everything else is reserved. */
    if (ar == AR_RSC && (value & ~(0x1Full | (0x3FFFull << 16)))) {
        *st = deliver_fault(m, VEC_GENERAL, 0x30, 0, false);
        return true;
    }
    return false;
}

/* The same for a mov from an application register; an ignored register reads
 * as zero (*value). */
static bool ar_read_blocked(Merced *m, unsigned ar, bool i_form,
                            uint64_t *value, MercedStatus *st) {
    int cls = ar_class(ar, i_form);
    if (cls == AR_RESERVED ||
        ((ar == AR_BSPSTORE || ar == AR_RNAT) && (m->ar[AR_RSC] & 3u) != 0)) {
        *st = deliver_fault(m, VEC_GENERAL, 0, 0, false);
        return true;
    }
    if (((m->psr >> PSR_CPL_SHIFT) & 3) != 0 && (m->psr & PSR_SI) &&
        (ar == 44 || ar == 45)) {
        *st = deliver_fault(m, VEC_GENERAL, 0x20, 0, false);
        return true;
    }
    if (cls == AR_IGNORED)
        *value = 0;
    return false;
}

/* A failed chk branches to its recovery code only where the processor can
 * resteer in hardware; with translation off, interruption collection off or
 * single-step/taken-branch traps on, it takes a Speculative Operation fault
 * instead (vol. 2, 5.8.5).  code: 0 chk.a gr, 1 chk.s gr, 2 chk.a fr,
 * 3 chk.s fr; imm is the instruction's 21-bit immediate, which IIM holds. */
static MercedStatus chk_recover(Merced *m, unsigned code, uint64_t imm21) {
    static const uint64_t hw = PSR_IC | PSR_IT;
    if ((m->psr & hw) != hw || (m->psr & (PSR_SS | PSR_TB))) {
        m->cr[CR_IIM] = imm21;
        return deliver_fault(m, VEC_SPEC, code, 0, false);
    }
    m->ip = (m->ip & ~0xFull) + (uint64_t)(sext(imm21, 21) * 16);
    m->taken = 1;
    return MERCED_OK;
}

/* A reference that is not naturally aligned takes an Unaligned Data
 * Reference fault when PSR.ac is set; a semaphore's always does. */
#define DCACHE_LINE 64u

static bool unaligned_fault(Merced *m, uint64_t va, unsigned size,
                            uint64_t isr_access, bool always, MercedStatus *st) {
    if (!(va & (size - 1u)))
        return false;
    /* Without PSR.ac only a reference that straddles a data cache line
     * faults; a semaphore always does. */
    if (!(always || (m->psr & PSR_AC)) &&
        (va & (DCACHE_LINE - 1u)) + size <= DCACHE_LINE)
        return false;
    *st = deliver_fault(m, VEC_UNALIGNED, isr_access, va, true);
    return true;
}

/* A privileged instruction executed at a privilege level above 0 takes a
 * Privileged Operation fault. */
static bool privileged_fault(Merced *m, MercedStatus *st) {
    if (((m->psr >> PSR_CPL_SHIFT) & 3) == 0)
        return false;
    *st = deliver_fault(m, VEC_GENERAL, 0x10, 0, false);
    return true;
}

/* The M-unit system instructions (opcode 1, x3 = 0) that are privileged, by
 * x6: the indirect-register writes and the rr/dbr/ibr/pkr/pmc/msr reads, the
 * translation instructions, tpa/tak, and the psr and cr moves.  pmd and
 * cpuid reads, thash/ttag and the user-mask moves are not. */
static bool m_sys_x6_privileged(unsigned x6) {
    switch (x6) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05:
    case 0x06: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D:
    case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x14: case 0x16: case 0x1E: case 0x1F: case 0x24: case 0x25:
    case 0x2C: case 0x2D: case 0x2E: case 0x2F: case 0x34:
        return true;
    default:
        return false;
    }
}

/* ── Performance monitors ────────────────────────────────────────────────── */

/* Which PMC / PMD bits are implemented (Itanium Processor Reference Manual for
 * Software Development, 245320-003, ch. 6): PMC4-7 are the generic counter
 * controls, PMC8/9 the opcode matchers, PMC10-13 the event address registers
 * and the branch trace buffer.  Writing a register stores only these bits. */
static uint64_t pmc_value_for_write(unsigned idx, uint64_t v) {
    static const uint64_t mask[14] = {
        [0]  = UINT64_C(0x00000000000000F1),
        [1]  = 0, [2] = 0, [3] = 0,
        [4]  = UINT64_C(0x00000000037F7F7F),
        [5]  = UINT64_C(0x00000000037F7F7F),
        [6]  = UINT64_C(0x00000000033F7F7F),
        [7]  = UINT64_C(0x00000000033F7F7F),
        [8]  = UINT64_C(0xFFFFFFFE3FFFFFF8),
        [9]  = UINT64_C(0xFFFFFFFE3FFFFFF8),
        [10] = UINT64_C(0x00000000030F00CF),
        [11] = UINT64_C(0x00000000130F00CF),
        [12] = UINT64_C(0x000000000000FFCF),
        [13] = UINT64_C(0x0000000000000001),
    };
    return idx < 14u ? v & mask[idx] : v;
}

/* The address fields hold a 51-bit virtual address: bits 60:51 read as
 * copies of bit 50. */
static uint64_t pmd_va_sext(uint64_t v) {
    const uint64_t unimpl = UINT64_C(0x3FF) << 51;
    return (v & (UINT64_C(1) << 50)) ? (v | unimpl) : (v & ~unimpl);
}

static uint64_t pmd_value_for_write(unsigned idx, uint64_t v) {
    switch (idx) {
    case 0:  return pmd_va_sext(v & ~UINT64_C(0x1C));       /* IEAR address, tlb, v */
    case 1:  return v & UINT64_C(0xFFF);                    /* IEAR latency */
    case 2:  return pmd_va_sext(v);                         /* DEAR data address */
    case 3:  return v & UINT64_C(0xC000000000000FFF);       /* DEAR level, latency */
    case 4: case 5: case 6: case 7:                         /* 32-bit counters, sign-extended */
        return (uint64_t)(int64_t)(int32_t)(uint32_t)v;
    case 16: return v & UINT64_C(0xF);                      /* BTB full, bbi */
    case 17: return pmd_va_sext(v & ~UINT64_C(0x2));        /* DEAR instruction address */
    default: return v;                                      /* 8-15: BTB, full width */
    }
}

/* The generic counters PMC/PMD[4..7] count the events below; the model counts
 * only these two.  A counter runs while PMC0.fr is clear, its plm bit for the
 * current privilege level is set, PSR.pp (pm = 1) or PSR.up (pm = 0) is set
 * and the instruction-set mask allows Itanium execution. */
enum { PMU_EV_NOPS = 0x30, PMU_EV_STOPS = 0x2E };

/* Whether any armed counter has an event to count; kept out of the Merced
 * struct, which snapshots copy whole. */
static bool pmu_count_on;

static void pmu_update(Merced *m) {
    bool on = false;
    if (!(m->pmc[0] & 1u)) {
        for (unsigned n = 4; n <= 7; n++) {
            uint64_t pmc = m->pmc[n];
            if (!(pmc & 0xFu))
                continue;
            unsigned es = (unsigned)(pmc >> 8) & 0x7Fu;
            if (es == PMU_EV_NOPS || es == PMU_EV_STOPS)
                on = true;
        }
    }
    pmu_count_on = on;
}

static void pmu_count(Merced *m, unsigned es, uint64_t inc) {
    unsigned cpl = (unsigned)((m->psr >> PSR_CPL_SHIFT) & 3);
    for (unsigned n = 4; n <= 7; n++) {
        uint64_t pmc = m->pmc[n];
        if (((pmc >> 8) & 0x7Fu) != es || !((pmc >> cpl) & 1u))
            continue;
        bool pm = (pmc >> 6) & 1u;
        if (pm ? !((m->psr >> 21) & 1u) : !((m->psr >> 2) & 1u))
            continue;
        if ((pmc >> 24) & 1u)                       /* ism: Itanium disabled */
            continue;
        uint64_t old = m->pmd[n] & UINT64_C(0xFFFFFFFF);
        uint64_t sum = old + inc;
        m->pmd[n] = (uint64_t)(int64_t)(int32_t)(uint32_t)sum;
        if (sum >> 32) {                            /* carry out of bit 31 */
            m->pmc[0] |= UINT64_C(1) << n;
            if ((pmc >> 5) & 1u) {                  /* oi: freeze and interrupt */
                m->pmc[0] |= 1u;
                uint64_t pmv = m->cr[73];
                if (!((pmv >> 16) & 1u))
                    merced_raise_external(m, (uint8_t)(pmv & 0xFFu));
            }
        }
    }
    if (m->pmc[0] & 1u)
        pmu_update(m);
}

/* A slot retired while a counter is armed. */
static void pmu_retire(Merced *m, bool is_nop, int qp, unsigned stop_after) {
    if (m->psr & PSR_IS)
        return;
    /* nop.m/i/b and nop.f all count: the architecture's list names the
     * first three, but the firmware's own check only holds counting nop.f. */
    if (is_nop && qp)
        pmu_count(m, PMU_EV_NOPS, 1);
    if (stop_after)
        pmu_count(m, PMU_EV_STOPS, 1);
}

/* Data and instruction breakpoints (SDM vol. 2, 7.1).  DBR/IBR come in pairs:
 * an address and a control word with the read/write or execute enable in bits
 * 63:62, the privilege-level mask in 59:56 and an address mask in 55:0 (bits
 * 63:56 always compare). */
static bool data_debug_fault(Merced *m, uint64_t va, unsigned size,
                             uint64_t isr_access, MercedStatus *st) {
    if (!(m->psr & PSR_DB) || (m->psr & PSR_DD))
        return false;
    bool is_write = (isr_access & ISR_W) != 0, is_read = (isr_access & ISR_R) != 0;
    unsigned cpl = (unsigned)((m->psr >> PSR_CPL_SHIFT) & 3);
    for (unsigned i = 0; i + 1 < 16; i += 2) {
        uint64_t addr = m->dbr[i], ctl = m->dbr[i + 1];
        bool w = (ctl >> 62) & 1u, r = (ctl >> 63) & 1u;
        if (!w && !r)
            continue;                               /* pair disabled */
        if (!((w && is_write) || (r && is_read)))
            continue;
        if (!((ctl >> (56 + cpl)) & 1u))
            continue;
        uint64_t cmp = (ctl & UINT64_C(0x00FFFFFFFFFFFFFF)) |
                       UINT64_C(0xFF00000000000000);
        for (unsigned b = 0; b < size; b++) {       /* any byte matching hits */
            if ((((va + b) ^ addr) & cmp) == 0) {
                *st = deliver_fault(m, VEC_DEBUG, isr_access, va, true);
                return true;
            }
        }
    }
    return false;
}

/* The same for an instruction fetch: the match is on the bundle. */
static bool inst_debug_fault(Merced *m, uint64_t ip, MercedStatus *st) {
    if (!(m->psr & PSR_DB) || (m->psr & PSR_ID))
        return false;
    unsigned cpl = (unsigned)((m->psr >> PSR_CPL_SHIFT) & 3);
    for (unsigned i = 0; i + 1 < 16; i += 2) {
        uint64_t ctl = m->ibr[i + 1];
        if (!((ctl >> 63) & 1u) || !((ctl >> (56 + cpl)) & 1u))
            continue;
        uint64_t cmp = ((ctl & UINT64_C(0x00FFFFFFFFFFFFFF)) |
                        UINT64_C(0xFF00000000000000)) & ~UINT64_C(0xF);
        if (((ip ^ m->ibr[i]) & cmp) != 0)
            continue;
        *st = deliver_fault(m, VEC_DEBUG, ISR_X, ip, true);
        return true;
    }
    return false;
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
            if (x4 == 4 || x4 == 6) {              /* A10 pshladd2 / pshradd2 */
                unsigned count = x2b + 1;
                for (unsigned sh = 0; sh < 64; sh += 16) {
                    int64_t la = (int16_t)(a >> sh), lb = (int16_t)(b >> sh);
                    /* A left shift that overflows saturates and the add is
                     * skipped; an in-range shift is added with saturation. */
                    int64_t v = x4 == 6 ? (la >> count) : (la * (1ll << count));
                    if (!(v > 0x7FFF || v < -0x8000))
                        v += lb;
                    v = v > 0x7FFF ? 0x7FFF : v < -0x8000 ? -0x8000 : v;
                    res |= ((uint64_t)v & 0xFFFF) << sh;
                }
                gr_write(m, r1, res, nat);
                return 1;
            }
            for (unsigned sh = 0; sh < 64; sh += width) {
                int64_t la = (int64_t)sext((a >> sh) & mask, width);
                int64_t lb = (int64_t)sext((b >> sh) & mask, width);
                uint64_t ua = (a >> sh) & mask, ub = (b >> sh) & mask;
                uint64_t r;
                switch (x4) {
                case 0: case 1: {              /* padd / psub */
                    bool sub = x4 == 1;
                    int64_t u = sub ? (int64_t)ua - (int64_t)ub : (int64_t)ua + (int64_t)ub;
                    int64_t sg = sub ? la - lb : la + lb;
                    /* uus: the first operand is unsigned, the second signed */
                    int64_t us = sub ? (int64_t)ua - lb : (int64_t)ua + lb;
                    int64_t shi = (int64_t)(smin - 1), slo = -(int64_t)smin;
                    int64_t umax = (int64_t)mask;
                    switch (x2b) {
                    case 0:  r = (uint64_t)u & mask; break;
                    case 1:  r = (uint64_t)(sg > shi ? shi : sg < slo ? slo : sg) & mask; break;
                    case 2:  r = (uint64_t)(u > umax ? umax : u < 0 ? 0 : u); break;
                    default: r = (uint64_t)(us > umax ? umax : us < 0 ? 0 : us); break;
                    }
                    break;
                }
                case 2: case 3: {              /* pavg (.raz) / pavgsub */
                    uint32_t n = x4 == 3 ? (uint32_t)(ua - ub) : (uint32_t)(ua + ub);
                    /* pavg rounds to odd unless .raz rounds away from zero */
                    r = (x4 == 2 && x2b == 3 ? (n + 1) >> 1 : (n >> 1) | (n & 1)) & mask;
                    break;
                }
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
            if (nat && qp) {
                /* A NaT operand makes AND comparisons clear both targets;
                 * OR and OR.ANDCM comparisons leave them unchanged. */
                if (ctype == 2) {
                    pr_write(m, p1, 0);
                    pr_write(m, p2, 0);
                }
                return 1;
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
            if (nat && qp) {
                if (ctype == 2) {
                    pr_write(m, p1, 0);
                    pr_write(m, p2, 0);
                }
                return 1;
            }
            set_preds(m, p1, p2, qp, res, ctype);
            return 1;
        }

        switch (major) {
        case 0xC: res = a < sb; break;                   /* lt (signed) */
        case 0xD: res = au < ub; break;                  /* ltu */
        default:  res = au == ub; break;                 /* eq */
        }
        if (nat && qp) {
            pr_write(m, p1, 0);
            pr_write(m, p2, 0);
            return 1;
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
        if (data_debug_fault(m, va, size, ISR_R | ISR_W, &st)) return st;
        if (unaligned_fault(m, va, size, ISR_R | ISR_W, true, &st)) return st;
        /* A semaphore needs cacheable memory: a physical reference with
         * address bit 63 set is uncacheable (vol. 2, 4.4.7). */
        if (!(m->psr & PSR_DT) && (va >> 63))
            return deliver_fault(m, VEC_UNSUPPORTED, ISR_R | ISR_W, va, true);
        if (m->psr & PSR_DT) {
            uint32_t rid = (uint32_t)((m->rr[va >> 61] >> 8) & 0xFFFFFFull);
            const MercedTlbEntry *e = tlb_search(m->dtr, MERCED_N_DTR, rid, va, &m->dtr_hint);
            if (!e) e = tlb_search(m->dtc, MERCED_N_TC, rid, va, &m->dtc_hint);
            if (e && (e->pte & PTE_PRESENT) && ((e->pte >> PTE_MA_SHIFT) & 7) != 0)
                return deliver_fault(m, VEC_UNSUPPORTED, ISR_R | ISR_W, va, true);
        }
        if (!va_translate(m, va, false, false, ISR_R | ISR_W, &pa, &st)) return st;
        if (x6 <= 0x07) {                       /* cmpxchg.acq/.rel */
            uint64_t old = phys_read(m, pa, size);
            uint64_t ccv = m->ar[AR_CCV];
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
        case 0x1E: v = merced_fp_to_single_mem(f); break;         /* getf.s */
        case 0x1F: v = merced_fp_to_double_mem(f); break;         /* getf.d */
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
            if (x6 == 0x1B) size = 8;              /* ld8.fill */
            /* An ordinary load consumes a NaT address as a non-access
             * register NaT fault.  Speculative loads defer it by producing
             * a NaT destination, just like any other deferrable fault. */
            if (n3 && !spec)
                return deliver_fault(m, VEC_NAT,
                                     ISR_NA | ISR_R | UINT64_C(0x10),
                                     0, false);
            if (!n3 && data_debug_fault(m, va, size, ISR_R, &st))
                return st;
            if (!n3 && unaligned_fault(m, va, size, ISR_R, false, &st))
                return st;
            if (n3) {
                gr_write(m, r1, 0, 1);
            } else if (!va_translate(m, va, false, spec, ISR_R, &pa, &st)) {
                /* A delivered fault also returns MERCED_OK, with taken
                 * marking the transfer to its handler.  Only a deferred
                 * speculative fault completes the load: a real fault must
                 * preserve both destination and postincrement base for
                 * the handler's retry. */
                if (st != MERCED_OK || m->taken) return st;
                gr_write(m, r1, 0, 1);             /* NaT on deferred spec load */
            } else {
                bool check = x6 >= 0x20 && x6 <= 0x2B;
                bool clear = x6 >= 0x20 && x6 <= 0x23;
                bool alat_hit = check && alat_check(m, r1, pa, size, clear);
                if (!check || !alat_hit) {
                    uint64_t v = phys_read(m, pa, size);
                    uint8_t nat = 0;
                    if (x6 == 0x1B)                /* ld8.fill: NaT from UNAT */
                        nat = (uint8_t)((m->ar[AR_UNAT] >> ((va >> 3) & 0x3F)) & 1);
                    gr_write(m, r1, v, nat);
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
            if (data_debug_fault(m, va, size, ISR_W, &st)) return st;
            if (unaligned_fault(m, va, size, ISR_W, false, &st)) return st;
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
        if (major == 6 && xbit && !(x6 >= 0x1C && x6 <= 0x1F)) {
            /* M11 ldfps / ldfpd / ldfp8 and their check forms: a pair of
             * floating-point loads into two registers of opposite parity. */
            if (!qp) return MERCED_OK;
            unsigned fmt = x6 & 3, fa = r1, fb = r2;
            bool chk = x6 >= 0x20 && x6 <= 0x27, spec = ((x6 >> 2) & 3) == 1 || ((x6 >> 2) & 3) == 3;
            if (fmt == 0 || (x6 > 0x0F && !chk))
                return mhalt(m, "unimpl M ldfp x6=0x%02X", x6);
            if ((fr_phys(m, fa) & 1u) == (fr_phys(m, fb) & 1u))
                return deliver_fault(m, VEC_GENERAL, 0, 0, false);
            uint64_t va = gr_read(m, r3, &n3), pa;
            unsigned total = fmt == 2 ? 8 : 16;
            if (n3 && !spec)
                return deliver_fault(m, VEC_NAT, ISR_NA | ISR_R | UINT64_C(0x10), 0, false);
            if (data_debug_fault(m, va, total, ISR_R, &st)) return st;
            if (unaligned_fault(m, va, total, ISR_R, false, &st)) return st;
            unsigned ka = ALAT_FP_KEY + fr_phys(m, fa), kb = ALAT_FP_KEY + fr_phys(m, fb);
            unsigned esz = total / 2;
            bool skip = false;
            if (chk) {
                if (!va_translate(m, va, false, false, ISR_R, &pa, &st)) return st;
                bool ha = alat_check(m, ka, pa, esz, x6 < 0x24);
                bool hb = alat_check(m, kb, pa + esz, esz, x6 < 0x24);
                skip = ha && hb;
            } else if (!va_translate(m, va, false, spec, ISR_R, &pa, &st)) {
                if (st != MERCED_OK || m->taken) return st;
                MercedFpReg nat = {0, 0, 0, 1};
                fr_write(m, fa, nat);
                fr_write(m, fb, nat);
                skip = true;
            } else {
                alat_invalidate_reg(m, ka);
                alat_invalidate_reg(m, kb);
            }
            if (!skip) {
                MercedFpReg f[2];
                for (unsigned k = 0; k < 2; k++) {
                    uint64_t u = phys_read(m, pa + k * esz, esz);
                    if (fmt == 1) {
                        MercedFpReg g = {u, 0x1003E, 0, 0};
                        f[k] = g;
                    } else if (fmt == 2) {
                        f[k] = merced_fp_from_single_mem((uint32_t)u);
                    } else {
                        f[k] = merced_fp_from_double_mem(u);
                    }
                }
                fr_write(m, fa, f[0]);
                fr_write(m, fb, f[1]);
                if (((x6 >> 2) & 3) == 2 || ((x6 >> 2) & 3) == 3) {  /* .a / .sa */
                    alat_set(m, ka, pa, esz);
                    alat_set(m, kb, pa + esz, esz);
                }
            }
            if (mbit)
                gr_write(m, r3, va + total, n3);
            return MERCED_OK;
        }
        if (major == 6 && xbit && mbit == 0) {     /* M18 setf */
            if (!qp) return MERCED_OK;
            uint64_t v = gr_read(m, r2, &n2);
            MercedFpReg f = {0, 0, 0, 0};
            switch (x6) {
            case 0x1C: f.sig = v; f.exp = 0x1003E; break;         /* setf.sig */
            case 0x1D: f.exp = (uint32_t)(v & 0x1FFFF);           /* setf.exp */
                       f.sign = (uint8_t)((v >> 17) & 1);
                       f.sig = 0x8000000000000000ull; break;
            case 0x1E: f = merced_fp_from_single_mem((uint32_t)v); break;   /* setf.s */
            case 0x1F: f = merced_fp_from_double_mem(v); break;             /* setf.d */
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
        /* FP loads: x6 0x00-0x0F ldfe/ldf8/ldfs/ldfd(+spec), 0x1B ldf.fill,
         * 0x20-0x27 ldf*.c.clr/ldf*.c.nc;
         * stores: 0x30-0x33 stfe/stf8/stfs/stfd, 0x3B stf.spill */
        int is_st = (x6 >= 0x30);
        bool check_load = x6 >= 0x20 && x6 <= 0x27;
        if (!qp) return MERCED_OK;
        uint64_t va = gr_read(m, r3, &n3), pa;
        if (check_load && n3)
            return deliver_fault(m, VEC_NAT,
                                 ISR_NA | ISR_R | UINT64_C(0x10), 0, false);
        {
            static const unsigned fsize[4] = { 16, 8, 4, 8 };
            unsigned fsz = (x6 == 0x1B || x6 == 0x3B) ? 16 : fsize[x6 & 3];
            if (data_debug_fault(m, va, fsz, is_st ? ISR_W : ISR_R, &st))
                return st;
            if (unaligned_fault(m, va, fsz, is_st ? ISR_W : ISR_R, false, &st))
                return st;
        }
        bool fspec = !is_st && x6 <= 0x0F && (((x6 >> 2) & 3) == 1 || ((x6 >> 2) & 3) == 3);
        if (!va_translate(m, va, false, fspec, is_st ? ISR_W : ISR_R, &pa, &st)) {
            /* A deferred speculative load completes with a NaTVal */
            if (!fspec || st != MERCED_OK || m->taken) return st;
            MercedFpReg nat = {0, 0, 0, 1};
            fr_write(m, (unsigned)bits(raw, 6, 7), nat);
            goto fp_base_update;
        }
        unsigned fmt = x6 & 3;   /* 0=e 1=8 2=s 3=d */
        if (!is_st && (x6 <= 0x0F || x6 == 0x1B || check_load)) {
            static const unsigned fsize[4] = { 16, 8, 4, 8 };
            unsigned fsz = x6 == 0x1B ? 16 : fsize[fmt];
            unsigned alat_key = ALAT_FP_KEY + fr_phys(m, (unsigned)bits(raw, 6, 7));
            /* A check load that finds its entry leaves the register alone;
             * ldf.a and ldf.sa establish the entry a later check consumes,
             * and any other load removes the register's old one. */
            if (check_load && alat_check(m, alat_key, pa, fsz, x6 < 0x24))
                goto fp_load_done;
            if (!check_load)
                alat_invalidate_reg(m, alat_key);
            MercedFpReg f = {0, 0, 0, 0};
            if (x6 == 0x1B) {                       /* ldf.fill: spill format */
                f.sig = phys_read(m, pa, 8);
                uint64_t se = phys_read(m, pa + 8, 8);
                f.exp = (uint32_t)(se & 0x1FFFF);
                f.sign = (uint8_t)((se >> 17) & 1);
                /* the NaTVal encoding (sign 0, exponent 0x1FFFE, zero
                 * significand) comes back as a NaTVal */
                if (!f.sign && f.exp == 0x1FFFE && f.sig == 0)
                    f.nat = 1;
            } else switch (fmt) {
            case 0: {                                /* ldfe: 80-bit extended */
                f.sig = phys_read(m, pa, 8);
                uint32_t hw = (uint32_t)phys_read(m, pa + 8, 2);
                uint32_t me = hw & 0x7FFFu;
                f.exp = me == 0x7FFFu ? 0x1FFFFu : me == 0 ? 0 : me + 0xC000u;
                f.sign = (uint8_t)((hw >> 15) & 1);
                break;
            }
            case 1: f.sig = phys_read(m, pa, 8); f.exp = 0x1003E; break; /* ldf8 */
            case 2: f = merced_fp_from_single_mem((uint32_t)phys_read(m, pa, 4)); break;
            case 3: f = merced_fp_from_double_mem(phys_read(m, pa, 8)); break;
            }
            fr_write(m, (unsigned)bits(raw, 6, 7), f);
            if ((x6 >= 0x08 && x6 <= 0x0F) || (x6 >= 0x28 && x6 <= 0x2B))
                alat_set(m, alat_key, pa, fsz);
        fp_load_done:;
        } else if (is_st) {
            MercedFpReg f = fr_read(m, (unsigned)bits(raw, 13, 7));
            if (x6 == 0x3B) {                        /* stf.spill */
                if (f.nat) {                         /* NaTVal's spill image */
                    phys_write(m, pa, 0, 8);
                    phys_write(m, pa + 8, 0x1FFFE, 8);
                } else {
                    phys_write(m, pa, f.sig, 8);
                    phys_write(m, pa + 8, ((uint64_t)f.sign << 17) | f.exp, 8);
                }
            } else switch (fmt) {
            case 0: {                                /* stfe: 80-bit extended */
                uint32_t me = f.exp == 0x1FFFFu ? 0x7FFFu : f.exp == 0 ? 0
                            : (f.exp - 0xC000u) & 0x7FFFu;
                phys_write(m, pa, f.sig, 8);
                phys_write(m, pa + 8, ((uint32_t)f.sign << 15) | me, 2);
                break;
            }
            case 1: phys_write(m, pa, f.sig, 8); break;
            case 2: phys_write(m, pa, merced_fp_to_single_mem(f), 4); break;
            case 3: phys_write(m, pa, merced_fp_to_double_mem(f), 8); break;
            }
        } else {
            return mhalt(m, "unimpl M fp-mem x6=0x%02X major %u", x6, major);
        }
    fp_base_update:
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
    xlate_flush(m);
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
    xlate_flush(m);
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

/* AMI relocates its EFI runtime image below the detected top of RAM.
 * Firmware handoff handling must follow that relocation on smaller
 * machines.  Retain the original 2 GiB window for older snapshots. */
static bool ami_runtime_ip(const Merced *m, uint64_t ip) {
    const uint64_t size = UINT64_C(0x01000000);
    return (ip >= UINT64_C(0x7F000000) && ip < UINT64_C(0x80000000)) ||
           (m->bus.ram_size >= size &&
            ip >= m->bus.ram_size - size && ip < m->bus.ram_size);
}

/* EFI allocates applications below its runtime image. While IVA is unset,
 * keep interrupts pending across those entries as well as the firmware's
 * own transition. Once the loader installs an IVT, normal delivery resumes. */
static bool ami_ivt_handoff_pending(const Merced *m) {
    return m->cr[CR_IVA] == 0 &&
           (ami_runtime_ip(m, m->ip) ||
            (m->ip >= UINT64_C(0x04000000) && m->ip < m->bus.ram_size) ||
            (m->ip & UINT64_C(0x00000fff00000000)) ==
                UINT64_C(0x00000e0000000000) ||
            (m->psr & PSR_IS));
}

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
    bool ami_firmware_ip = ami_runtime_ip(m, m->ip);

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
            if (vhpt_abort_debug++ < 64)
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
 * Reserved entry encodings must fall back to a software TLB-miss fault
 * without being cached; an OS may use them for software-managed mappings. */
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
    /* SDM Vol.2, VHPT Walker: unlike an explicit itc/itr, a reserved entry
     * causes an ordinary TLB miss.  Leave these entries to the software
     * miss handler instead of caching an unusable translation. */
    if (!translation_insert_fields_valid(pte, itir))
        return VHPT_MISS;
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
        xlate_flush(m);
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
    for (int64_t p = m->rse_flushed_regs; p < end; p++) {
        uint32_t idx = rse_stack_slot(m, p);
        uint64_t addr = rse_addr(m, p), pa, rnat_pa = 0;
        MercedStatus st;
        if (!va_translate(m, addr, false, false,
                          ISR_W | ISR_RS, &pa, &st))
            return st;
        /* On Merced PSR.da and PSR.dd cover one RSE reference, not a whole
         * flushrs: each spill takes its own access-bit fault. */
        m->psr &= ~(PSR_DA | PSR_DD);
        uint64_t bit = UINT64_C(1) << ((addr >> 3) & 63);
        uint64_t rnat = (m->ar[AR_RNAT] & ~bit) |
                        (m->nat_stack[idx] ? bit : 0);
        bool end_group = rse_is_rnat_slot(addr + 8);
        if (end_group &&
            !va_translate(m, addr + 8, false, false,
                          ISR_W | ISR_RS, &rnat_pa, &st))
            return st;
        uint64_t value = m->gr_stack[idx];
        if (m->ar[AR_RSC] & (UINT64_C(1) << 4))
            value = __builtin_bswap64(value);
        phys_write(m, pa, value, 8);
        if (end_group) {
            uint64_t word = rnat;
            if (m->ar[AR_RSC] & (UINT64_C(1) << 4))
                word = __builtin_bswap64(word);
            phys_write(m, rnat_pa, word, 8);
        }
        m->ar[AR_RNAT] = end_group ? 0 : rnat;
        /* Keep the completed prefix restartable if a later spill faults. */
        m->rse_flushed_regs = p + 1;
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

/* flushrs spills the dirty parent frames up to BSP (the current frame's
 * base).  Current-frame registers are not part of the dirty partition. */
static MercedStatus rse_flush(Merced *m) {
    int64_t target = (int64_t)m->bof_total;
    return rse_store_through(m, target);
}

/* Fill only the clean portion of a restored frame.  Its backing store is
 * authoritative after mov-to-BSPSTORE invalidates the old clean partition.
 * Stage values and NaTs until every reference succeeds, so a fault cannot
 * expose a partly restored frame or discard still-resident dirty values. */
static bool rse_fill_range(Merced *m, int64_t start, int64_t end,
                           bool incomplete, MercedStatus *st) {
    *st = MERCED_OK;
    if (start >= end)
        return true;
    uint64_t values[MERCED_RSE_CAPACITY];
    uint8_t nats[MERCED_RSE_CAPACITY];
    if ((uint64_t)(end - start) > MERCED_RSE_CAPACITY) {
        *st = mhalt(m, "RSE fill exceeds register capacity");
        return false;
    }
    uint64_t store = rse_addr(m, m->rse_flushed_regs);
    uint64_t first_rnat = 0;
    uint64_t access = ISR_R | ISR_RS | (incomplete ? ISR_IR : 0);
    for (int64_t p = start; p < end; p++) {
        uint64_t addr = rse_addr(m, p), pa;
        if (!va_translate(m, addr, false, false, access, &pa, st))
            return false;
        uint64_t value = phys_read(m, pa, 8);
        if (m->ar[AR_RSC] & (UINT64_C(1) << 4))
            value = __builtin_bswap64(value);
        uint64_t rnat_addr = addr | UINT64_C(0x1f8);
        uint64_t rnat = m->ar[AR_RNAT];
        if (rnat_addr < store) {
            if (!va_translate(m, rnat_addr, false, false, access, &pa, st))
                return false;
            rnat = phys_read(m, pa, 8);
            if (m->ar[AR_RSC] & (UINT64_C(1) << 4))
                rnat = __builtin_bswap64(rnat);
        }
        if (p == start)
            first_rnat = rnat;
        values[p - start] = value;
        nats[p - start] = (uint8_t)((rnat >> ((addr >> 3) & 63)) & 1);
    }
    for (int64_t p = start; p < end; p++) {
        uint32_t slot = rse_stack_slot(m, p);
        m->gr_stack[slot] = values[p - start];
        m->nat_stack[slot] = nats[p - start];
    }
    m->rse_flushed_regs = start;
    m->ar[AR_RNAT] = first_rnat;
    if (incomplete)
        m->psr &= ~(PSR_DA | PSR_DD);
    return true;
}

static bool rse_fill_current_frame(Merced *m, MercedStatus *st) {
    int64_t base = (int64_t)m->bof_total;
    int64_t end = base + CFM_SOF(m->cfm);
    if (end > m->rse_flushed_regs)
        end = m->rse_flushed_regs;
    return rse_fill_range(m, base, end, true, st);
}

/* loadrs positions BSPSTORE ar.rsc.loadrs bytes behind BSP, excluding RNAT
 * words from the register count. Fill the clean portion from its current
 * backing store and retain the dirty registers already resident in the ring. */
static MercedStatus rse_load(Merced *m, uint64_t loadrs_bytes) {
    int64_t bsp_regs = (int64_t)m->bof_total;
    uint64_t bsp_addr = rse_addr(m, bsp_regs);
    int64_t target = bsp_regs - rse_regs_in_window(bsp_addr, loadrs_bytes);
    /* Only the portion below the current BSPSTORE/load boundary needs a
     * memory fill.  Registers from that boundary through BSP are already
     * resident; the architecture merely reclassifies them as dirty.  Reading
     * the whole requested window destroys live interrupted frames because
     * those resident values need not have been eagerly spilled yet. */
    int64_t load_end = m->rse_flushed_regs < bsp_regs
                     ? m->rse_flushed_regs : bsp_regs;
    MercedStatus st;
    if (!rse_fill_range(m, target, load_end, false, &st))
        return st;
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
                MercedStatus pst;
                if (x4 >= 6 && privileged_fault(m, &pst)) return pst;
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
                if (qp) {
                    memset(m->alat, 0, sizeof(m->alat));
                    m->alat_valid_mask = 0;
                }
                return MERCED_OK;
            case 0x12:                              /* invala.e r */
                if (qp)
                    alat_invalidate_reg(m, r1);
                return MERCED_OK;
            case 0x13:                              /* invala.e f */
                if (qp)
                    alat_invalidate_reg(m, ALAT_FP_KEY + fr_phys(m, r1));
                return MERCED_OK;
            case 0x20: return MERCED_OK;            /* fwb */
            case 0x22: case 0x23: return MERCED_OK; /* mf / mf.a */
            case 0x28: {                            /* M30 mov.m ar3=imm8 */
                if (!qp) return MERCED_OK;
                unsigned ar3 = (unsigned)bits(raw, 20, 7);
                uint64_t imm = (uint64_t)sext((bits(raw, 36, 1) << 7) | bits(raw, 13, 7), 8);
                MercedStatus ast;
                if (ar_write_blocked(m, ar3, false, imm, 0, &ast)) return ast;
                m->ar[ar3] = ar_value_for_write(m, ar3, imm);
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
            bool hit = alat_check_reg(m, x3 < 6 ? reg : ALAT_FP_KEY + fr_phys(m, reg), x3 & 1);
            if (hit)
                return MERCED_OK;
            /* M22/M23 use contiguous imm20b in bits 32:13.  Bits 12:6
             * name the checked register, unlike chk.s's split immediate. */
            return chk_recover(m, x3 < 6 ? 0 : 2,
                               (bits(raw, 36, 1) << 20) | bits(raw, 13, 20));
        }
        if (x3 == 1 || x3 == 2 || x3 == 3) {
            /* M20 chk.s.m r2,target25 (x3=1); 2/3 are fp/spec variants */
            if (!qp) return MERCED_OK;
            gr_read(m, r2, &n2);
            if (x3 == 1 && n2)
                return chk_recover(m, 1, (bits(raw, 36, 1) << 20) |
                                   (bits(raw, 20, 13) << 7) | bits(raw, 6, 7));
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
        /* The frame must be consistent and the target a register of the new
         * frame (and not r0). */
        if (sof > MERCED_N_STACKED || sol > sof || sor * 8 > sof || r1 == 0 ||
            (r1 >= 32 && r1 - 32 >= sof))
            return deliver_fault(m, VEC_GENERAL, 0, 0, false);
        uint64_t pfs = m->ar[AR_PFS];
        /* alloc replaces sof/sol/sor but preserves the three rotation
         * bases.  Firmware routinely enters leaf arithmetic helpers with
         * a non-zero rrb.pr and relies on alloc leaving it intact. */
        m->cfm = (m->cfm & ~0x3FFFFull) |
                 sof | ((uint64_t)sol << 7) | ((uint64_t)sor << 14);
        gr_write(m, r1, pfs, 0);
        return rse_spill_excess(m);
    }
    if (x3 == 1 || x3 == 3) {
        /* M20 chk.s.m r2,target25 / M21 chk.s f2,target25. The checked
         * source occupies bits 19:13, between the two immediate fields. */
        if (!qp) return MERCED_OK;
        bool failed;
        if (x3 == 3) {
            unsigned disabled = r2 >= 32 ? 2 : r2 >= 2 ? 1 : 0;
            if ((disabled == 1 && (m->psr & PSR_DFL)) ||
                (disabled == 2 && (m->psr & PSR_DFH)))
                return deliver_fault(m, VEC_DISABLED_FP, disabled, 0, false);
            MercedFpReg f = fr_read(m, r2);
            /* ldf.fill also supplies the architectural register encoding.
             * Its sign-one counterpart is a pseudo-zero, not NaTVal. */
            failed = f.nat || (!f.sign && f.exp == 0x1fffe && f.sig == 0);
        } else {
            gr_read(m, r2, &n2);
            failed = n2;
        }
        if (failed)
            return chk_recover(m, x3 == 3 ? 3 : 1, (bits(raw, 36, 1) << 20) |
                               (bits(raw, 20, 13) << 7) | bits(raw, 6, 7));
        return MERCED_OK;
    }
    if (x3 != 0)
        return mhalt(m, "unimpl M-sys major 1 x3=%u", x3);

    unsigned x6 = (unsigned)bits(raw, 27, 6);
    if (!qp) return MERCED_OK;
    if (m_sys_x6_privileged(x6)) {
        MercedStatus pst;
        if (privileged_fault(m, &pst)) return pst;
    }
    switch (x6) {
    case 0x00: case 0x03: {                         /* rr[r3] = r2, pkr[r3] = r2 */
        uint64_t idx = gr_read(m, r3, &n3), v = gr_read(m, r2, &n2);
        if (n2 || n3)
            return deliver_fault(m, VEC_NAT, UINT64_C(0x10), 0, false);
        /* A reserved bit, or for an rr a page size the processor does not
         * implement, is a Reserved Register/Field fault. */
        bool bad = x6 == 0
            ? ((v & UINT64_C(0xFFFFFFFF00000002)) ||
               !((PAL_INSERTABLE_PS_MASK >> ((v >> 2) & 0x3F)) & 1))
            : (v & UINT64_C(0xFFFFFFFF000000F0)) != 0;
        if (bad)
            return deliver_fault(m, VEC_GENERAL, 0x30, 0, false);
        xlate_flush(m);
        if (x6 == 0) {
            m->rr[idx >> 61 & 7] = v;      /* the index is GR[r3] bits 63:61 */
            return MERCED_OK;
        }
        unsigned i = (unsigned)(idx & 15);
        /* Merced invalidates the older copy of a valid key another register
         * already holds, and keeps the one being written. */
        if (v & 1u) {
            for (unsigned k = 0; k < 16; k++)
                if (k != i && (m->pkr[k] & 1u) &&
                    ((m->pkr[k] ^ v) & UINT64_C(0xFFFFFF00)) == 0)
                    m->pkr[k] &= ~UINT64_C(1);
        }
        m->pkr[i] = v;
        return MERCED_OK;
    }
    case 0x01: m->dbr[gr_read(m, r3, &n3) & 15] = gr_read(m, r2, &n2); return MERCED_OK;
    case 0x02: m->ibr[gr_read(m, r3, &n3) & 15] = gr_read(m, r2, &n2); return MERCED_OK;
    case 0x04: {
        unsigned idx = (unsigned)(gr_read(m, r3, &n3) & 31);
        m->pmc[idx] = pmc_value_for_write(idx, gr_read(m, r2, &n2));
        if (idx == 0 || (idx >= 4 && idx <= 7))
            pmu_update(m);
        return MERCED_OK;
    }
    case 0x05: {
        unsigned idx = (unsigned)(gr_read(m, r3, &n3) & 31);
        m->pmd[idx] = pmd_value_for_write(idx, gr_read(m, r2, &n2));
        return MERCED_OK;
    }
    case 0x06: {
        unsigned idx = (unsigned)(gr_read(m, r3, &n3) & 4095);
        uint64_t v = gr_read(m, r2, &n2);
        if (idx == MSR_PMI_LATCH) {
            m->msr[idx] &= ~v;                      /* write one to clear */
        } else if (m->merced_msrs) {
            merced_msr_access(m, true, idx, v);
        } else {
            m->msr[idx] = v;
        }
        return MERCED_OK;
    }
    case 0x09: case 0x0A: case 0x0B: {              /* ptc.l/g/ga */
        uint64_t va = gr_read(m, r3, &n3);
        uint64_t ps = (gr_read(m, r2, &n2) >> 2) & 0x3F;
        if (n3 || n2)
            return deliver_fault(m, VEC_NAT, UINT64_C(0x10), 0, false);
        uint32_t rid = (uint32_t)((m->rr[va >> 61] >> 8) & 0xFFFFFFull);
        uint64_t len = ps >= 64 ? ~0ull : 1ull << ps;
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
        if (idx == MSR_PMI_LATCH) {
            v = m->msr[idx] & PMI_VECTORS;
        } else if (m->merced_msrs) {
            v = merced_msr_access(m, false, idx, 0);
        } else if (idx == 0x617 && m->clock_ratio_strapped) {
            /* The ratio is hardware strapping; firmware writes do not
             * define it. */
            v = (m->msr[idx] & ~(UINT64_C(0xf) << 22)) |
                ((uint64_t)m->clock_ratio_code << 22);
        } else {
            v = m->msr[idx];
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
        MercedStatus ast;
        if (ar_read_blocked(m, ar3, false, &v, &ast)) return ast;
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
            } else if (m->timer_pending &&
                       interrupt_unmasked(m, (uint8_t)m->cr[CR_ITV])) {
                v = m->cr[CR_ITV] & 0xFFull;
                m->timer_pending = 0;
            } else {
                v = 15;
            }
            if (v >= 16)
                m->ext_in_service[v >> 3] |= (uint8_t)(1u << (v & 7));
        }
        gr_write(m, r1, v, 0);
        return MERCED_OK;
    }
    case 0x25: gr_write(m, r1, m->psr, 0); return MERCED_OK;   /* mov r1=psr */
    case 0x29: m->psr = (m->psr & ~0x3Full) | (gr_read(m, r2, &n2) & 0x3F);
               return MERCED_OK;                    /* mov psr.um=r2 */
    case 0x2A: {                                    /* mov.m ar3=r2 */
        unsigned ar3 = (unsigned)bits(raw, 20, 7);
        uint64_t ar_val = gr_read(m, r2, &n2);
        MercedStatus ast;
        if (ar_write_blocked(m, ar3, false, ar_val, n2, &ast)) return ast;
        m->ar[ar3] = ar_value_for_write(m, ar3, ar_val);
        if (ar3 == AR_BSPSTORE) {
            int64_t bsp_regs = (int64_t)m->bof_total;
            int64_t dirty = bsp_regs > m->rse_flushed_regs
                          ? bsp_regs - m->rse_flushed_regs : 0;
            uint64_t new_store = m->ar[AR_BSPSTORE];
            /* BSPSTORE invalidates the clean partition but preserves the
             * current and dirty registers, even for a nearby backward
             * rewind.  Automatic return fills restore the older frame;
             * moving BOF here would incorrectly rename the live registers. */
            m->rse_flushed_regs = bsp_regs - dirty;
            m->rse_anchor_addr = new_store;
            m->rse_anchor_regs = m->rse_flushed_regs;
            m->ar[AR_BSP] = rse_addr(m, (int64_t)m->bof_total);
        }
        return MERCED_OK;
    }
    case 0x3C: {
        /* Not an architected encoding (mov cr3=r2 is 0x2C).  Merced's PAL
         * uses it beside its model-specific register accesses (0x06/0x16),
         * e.g. writing "register 2" = 0 while it polls an MSR during a
         * service EFI calls at runtime.  Treating it as mov cr cleared
         * cr.iva there: EFI's timer interrupt then never fired again, so
         * the Boot Manager and loader countdowns froze until a key was
         * pressed.  No modelled state corresponds to it. */
        return MERCED_OK;
    }
    case 0x2C: {                                    /* mov cr3=r2 */
        unsigned cr3 = (unsigned)bits(raw, 20, 7);
        uint64_t crv = gr_read(m, r2, &n2);
        if (n2)
            return deliver_fault(m, VEC_NAT, UINT64_C(0x10), 0, false);
        /* CR.PTA: size below 15 and the reserved bits fault. */
        if (cr3 == CR_PTA && (((crv >> 2) & 0x3F) < 15 || (crv & UINT64_C(0x7E02))))
            return deliver_fault(m, VEC_GENERAL, 0x30, 0, false);
        m->cr[cr3] = crv;
        if (cr3 == CR_IVA)
            m->cr[cr3] &= ~UINT64_C(0x7FFF);        /* 32 KiB aligned */
        if (cr3 == CR_EOI) {                        /* retire highest in-service */
            unsigned isv = in_service_highest(m);
            if (isv)
                m->ext_in_service[isv >> 3] &= (uint8_t)~(1u << (isv & 7));
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
        if (m->cr[CR_IFA] >= watch_va_base && m->cr[CR_IFA] < watch_va_end) {
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
    case 0x30:                                      /* fc / fc.i */
        /* A cache-array model holding the line checks it, writes it back
         * and drops it (merced_cache.c; physical mode -- fc names the line
         * whatever the attribute of the address used). */
        if (__builtin_expect(m->cache.observe, 0) && !(m->psr & PSR_DT))
            merced_cache_fc(m, model_pa(m, gr_read(m, r3, &n3)));
        return MERCED_OK;
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
        xlate_flush(m);
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

    /* Unconditional compares and tests clear both destination predicates
     * even when their qualifying predicate is false.  A-unit compares are
     * handled by exec_alu(); I-unit tbit/tnat .unc need the same treatment
     * before discarding other predicated-off instructions. */
    if (major_is_alu(major)) {
        if (exec_alu(m, raw, qp, &st)) return st;
        if (st != MERCED_OK) return st;
    }
    if (!qp) {
        if (major == 5 && bits(raw, 33, 4) == 0 && bits(raw, 12, 1))
            set_preds(m, (unsigned)bits(raw, 6, 6),
                      (unsigned)bits(raw, 27, 6), 0, 0, 1);
        return MERCED_OK;
    }

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
                uint64_t imm = (uint64_t)sext((bits(raw, 36, 1) << 7) | bits(raw, 13, 7), 8);
                MercedStatus ast;
                if (ar_write_blocked(m, ar3, true, imm, 0, &ast)) return ast;
                m->ar[ar3] = ar_value_for_write(m, ar3, imm);
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
                uint64_t v = m->ar[ar3];
                MercedStatus ast;
                if (ar_read_blocked(m, ar3, true, &v, &ast)) return ast;
                gr_write(m, r1, v, 0);
                return MERCED_OK;
            }
            case 0x33: {                            /* mov r1=pr */
                if (!qp) return MERCED_OK;
                /* Broadside predicate transfers ignore CFM.rrb.pr. An
                 * interrupt must save the physical bits before cover
                 * clears RRB, so a later restore preserves the pipeline. */
                gr_write(m, r1, m->pr | 1, 0);
                return MERCED_OK;
            }
            case 0x2A: {                            /* mov.i ar3=r2 */
                if (!qp) return MERCED_OK;
                unsigned ar3 = (unsigned)bits(raw, 20, 7);
                uint64_t ar_val = gr_read(m, r2, &n2);
                MercedStatus ast;
                if (ar_write_blocked(m, ar3, true, ar_val, n2, &ast)) return ast;
                m->ar[ar3] = ar_value_for_write(m, ar3, ar_val);
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
            if (n2) return deliver_fault(m, VEC_NAT, 0, 0, false);
            m->pr = (m->pr & ~mask) | (v & mask) | 1;
            return MERCED_OK;
        }
        if (x3 == 2) {                              /* mov pr.rot=imm44 */
            if (!qp) return MERCED_OK;
            /* Sign-extend the encoded 28-bit immediate into physical
             * PR[16..63], independently of the rotation base. */
            uint64_t imm = (uint64_t)sext((bits(raw, 36, 1) << 27) |
                                         bits(raw, 6, 27), 28) << 16;
            m->pr = (m->pr & UINT64_C(0xffff)) | imm;
            return MERCED_OK;
        }
        if (x3 == 1) {                              /* chk.s.i */
            if (!qp) return MERCED_OK;
            gr_read(m, r2, &n2);
            if (n2)
                return chk_recover(m, 1, (bits(raw, 36, 1) << 20) |
                                   (bits(raw, 20, 13) << 7) | bits(raw, 6, 7));
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
        if (x2a == 0 && ((za == 0 && zb == 1) || (za == 1 && zb == 0)) &&
            ((x2c == 0 && (x2b == 0 || x2b == 2)) ||        /* pshr.u / pshr */
             (x2c == 1 && x2b == 0))) {                     /* pshl */
            if (!qp) return MERCED_OK;
            bool shl = x2c == 1;
            uint8_t nd, nc;
            /* pshr takes its data from r3 and its count from r2; pshl the
             * reverse.  Counts above the lane width empty (or sign-fill) it. */
            uint64_t data = gr_read(m, shl ? r2 : r3, &nd);
            uint64_t cnt  = gr_read(m, shl ? r3 : r2, &nc);
            unsigned lane_bits = za ? 32 : 16;
            uint64_t lane_mask = (UINT64_C(1) << lane_bits) - 1, res = 0;
            for (unsigned shift = 0; shift < 64; shift += lane_bits) {
                uint64_t lane = (data >> shift) & lane_mask, v;
                if (shl)
                    v = cnt >= lane_bits ? 0 : (lane << cnt) & lane_mask;
                else if (x2b == 0)
                    v = cnt >= lane_bits ? 0 : lane >> cnt;
                else
                    v = (uint64_t)(sext(lane, lane_bits) >>
                                   (cnt >= lane_bits ? lane_bits - 1 : cnt)) & lane_mask;
                res |= v << shift;
            }
            gr_write(m, r1, res, nd | nc);
            return MERCED_OK;
        }
        if (x2a == 1 && x2c == 0 && (x2b == 1 || x2b == 3) &&
            ((za == 0 && zb == 1) || (za == 1 && zb == 0))) {   /* pshr imm */
            if (!qp) return MERCED_OK;
            uint64_t data = gr_read(m, r3, &n3), res = 0;
            unsigned cnt = (unsigned)bits(raw, 14, 5);
            unsigned lane_bits = za ? 32 : 16;
            uint64_t lane_mask = (UINT64_C(1) << lane_bits) - 1;
            for (unsigned shift = 0; shift < 64; shift += lane_bits) {
                uint64_t lane = (data >> shift) & lane_mask, v;
                if (x2b == 1)
                    v = cnt >= lane_bits ? 0 : lane >> cnt;
                else
                    v = (uint64_t)(sext(lane, lane_bits) >>
                                   (cnt >= lane_bits ? lane_bits - 1 : cnt)) & lane_mask;
                res |= v << shift;
            }
            gr_write(m, r1, res, n3);
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
                /* mix.r keeps the right (even-numbered) element of each pair,
                 * mix.l the left one; r2's go to the left of r3's. */
                unsigned off = (x2b == 0) ? 0 : 1;
                for (unsigned k = 0; k < n / 2; k++) {
                    PUT(2 * k,     ELEM(s2, 2 * k + off));
                    PUT(2 * k + 1, ELEM(s1, 2 * k + off));
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
                /* The mnemonic's size is the source element: pack2 narrows
                 * halfwords to bytes, pack4 words to halfwords.
                 * x2b=0 unsigned-sat (pack2.uss), x2b=2 signed-sat. */
                unsigned dbytes = esz / 2;           /* dest element size */
                unsigned sbytes = esz;               /* src element size */
                unsigned sn = 8 / sbytes;            /* src elems per reg */
                uint64_t smask = (1ull << (sbytes * 8)) - 1;
                for (unsigned k = 0; k < sn; k++) {
                    for (int src = 0; src < 2; src++) {
                        uint64_t sv = src ? s2 : s1;       /* r2 low, r3 high */
                        int64_t e = (int64_t)((sv >> (k * sbytes * 8)) & smask);
                        e = sext((uint64_t)e, sbytes * 8);
                        int64_t lim_hi = (1ll << (dbytes * 8 - 1)) - 1;
                        int64_t lim_lo, out;
                        if (x2b == 0) { lim_lo = 0; lim_hi = (1ll << (dbytes * 8)) - 1; }
                        else lim_lo = -(1ll << (dbytes * 8 - 1));
                        out = e < lim_lo ? lim_lo : e > lim_hi ? lim_hi : e;
                        unsigned di = k + (src ? sn : 0);
                        res |= ((uint64_t)out & ((1ull << (dbytes * 8)) - 1)) << (di * dbytes * 8);
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
                    [0x0] = {0,0,0,0,0,0,0,0},          /* @brcst (low byte) */
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
        case 0x09:                                  /* implementation-specific:
                                                     * PAL's rfi into microcode */
        case 0x08: {                                /* rfi */
            if (qp) {
                MercedStatus pst;
                if (privileged_fault(m, &pst)) return pst;
            }
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
                m->bof = (m->bof + MERCED_RSE_CAPACITY -
                          CFM_SOF(new_cfm)) % MERCED_RSE_CAPACITY;
                m->bof_total -= CFM_SOF(new_cfm);
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
            /* Mandatory fills fault in the restored target context, with
             * ISR.ir set, and retry through a subsequent rfi. */
            MercedStatus fill_status;
            if (!rse_fill_current_frame(m, &fill_status))
                return fill_status;
            return rse_spill_excess(m);
        }
        case 0x0C: case 0x0D: {                     /* bsw.0 / bsw.1 */
            if (!qp) return MERCED_OK;
            MercedStatus pst;
            if (privileged_fault(m, &pst)) return pst;
            if (x6 == 0x0C) m->psr &= ~PSR_BN; else m->psr |= PSR_BN;
            return MERCED_OK;
        }
        case 0x10: {                                /* epc */
            /* SDM Vol.3 "epc": PFS.ppl below the current level would let
             * a following br.ret keep the promoted privilege (ar.pfs is
             * user-writable), so that is an Illegal Operation fault. */
            unsigned cpl = (unsigned)((m->psr >> PSR_CPL_SHIFT) & 3);
            if (((m->ar[AR_PFS] >> 62) & 3) < cpl)
                return deliver_fault(m, VEC_GENERAL, 0, 0, false);
            unsigned new_cpl = cpl;
            if (!(m->psr & PSR_IT)) {
                new_cpl = 0;
            } else {
                /* Promote to the page's privilege level when the epc's
                 * own page has execute/promote rights (TLB.ar = 7). */
                const MercedTlbEntry *e = itlb_entry(m, m->ip);
                if (e && ((e->pte >> PTE_AR_SHIFT) & 7) == 7) {
                    unsigned pl = (unsigned)((e->pte >> PTE_PL_SHIFT) & 3);
                    if (pl < cpl)
                        new_cpl = pl;
                }
            }
            m->psr = (m->psr & ~(UINT64_C(3) << PSR_CPL_SHIFT)) |
                     ((uint64_t)new_cpl << PSR_CPL_SHIFT);
            return MERCED_OK;
        }
        case 0x20: {                                /* br.cond/br.ia b2 */
            if (!qp) return MERCED_OK;
            if (btype == 1) {
                unsigned b = (unsigned)bits(raw, 13, 3);
                if (m->psr & PSR_DI)
                    return deliver_fault(m, VEC_GENERAL, 0x40, 0, false);
                /* An ISA transition made with translation off or interruption
                 * collection disabled is undefined (SDM vol. 2, 9.1.2).  The
                 * processor leaves it untried and execution falls through:
                 * the stepping microcode hand-offs in PAL_A rely on it. */
                if (!(m->psr & PSR_IT) || !(m->psr & PSR_DT) || !(m->psr & PSR_IC))
                    return MERCED_OK;
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
        case 0x21: {                                /* br.ret b2 */
            if (!qp) return MERCED_OK;
            do_ret(m, m->br[bits(raw, 13, 3)] & ~0xFull);
            /* SDM Vol.2 6.6: a mandatory return fill faults at the target
             * instruction, after BOF/CFM have changed, rather than replaying
             * br.ret and popping the register frame a second time. */
            MercedStatus fill_status;
            if (!rse_fill_current_frame(m, &fill_status))
                return fill_status;
            return rse_spill_excess(m);
        }
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
    /* A reserved B-unit opcode is an Illegal Operation fault. */
    return deliver_fault(m, VEC_GENERAL, 0, 0, false);
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

/* Floating-point Fault conditions (vector 0x5C00, ISR.code): V invalid
 * operation, D denormal/unnormal operand, Z divide by zero, SWA software
 * assist (what this processor reports for a denormal operand). */
enum { FPF_V = 1u << 0, FPF_D = 1u << 1, FPF_Z = 1u << 2, FPF_SWA = 1u << 3 };

static bool frc_nan(const MercedFpReg *f) { return f->exp == 0x1FFFFu && (f->sig << 1) != 0; }
static bool frc_snan(const MercedFpReg *f) { return frc_nan(f) && !(f->sig & (UINT64_C(1) << 62)); }
static bool frc_inf(const MercedFpReg *f) { return f->exp == 0x1FFFFu && f->sig == (UINT64_C(1) << 63); }
static bool frc_zero(const MercedFpReg *f) { return f->exp == 0 && f->sig == 0; }
/* Pseudo-NaN and pseudo-infinity (exponent all ones, integer bit clear) are
 * unsupported encodings: an Invalid Operation. */
static bool frc_unsupported(const MercedFpReg *f) {
    return f->exp == 0x1FFFFu && !(f->sig & (UINT64_C(1) << 63));
}
/* A non-zero value with its integer bit clear is unnormal; exponent 0 with a
 * non-zero significand is the usual denormal. */
static bool frc_denorm_or_unnormal(const MercedFpReg *f) {
    if (f->exp == 0x1FFFFu || frc_zero(f)) return false;
    return f->exp == 0 || !(f->sig & (UINT64_C(1) << 63));
}

/* The faults f3*f4 (+/-) f2 raises from its operands alone. */
static unsigned fp_fault_fma(const MercedFpReg *a, const MercedFpReg *b,
                             const MercedFpReg *d, bool neg_prod, bool neg_add) {
    unsigned f = 0;
    if (frc_snan(a) || frc_snan(b) || frc_snan(d)) f |= FPF_V;
    if (frc_unsupported(a) || frc_unsupported(b) || frc_unsupported(d)) f |= FPF_V;
    if ((frc_inf(a) && frc_zero(b)) || (frc_zero(a) && frc_inf(b)))
        f |= FPF_V;                                         /* inf * 0 */
    if ((frc_inf(a) || frc_inf(b)) && frc_inf(d)) {
        bool psign = (a->sign ^ b->sign) ^ neg_prod;
        if (psign != (d->sign ^ neg_add)) f |= FPF_V;       /* inf - inf */
    }
    if (frc_denorm_or_unnormal(a) || frc_denorm_or_unnormal(b) ||
        frc_denorm_or_unnormal(d))
        f |= FPF_D;
    return f;
}

/* V for the operations that take their operands as values: any signalling
 * NaN, and a quiet NaN for the signalling relations and the min/max family. */
static unsigned fp_fault_operands(const MercedFpReg *x, const MercedFpReg *y,
                                  bool quiet_nan_too) {
    unsigned f = 0;
    if (frc_snan(x) || (y && frc_snan(y))) f |= FPF_V;
    if (frc_unsupported(x) || (y && frc_unsupported(y))) f |= FPF_V;
    if (quiet_nan_too && (frc_nan(x) || (y && frc_nan(y)))) f |= FPF_V;
    if (frc_denorm_or_unnormal(x) || (y && frc_denorm_or_unnormal(y)))
        f |= FPF_D;
    return f;
}

/* The enabled subset of f: the FPSR trap-disable bits vd, dd and zd, and the
 * status field's td which disables all of them. */
static unsigned fp_fault_enabled(const Merced *m, uint64_t raw, unsigned f) {
    if (!f)
        return 0;
    uint64_t fpsr = m->ar[AR_FPSR];
    unsigned sh = 6u + 13u * (unsigned)bits(raw, 34, 2);
    if ((fpsr >> sh) & (1u << 6))
        return 0;
    unsigned out = 0;
    if ((f & FPF_V) && !(fpsr & 1u)) out |= FPF_V;
    if ((f & FPF_Z) && !((fpsr >> 2) & 1u)) out |= FPF_Z;
    /* This processor reports a denormal operand as a software assist. */
    if ((f & (FPF_D | FPF_SWA)) && !((fpsr >> 1) & 1u)) out |= FPF_SWA;
    return out;
}

/* Take the Floating-point Fault if any enabled condition is raised; false
 * otherwise. */
static bool fp_fault_check(Merced *m, uint64_t raw, unsigned f, MercedStatus *st) {
    unsigned code = fp_fault_enabled(m, raw, f);
    if (!code)
        return false;
    *st = deliver_fault(m, VEC_FP_FAULT, code, 0, false);
    return true;
}

/* Turn an operation's IEEE status into FPSR flags or, with the trap enabled,
 * a Floating-point Trap (vector 0x5D00).  A trap reports where execution
 * resumes, after the instruction; ISR.ei names the instruction itself. */
static MercedStatus fp_report(Merced *m, uint64_t raw, unsigned status) {
    if (!status)
        return MERCED_OK;
    unsigned sf = (unsigned)bits(raw, 34, 2);
    unsigned sh = 6 + 13 * sf;
    uint64_t fpsr = m->ar[AR_FPSR];
    bool td = ((fpsr >> sh) & (1u << 6)) != 0;
    uint64_t code = 0;
    if ((status & MERCED_FP_O) && !td && !((fpsr >> 3) & 1)) code |= 1u << 11;
    if ((status & MERCED_FP_U) && !td && !((fpsr >> 4) & 1)) code |= 1u << 12;
    if ((status & MERCED_FP_I) && !td && !((fpsr >> 5) & 1)) code |= 1u << 13;
    /* FPA rides with a trap already being taken; it never raises one. */
    if (code && (status & MERCED_FP_FPA)) code |= 1u << 14;
    if (!code) {
        uint64_t fl = 0;
        if (status & MERCED_FP_O) fl |= 1u << 3;
        if (status & MERCED_FP_U) fl |= 1u << 4;
        if (status & MERCED_FP_I) fl |= 1u << 5;
        m->ar[AR_FPSR] |= fl << (sh + 7u);
        return MERCED_OK;
    }
    unsigned slot = (unsigned)(m->ip & 3);
    m->ip = slot == 2 ? (m->ip & ~UINT64_C(0xF)) + 16 : (m->ip | 0) + 1;
    MercedStatus st = deliver_fault(m, VEC_FP_TRAP, code | 1u, 0, false);
    m->cr[CR_ISR] = (m->cr[CR_ISR] & ~(UINT64_C(3) << 41)) |
                    ((uint64_t)slot << 41);
    return st;
}

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
        if (!x && major == 1 && x6 >= 0x30 && x6 <= 0x37) {
            /* fpcmp: each single of the pair against its counterpart, the
             * outcome a mask of ones or zeros in the matching half of f1. */
            if (!qp) return MERCED_OK;
            MercedFpReg a = fr_read(m, f2), b = fr_read(m, f3);
            uint64_t pair = 0;
            for (unsigned i = 0; i < 2; i++) {
                uint32_t ua = (uint32_t)(a.sig >> (32 * i));
                uint32_t ub = (uint32_t)(b.sig >> (32 * i));
                float p, q;
                memcpy(&p, &ua, 4);
                memcpy(&q, &ub, 4);
                bool un = isnan(p) || isnan(q), r;
                switch (x6 & 7) {
                case 0:  r = !un && p == q; break;          /* eq */
                case 1:  r = !un && p < q; break;           /* lt */
                case 2:  r = !un && p <= q; break;          /* le */
                case 3:  r = un; break;                     /* unord */
                case 4:  r = un || p != q; break;           /* neq */
                case 5:  r = un || !(p < q); break;         /* nlt */
                case 6:  r = un || !(p <= q); break;        /* nle */
                default: r = !un; break;                    /* ord */
                }
                if (r)
                    pair |= (uint64_t)0xFFFFFFFFu << (32 * i);
            }
            MercedFpReg pr = {pair, 0x1003E, 0, (uint8_t)(a.nat | b.nat)};
            fr_write(m, f1, pr);
            return MERCED_OK;
        }
        if (!x) {
            switch (x6) {
            case 0x00: {                            /* break.f */
                uint64_t imm = (bits(raw, 36, 1) << 20) | bits(raw, 6, 20);
                if (!qp) return MERCED_OK;
                m->cr[CR_IIM] = imm;
                return deliver_fault(m, VEC_BREAK, 0, 0, false);
            }
            case 0x01: return MERCED_OK;            /* nop.f / hint.f */
            case 0x04: {                            /* fsetc.sf */
                if (!qp) return MERCED_OK;
                unsigned shift = 6u + 13u * (unsigned)bits(raw, 34, 2);
                uint64_t amask = bits(raw, 13, 7), omask = bits(raw, 20, 7);
                uint64_t sf = m->ar[AR_FPSR] >> shift & 0x1FFF;
                sf = (sf & (amask | ~0x7Full)) | omask;
                m->ar[AR_FPSR] = (m->ar[AR_FPSR] & ~(0x1FFFull << shift)) | (sf << shift);
                return MERCED_OK;
            }
            case 0x05:                              /* fclrf.sf */
                if (qp) m->ar[AR_FPSR] &= ~(0x3Full << (13u + 13u * (unsigned)bits(raw, 34, 2)));
                return MERCED_OK;
            case 0x08: return MERCED_OK;            /* fchkf */
            case 0x10: case 0x11: case 0x12: {      /* fmerge.s/ns/se, fpmerge */
                if (!qp) return MERCED_OK;
                MercedFpReg a = fr_read(m, f2), b = fr_read(m, f3), r = b;
                if (major == 1) {
                    /* fpmerge: each single of the pair on its own */
                    uint64_t pair = 0;
                    for (unsigned i = 0; i < 2; i++) {
                        uint32_t la = (uint32_t)(a.sig >> (32 * i));
                        uint32_t lb = (uint32_t)(b.sig >> (32 * i)), v;
                        if (x6 == 0x10)      v = (lb & 0x7FFFFFFFu) | (la & 0x80000000u);
                        else if (x6 == 0x11) v = (lb & 0x7FFFFFFFu) | (~la & 0x80000000u);
                        else                 v = (lb & 0x007FFFFFu) | (la & 0xFF800000u);
                        pair |= (uint64_t)v << (32 * i);
                    }
                    MercedFpReg pr = {pair, 0x1003E, 0, (uint8_t)(a.nat | b.nat)};
                    fr_write(m, f1, pr);
                    return MERCED_OK;
                }
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
                if (qp && major == 0) {
                    /* a comparison, so any NaN operand is invalid */
                    MercedStatus fst;
                    if (fp_fault_check(m, raw, fp_fault_operands(&a, &b, true), &fst))
                        return fst;
                }
                if (a.nat || b.nat) {
                    MercedFpReg nat = {0, 0, 0, 1};
                    fr_write(m, f1, nat);
                    return MERCED_OK;
                }
                if (major == 1) {
                    /* fpmin/fpmax/fpamin/fpamax on the pair of singles */
                    uint64_t pair = 0;
                    for (unsigned i = 0; i < 2; i++) {
                        uint32_t ua = (uint32_t)(a.sig >> (32 * i));
                        uint32_t ub = (uint32_t)(b.sig >> (32 * i));
                        float x, y;
                        memcpy(&x, &ua, 4);
                        memcpy(&y, &ub, 4);
                        float cx = x6 >= 0x16 ? fabsf(x) : x;
                        float cy = x6 >= 0x16 ? fabsf(y) : y;
                        bool sel_a = (x6 & 1) ? (cx > cy) : (cx < cy);
                        pair |= (uint64_t)(sel_a ? ua : ub) << (32 * i);
                    }
                    MercedFpReg pr = {pair, 0x1003E, 0, 0};
                    fr_write(m, f1, pr);
                    return MERCED_OK;
                }
                MercedFpReg ca = a, cb = b;
                if (x6 >= 0x16) {                   /* famin / famax: magnitudes */
                    ca.sign = 0;
                    cb.sign = 0;
                }
                int cmp = merced_fp_compare(ca, cb);
                bool take_a = (x6 == 0x15 || x6 == 0x17) ? cmp == 1 : cmp == -1;
                fr_write(m, f1, take_a ? a : b);
                return MERCED_OK;
            }
            case 0x18: case 0x19: case 0x1A: case 0x1B: {
                /* fcvt.fx / fcvt.fxu / .trunc: exact, on the register fields */
                if (!qp) return MERCED_OK;
                MercedFpReg a = fr_read(m, f2);
                MercedFpReg r = {0, 0x1003E, 0, a.nat};
                if (major == 0) {
                    /* a NaN or infinity has no integer value, nor does a value
                     * outside the range (integer invalid) */
                    unsigned flt = fp_fault_operands(&a, NULL, true);
                    if (frc_inf(&a)) flt |= FPF_V;
                    bool bad = false;
                    (void)merced_fp_to_int(a, (x6 & 2) != 0, (x6 & 1) != 0, &bad);
                    if (bad) flt |= FPF_V;
                    MercedStatus fst;
                    if (fp_fault_check(m, raw, flt, &fst)) return fst;
                }
                if (major == 1) {
                    /* fpcvt.fx: the pair of singles as two 32-bit integers */
                    for (unsigned i = 0; i < 2; i++) {
                        uint32_t ua = (uint32_t)(a.sig >> (32 * i)), v;
                        float x;
                        memcpy(&x, &ua, 4);
                        float t = (x6 & 2) ? truncf(x) : rintf(x);
                        v = (x6 & 1) ? (uint32_t)t : (uint32_t)(int32_t)t;
                        r.sig |= (uint64_t)v << (32 * i);
                    }
                    fr_write(m, f1, r);
                    return MERCED_OK;
                }
                r.sig = merced_fp_to_int(a, (x6 & 2) != 0, (x6 & 1) != 0, NULL);
                fr_write(m, f1, r);
                return MERCED_OK;
            }
            case 0x1C: {                            /* fcvt.xf (int to fp) */
                if (!qp) return MERCED_OK;
                MercedFpReg a = fr_read(m, f2), r;
                r = merced_fp_from_int((int64_t)a.sig);
                r.nat = a.nat;
                fr_write(m, f1, r);
                return MERCED_OK;
            }
            case 0x28: {                            /* fpack: two singles */
                if (!qp) return MERCED_OK;
                MercedFpReg a = fr_read(m, f2), b = fr_read(m, f3);
                MercedFpReg r = {0, 0x1003E, 0, (uint8_t)(a.nat | b.nat)};
                r.sig = ((uint64_t)merced_fp_to_single_bits(a) << 32) |
                        merced_fp_to_single_bits(b);
                fr_write(m, f1, r);
                return MERCED_OK;
            }
            case 0x2C: case 0x2D: case 0x2E: case 0x2F: {   /* fand/fandcm/for/fxor */
                if (!qp) return MERCED_OK;
                MercedFpReg a = fr_read(m, f2), b = fr_read(m, f3);
                MercedFpReg r = {0, 0x1003E, 0, (uint8_t)(a.nat | b.nat)};
                switch (x6) {
                case 0x2C: r.sig = a.sig & b.sig; break;
                case 0x2D: r.sig = a.sig & ~b.sig; break;
                case 0x2E: r.sig = a.sig | b.sig; break;
                default:   r.sig = a.sig ^ b.sig; break;
                }
                fr_write(m, f1, r);
                return MERCED_OK;
            }
            case 0x34: case 0x35: case 0x36: {      /* fswap, .nl, .nr */
                if (!qp) return MERCED_OK;
                MercedFpReg a = fr_read(m, f2), b = fr_read(m, f3);
                MercedFpReg r = {0, 0x1003E, 0, (uint8_t)(a.nat | b.nat)};
                /* {left, right} = {f3's right, f2's left}, with the left or
                 * the right half negated for .nl / .nr */
                uint32_t hi = (uint32_t)b.sig, lo = (uint32_t)(a.sig >> 32);
                if (x6 == 0x35) hi ^= 0x80000000u;
                if (x6 == 0x36) lo ^= 0x80000000u;
                r.sig = ((uint64_t)hi << 32) | lo;
                fr_write(m, f1, r);
                return MERCED_OK;
            }
            case 0x39: {                            /* fmix.lr */
                if (!qp) return MERCED_OK;
                MercedFpReg a = fr_read(m, f2), b = fr_read(m, f3);
                MercedFpReg r = {0, 0x1003E, 0, (uint8_t)(a.nat | b.nat)};
                r.sig = (a.sig & UINT64_C(0xFFFFFFFF00000000)) |
                        (b.sig & UINT64_C(0xFFFFFFFF));
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
                case 0x3A: hi = a_lo; lo = b_lo; break;               /* fmix.r */
                case 0x3B: hi = a_hi; lo = b_hi; break;               /* fmix.l */
                /* fsxt selects one 32-bit lane from f3 for the low half,
                 * with the corresponding f2 lane's sign in the high half.
                 * Windows uses fsxt.r f6=f0,f13 to truncate a hash product
                 * to 32 bits before its integer division sequence. */
                case 0x3C: hi = (a_lo >> 31) ? UINT32_MAX : 0;
                           lo = b_lo; break;                /* fsxt.r */
                default:   hi = (a_hi >> 31) ? UINT32_MAX : 0;
                           lo = b_hi; break;                /* fsxt.l */
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
        /* F6 frcpa (q=0) and F7 frsqrta (q=1): the approximation goes to f1,
         * a valid-approximation flag to p2. */
        if (!qp) return MERCED_OK;
        unsigned p2 = (unsigned)bits(raw, 27, 6);
        MercedFpReg a = fr_read(m, f2), b = fr_read(m, f3), res;
        if (a.nat || b.nat) {
            MercedFpReg nat = {0, 0, 0, 1};
            fr_write(m, f1, nat);
            pr_write(m, p2, 0);
            return MERCED_OK;
        }
        MercedStatus fst;
        if (q) {
            /* the square root of a negative number (including -inf) is invalid */
            unsigned flt = fp_fault_operands(&b, NULL, false);
            if (b.sign && !frc_zero(&b) && !frc_nan(&b)) flt |= FPF_V;
            if (fp_fault_check(m, raw, flt, &fst)) return fst;
        } else {
            /* 0/0 and inf/inf are invalid; x/0 is a divide by zero */
            unsigned flt = fp_fault_operands(&a, &b, false);
            if ((frc_zero(&a) && frc_zero(&b)) || (frc_inf(&a) && frc_inf(&b)))
                flt |= FPF_V;
            else if (frc_zero(&b) && !frc_nan(&a))
                flt |= FPF_Z;
            if (fp_fault_check(m, raw, flt, &fst)) return fst;
        }
        bool valid = q ? merced_fp_frsqrta(b, &res) : merced_fp_frcpa(a, b, &res);
        fr_write(m, f1, res);
        pr_write(m, p2, valid);
        return MERCED_OK;
    }

    if (major >= 8 && major <= 0xD) {               /* F1 fma family */
        if (!qp) return MERCED_OK;
        unsigned xbit = (unsigned)bits(raw, 36, 1);
        if (xbit && (major & 1)) {
            /* fpma (parallel) - unimplemented */
            return mhalt(m, "unimpl F-unit fpma/parallel major %u", major);
        }
        MercedFpReg fa = fr_read(m, f3), fb = fr_read(m, f4), fc = fr_read(m, f2);
        if (fa.nat || fb.nat || fc.nat) {
            MercedFpReg nat = {0, 0, 0, 1};
            fr_write(m, f1, nat);
            return MERCED_OK;
        }
        MercedStatus fst;
        if (fp_fault_check(m, raw, fp_fault_fma(&fa, &fb, &fc, major >= 0xC,
                                                major == 0xA || major == 0xB), &fst))
            return fst;
        unsigned sf = (unsigned)bits(raw, 34, 2);
        unsigned ctl = (unsigned)((m->ar[AR_FPSR] >> (6 + 13 * sf)) & 0x7F);
        bool wre = (ctl >> 1) & 1u;
        unsigned dpc = (ctl >> 2) & 3u, rc = (ctl >> 4) & 3u;
        /* Odd major is double precision, even major with x=1 single, and an
         * even major with x=0 takes its precision from the status field. */
        unsigned prec, exp_bits;
        if (major & 1)    { prec = 53; exp_bits = wre ? 17u : 11u; }
        else if (xbit)    { prec = 24; exp_bits = wre ? 17u : 8u; }
        else {
            prec = dpc == 0 ? 24u : dpc == 2 ? 53u : 64u;   /* 1 is reserved */
            exp_bits = wre ? 17u : 15u;
        }
        unsigned status;
        MercedFpReg res = merced_fp_fma(fa, fb, fc, major >= 0xC,
                                        major == 0xA || major == 0xB, prec,
                                        exp_bits, rc, &status);
        fr_write(m, f1, res);
        return fp_report(m, raw, status);
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
        if (!qp) return MERCED_OK;                  /* F3 fselect f1 = f3, f4, f2 */
        MercedFpReg a3 = fr_read(m, f3), a4 = fr_read(m, f4), a2 = fr_read(m, f2);
        MercedFpReg r = {0, 0x1003E, 0, (uint8_t)(a3.nat | a4.nat | a2.nat)};
        /* The mask is f2: where it has a 1 the bit comes from f3. */
        r.sig = (a3.sig & a2.sig) | (a4.sig & ~a2.sig);
        fr_write(m, f1, r);
        return MERCED_OK;
    }

    if (major == 4) {                               /* F4 fcmp */
        if (!qp) return MERCED_OK;
        unsigned rb = (unsigned)bits(raw, 36, 1);
        unsigned ra = (unsigned)bits(raw, 33, 1);
        unsigned ta = (unsigned)bits(raw, 12, 1);
        unsigned p1 = (unsigned)bits(raw, 6, 6);
        unsigned p2 = (unsigned)bits(raw, 27, 6);
        MercedFpReg fa = fr_read(m, f2), fb = fr_read(m, f3);
        /* eq, lt and le are signalling relations: a quiet NaN raises V too;
         * unord is quiet. */
        MercedStatus fst;
        if (fp_fault_check(m, raw,
                           fp_fault_operands(&fa, &fb, ((ra << 1) | rb) != 3), &fst))
            return fst;
        int cmp = merced_fp_compare(fa, fb);
        int res;
        switch ((ra << 1) | rb) {
        case 0: res = cmp == 0; break;
        case 1: res = cmp == -1; break;
        case 2: res = cmp == -1 || cmp == 0; break;
        default: res = cmp == 2; break;                 /* unord */
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
        /* The class mask: 0x100 NaTVal, 0x080 quiet NaN, 0x040 signalling
         * NaN, 0x001 positive, 0x002 negative, 0x004 zero, 0x008 unnormal
         * (denormals included), 0x010 normal, 0x020 infinity. */
        if (a.nat) {
            res = (fclass & 0x100) != 0;
        } else if (frc_nan(&a)) {
            res = (frc_snan(&a) ? (fclass & 0x040) : (fclass & 0x080)) != 0;
        } else {
            bool sign_ok = ((fclass & 0x001) && !a.sign) || ((fclass & 0x002) && a.sign);
            bool type_ok = ((fclass & 0x004) && frc_zero(&a)) ||
                           ((fclass & 0x008) && frc_denorm_or_unnormal(&a)) ||
                           ((fclass & 0x010) && !frc_zero(&a) && !frc_inf(&a) &&
                            !frc_denorm_or_unnormal(&a) && !frc_unsupported(&a)) ||
                           ((fclass & 0x020) && frc_inf(&a));
            res = sign_ok && type_ok;
        }
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


static bool ia32_operand_addresses(Merced *m, uint64_t va, unsigned size,
                                    bool ifetch, uint64_t access,
                                    uint64_t pa[8], bool *contiguous) {
    MercedStatus st;
    if (!size || size > 8) return false;
    if (!va_translate(m, va, ifetch, false, access, &pa[0], &st))
        return false;
    *contiguous = true;
    /* Translation permissions are uniform within the minimum 4 KiB page.
     * Most operands need one lookup. A split operand must validate every
     * page before a bus read/write, including multi-byte instruction fetches.
     * Keep the full address: IA-32 I/O uses the 64-bit AR.KR0 aperture. */
    if ((va & 0xfff) <= 0x1000 - size) return true;
    for (unsigned i = 1; i < size; i++) {
        if (!va_translate(m, va + i, ifetch, false, access, &pa[i], &st))
            return false;
        if (pa[i] != pa[0] + i) *contiguous = false;
    }
    return true;
}

static bool ia32_read_access(Merced *m, uint64_t va, unsigned size,
                              bool ifetch, uint64_t access, uint64_t *value) {
    uint64_t pa[8];
    bool contiguous;
    if (!ia32_operand_addresses(m, va, size, ifetch, access,
                                  pa, &contiguous)) return false;
    if (contiguous) {
        *value = ifetch ? phys_fetch(m, pa[0], size) : phys_read(m, pa[0], size);
    } else {
        *value = 0;
        for (unsigned i = 0; i < size; i++) {
            uint64_t byte = ifetch ? phys_fetch(m, pa[i], 1) : phys_read(m, pa[i], 1);
            *value |= (byte & 0xff) << (8 * i);
        }
    }
    return true;
}

bool merced_ia32_read(Merced *m, uint64_t va, unsigned size,
                      bool ifetch, uint64_t *value) {
    return ia32_read_access(m, va, size, ifetch, ifetch ? ISR_X : ISR_R, value);
}

bool merced_ia32_read_for_write(Merced *m, uint64_t va, unsigned size,
                                uint64_t *value) {
    return ia32_read_access(m, va, size, false, ISR_R | ISR_W, value);
}

bool merced_ia32_probe_write(Merced *m, uint64_t va, unsigned size) {
    uint64_t pa[8];
    bool contiguous;
    return ia32_operand_addresses(m, va, size, false, ISR_W, pa, &contiguous);
}

bool merced_ia32_write(Merced *m, uint64_t va, unsigned size,
                       uint64_t value) {
    uint64_t pa[8];
    bool contiguous;
    if (!ia32_operand_addresses(m, va, size, false, ISR_W, pa, &contiguous)) return false;
    if (contiguous) {
        phys_write(m, pa[0], value, size);
    } else {
        for (unsigned i = 0; i < size; i++)
            phys_write(m, pa[i], (value >> (8 * i)) & 0xff, 1);
    }
    return true;
}

static bool ia32_read_modify_write(Merced *m, uint64_t va, unsigned size,
                                    uint64_t acc, uint64_t src, uint64_t *dest,
                                    bool compare) {
    uint64_t pa[8];
    /* IA-32 read-modify-write faults report both ISR.r and ISR.w. Check
     * every byte before any bus side effect: an unaligned operand can span
     * discontiguous pages, and a mismatch still requires write permission. */
    bool contiguous;
    if (!ia32_operand_addresses(m, va, size, false, ISR_R | ISR_W,
                                  pa, &contiguous)) return false;
    uint64_t mask = size == 8 ? UINT64_MAX : (UINT64_C(1) << (size * 8)) - 1;
    if (contiguous) {
        *dest = phys_read(m, pa[0], size) & mask;
    } else {
        *dest = 0;
        for (unsigned i = 0; i < size; i++)
            *dest |= (phys_read(m, pa[i], 1) & 0xff) << (8 * i);
    }
    uint64_t value = compare ? (acc == *dest ? src : *dest) : (*dest + src) & mask;
    if (contiguous) {
        phys_write(m, pa[0], value, size);
    } else {
        for (unsigned i = 0; i < size; i++)
            phys_write(m, pa[i], (value >> (8 * i)) & 0xff, 1);
    }
    return true;
}

bool merced_ia32_cmpxchg(Merced *m, uint64_t va, unsigned size,
                         uint64_t acc, uint64_t src, uint64_t *dest) {
    return ia32_read_modify_write(m, va, size, acc, src, dest, true);
}

bool merced_ia32_xadd(Merced *m, uint64_t va, unsigned size,
                      uint32_t src, uint32_t *dest) {
    uint64_t old;
    if (!ia32_read_modify_write(m, va, size, 0, src, &old, false)) return false;
    *dest = (uint32_t)old;
    return true;
}

bool merced_ia32_read_operand(Merced *m, uint64_t va, unsigned size,
                              uint32_t *value) {
    uint64_t result;
    if (!merced_ia32_read(m, va, size, false, &result)) return false;
    *value = (uint32_t)result;
    return true;
}

bool merced_ia32_write_operand(Merced *m, uint64_t va, unsigned size,
                               uint32_t value) {
    return merced_ia32_write(m, va, size, value);
}

MercedStatus merced_ia32_disabled_transition(Merced *m) {
    return deliver_fault(m, VEC_GENERAL, 0x40, 0, false);
}

MercedStatus merced_ia32_disabled_fp(Merced *m) {
    return deliver_fault(m, VEC_DISABLED_FP, 1, 0, false);
}

MercedStatus merced_ia32_fp_exception(Merced *m, unsigned vector) {
    /* #NM (7) and delayed x87 #MF (16) use the IA-32 Exception vector.
     * Itanium delivers #MF regardless of CFLG.CR0.ne; there is no IRQ13
     * fallback. FCR/FSR and the saved numeric pointers remain untouched. */
    return deliver_fault(m, 0x6900, (uint64_t)vector << 16, 0, false);
}

MercedStatus merced_ia32_instruction_intercept(Merced *m, uint64_t iim,
                                                uint16_t code) {
    /* IA-32 system instructions and invalid opcodes are intercepted into
     * the native IVT. ISR.vector zero identifies an instruction intercept;
     * IIM holds the instruction bytes after prefixes, zero padded. */
    m->cr[CR_IIM] = iim;
    return deliver_fault(m, VEC_IA32_INTERCEPT, code, 0, false);
}

/* Software interrupts are traps: IIP is the next instruction. INT n is an
 * IA_32_Interrupt (IVA+0x6B00); INT 3 is an IA_32_Exception (0x6900). */
MercedStatus merced_ia32_software_interrupt(Merced *m, unsigned vector,
                                            bool int_n) {
    return deliver_fault(m, int_n ? 0x6B00 : 0x6900,
                         (uint64_t)(vector & 0xFF) << 16, 0, false);
}

/* IA_32_Intercept(SystemFlag), a trap after CLI (0), STI (1) or POPF (2)
 * changed EFLAG.if with CFLG.ii set; IIM holds the previous EFLAG. */
MercedStatus merced_ia32_sysflag_trap(Merced *m, unsigned kind,
                                      uint64_t old_eflag) {
    m->cr[CR_IIM] = old_eflag;
    return deliver_fault(m, VEC_IA32_INTERCEPT,
                         (UINT64_C(2) << 16) | ((uint64_t)kind << 14), 0,
                         false);
}

uint64_t merced_ia32_gr_read(Merced *m, unsigned reg) {
    return gr_read(m, reg, NULL);
}

void merced_ia32_gr_write(Merced *m, unsigned reg, uint64_t value) {
    gr_write(m, reg, value, 0);
}

/* nop.m / nop.i / nop.f / nop.b and their hint/brp siblings: the encodings
 * whose exec_* handler returns MERCED_OK straight away, with no effect,
 * whatever the predicate.  Compilers pad most bundles with them (close to
 * half of all slots in firmware), so they skip the decoders entirely. */
static inline bool slot_is_nop(char unit, uint64_t raw) {
    unsigned major = (unsigned)(raw >> 37) & 0xF;
    unsigned x6 = (unsigned)(raw >> 27) & 0x3F;
    switch (unit) {
    case 'M':   /* M48: x3=0 (35:33), x2=0 (32:31), x4=1 (30:27) */
    case 'I':   /* I18: x3=0 (35:33), x6=1 (32:27) */
        return major == 0 && ((raw >> 27) & 0x1FF) == 1;
    case 'F':   /* F16: x (33)=0, x6=1 */
        return major <= 1 && ((raw >> 33) & 1) == 0 && x6 == 1;
    case 'B':   /* B9 nop.b/hint.b, and brp (B6/B7 hints) */
        return major == 2 && (x6 == 0x00 || x6 == 0x01 || x6 == 0x10 || x6 == 0x11);
    default:
        return false;
    }
}

/* Choose fast_op()'s specialized handler for one SLOT_EXEC instruction:
 * only encodings it executes exactly as the generic handler fn would
 * (the same operand reads, NaT rules, faults and state updates in the same
 * order); everything else stays FOP_GENERIC. */
static void tb_decode_op(MercedTbEntry *tb, unsigned i, char u, uint64_t raw) {
    unsigned major = (unsigned)bits(raw, 37, 4);
    tb->op[i] = FOP_GENERIC;
    tb->a1[i] = (uint8_t)bits(raw, 6, 7);
    tb->a2[i] = (uint8_t)bits(raw, 13, 7);
    tb->a3[i] = (uint8_t)bits(raw, 20, 7);
    tb->x[i] = 0;
    tb->imm[i] = 0;
    bool a_unit = u == 'M' || u == 'I';

    if (a_unit && major == 8) {              /* exec_alu() major 8 */
        unsigned x2a = (unsigned)bits(raw, 34, 2);
        unsigned ve  = (unsigned)bits(raw, 33, 1);
        unsigned x4  = (unsigned)bits(raw, 29, 4);
        unsigned x2b = (unsigned)bits(raw, 27, 2);
        int32_t imm8 = (int32_t)sext((bits(raw, 36, 1) << 7) |
                                     bits(raw, 13, 7), 8);
        static const uint8_t logic[4] = { FOP_AND, FOP_ANDCM, FOP_OR, FOP_XOR };
        static const uint8_t logici[4] = { FOP_ANDI, FOP_ANDCMI, FOP_ORI, FOP_XORI };
        if (x2a == 2) {                                  /* adds imm14 */
            tb->op[i] = FOP_ADDI;
            tb->imm[i] = (int32_t)sext((bits(raw, 36, 1) << 13) |
                                       (bits(raw, 27, 6) << 7) |
                                       bits(raw, 13, 7), 14);
        } else if (x2a == 0 && !ve) {
            switch (x4) {
            case 0: tb->op[i] = FOP_ADD; tb->imm[i] = x2b == 1; break;
            case 1: tb->op[i] = FOP_SUB; tb->imm[i] = x2b == 0; break;
            case 3: tb->op[i] = logic[x2b]; break;
            case 4: tb->op[i] = FOP_SHLADD; tb->imm[i] = (int32_t)x2b + 1; break;
            case 9:
                if (x2b == 1) { tb->op[i] = FOP_SUBI; tb->imm[i] = imm8; }
                break;
            case 0xB: tb->op[i] = logici[x2b]; tb->imm[i] = imm8; break;
            }
        }
        return;
    }
    if (a_unit && major == 9) {              /* addl imm22, r3 in 0..3 */
        tb->op[i] = FOP_ADDI;
        tb->a3[i] = (uint8_t)bits(raw, 20, 2);
        tb->imm[i] = (int32_t)sext((bits(raw, 36, 1) << 21) |
                                   (bits(raw, 22, 5) << 16) |
                                   (bits(raw, 27, 9) << 7) |
                                   bits(raw, 13, 7), 22);
        return;
    }
    if (a_unit && major >= 0xC && major <= 0xE) {
        /* A6/A8 normal and .unc compares (not the parallel A7 forms or
         * ta's .and/.or types). */
        unsigned tbit = (unsigned)bits(raw, 36, 1);
        unsigned x2 = (unsigned)bits(raw, 34, 2);
        unsigned ta = (unsigned)bits(raw, 33, 1);
        bool imm_form = x2 >= 2;
        if (ta || (!imm_form && tbit))
            return;
        tb->op[i] = FOP_CMP;
        tb->a1[i] = (uint8_t)bits(raw, 6, 6);            /* p1 */
        tb->a2[i] = (uint8_t)bits(raw, 27, 6);           /* p2 */
        tb->x[i] = (uint8_t)((major == 0xC ? CMPF_LT :
                              major == 0xD ? CMPF_LTU : CMPF_EQ) |
                             (x2 == 1 || x2 == 3 ? CMPF_CMP4 : 0) |
                             (imm_form ? CMPF_IMM : 0) |
                             (bits(raw, 12, 1) ? CMPF_UNC : 0));
        tb->imm[i] = imm_form ?
            (int32_t)sext((bits(raw, 36, 1) << 7) | bits(raw, 13, 7), 8) :
            (int32_t)bits(raw, 13, 7);                   /* r2 */
        return;
    }
    if (u == 'M' && (major == 4 || major == 5)) {     /* exec_mem() */
        unsigned x6 = (unsigned)bits(raw, 30, 6);
        unsigned mbit = (unsigned)bits(raw, 36, 1);
        unsigned xbit = (unsigned)bits(raw, 27, 1);
        if (major == 4 && xbit) {
            if (!mbit && x6 == 0x1C)
                tb->op[i] = FOP_GETF_SIG;
            return;
        }
        /* plain, .bias and .acq loads; plain and .rel stores */
        bool load = x6 <= 0x03 || (x6 >= 0x10 && x6 <= 0x17);
        bool store = x6 >= 0x30 && x6 <= 0x37;
        if (!load && !store)
            return;
        tb->op[i] = load ? FOP_LD : FOP_ST;
        tb->x[i] = (uint8_t)(1u << (x6 & 3));
        if (major == 5) {
            uint64_t lo7 = store ? bits(raw, 6, 7) : bits(raw, 13, 7);
            tb->x[i] |= LDST_INC_IMM;
            tb->imm[i] = (int32_t)sext((bits(raw, 36, 1) << 8) |
                                       (bits(raw, 27, 1) << 7) | lo7, 9);
        } else if (mbit) {
            tb->x[i] |= LDST_INC_REG;
        }
        return;
    }
    if (u == 'M' && major == 6 && bits(raw, 27, 1) && !bits(raw, 36, 1) &&
        bits(raw, 30, 6) == 0x1C) {
        tb->op[i] = FOP_SETF_SIG;
        return;
    }
    if (u == 'I' && major == 0 && bits(raw, 33, 3) == 0) {
        unsigned x6 = (unsigned)bits(raw, 27, 6);
        if ((x6 >= 0x10 && x6 <= 0x12) || (x6 >= 0x14 && x6 <= 0x16)) {
            tb->op[i] = FOP_EXT;
            tb->x[i] = (uint8_t)x6;
        }
        return;
    }
    if (u == 'B' && major == 4) {
        unsigned btype = (unsigned)bits(raw, 6, 3);
        if (btype != 0 && btype != 5)
            return;
        tb->op[i] = btype == 0 ? FOP_BR_COND : FOP_BR_CLOOP;
        tb->imm[i] = (int32_t)(sext((bits(raw, 36, 1) << 20) |
                                    bits(raw, 13, 20), 21) * 16);
        return;
    }
}

/* Fill a TB entry's decoded fields from its bundle bits.  slow_slot0 marks
 * the bundle-only half of each slot-0 special case in merced_step() (the
 * RAM-clear, empty-cloop and dead-loop idioms, and reserved templates), so
 * merced_run_fast() can leave exactly those to merced_step() (a reserved
 * template from any slot). */
static void tb_decode(MercedTbEntry *tb) {
    const char *units = bundle_units[tb->tmpl];
    const uint64_t *sl = tb->slots;
    tb->slow_slot0 = 0;
    tb->reserved = units[0] == '?';
    if (tb->reserved)
        return;
    for (unsigned i = 0; i < 3; i++) {
        char u = units[i] == 'L' ? 'X' : units[i];
        uint64_t raw = u == 'X' ? sl[2] : sl[i];
        tb->qpr[i] = (uint8_t)bits(raw, 0, 6);
        tb->stop[i] = bundle_stop_after[tb->tmpl][u == 'X' ? 2 : i];
        tb->fn[i] = NULL;
        tb->op[i] = FOP_GENERIC;
        if (u == 'X') {
            tb->kind[i] = SLOT_X;
            continue;
        }
        if (slot_is_nop(u, raw)) {
            tb->kind[i] = SLOT_NOP;
            continue;
        }
        tb->kind[i] = SLOT_EXEC;
        tb_decode_op(tb, i, u, raw);
        unsigned major = (unsigned)bits(raw, 37, 4);
        switch (u) {
        case 'M':
            /* exec_m()'s own routing, where it's a plain hand-off */
            if (major <= 1) tb->fn[i] = exec_m_sys;
            else if (major >= 4 && major <= 7) tb->fn[i] = exec_mem;
            else tb->fn[i] = exec_m;
            break;
        case 'I': tb->fn[i] = exec_i; break;
        case 'B': tb->fn[i] = exec_b; break;
        default:  tb->fn[i] = exec_f; break;
        }
    }
    bool fill = units[0] == 'M' && units[1] == 'M' && units[2] == 'B' &&
        bits(sl[0], 37, 4) == 5 && bits(sl[1], 37, 4) == 5 &&
        bits(sl[0], 30, 6) == 0x33 && bits(sl[1], 30, 6) == 0x33 &&
        bits(sl[0], 0, 6) == 0 && bits(sl[1], 0, 6) == 0 &&
        bits(sl[0], 13, 7) == 0 && bits(sl[1], 13, 7) == 0 &&
        bits(sl[0], 6, 7) == 16 && bits(sl[1], 6, 7) == 16 &&
        bits(sl[2], 37, 4) == 4 && bits(sl[2], 6, 3) == 5 &&
        bits(sl[2], 0, 6) == 0;
    bool empty_loop = units[0] == 'M' && units[1] == 'I' && units[2] == 'B' &&
        sl[0] == 0x00008000000ull && sl[1] == 0x00008000000ull &&
        bits(sl[2], 37, 4) == 4 && bits(sl[2], 0, 6) == 0 &&
        (bits(sl[2], 6, 3) == 5 || bits(sl[2], 6, 3) == 0);
    tb->slow_slot0 = fill || empty_loop;
}

/* Whether the cache-array model sees instruction fetches: it observes every
 * access, or it is following I-cache fills. */
static inline bool fetch_observed(const Merced *m) {
    return m->cache.observe || m->cache.ic_track;
}

/* Whether a bundle must be fetched afresh rather than from the bundle caches
 * on entry at slot 0: while the model sees fetches (a hit there would not
 * reach it), and once it stops, a bundle it served (memory's differs). */
static inline bool fetch_uncached(const Merced *m) {
    return fetch_observed(m) || m->cache.served;
}

typedef enum { FETCH_MEMORY, FETCH_SERVED, FETCH_MCA } ModelFetch;

/* The fetch of the bundle at bundle_va / pa through the cache-array model:
 * lines PAL planted in the I-cache arrays are what a cacheable fetch
 * executes, whatever memory holds, and the unified L2 and the L3 can hold
 * code a store left there.  *lo and *hi are memory's bundle on entry and the
 * model's on FETCH_SERVED; FETCH_MCA means an array error was delivered as a
 * machine check. */
static ModelFetch model_fetch(Merced *m, uint64_t bundle_va, uint64_t pa,
                              uint64_t *lo, uint64_t *hi) {
    merced_cache_tick(m);               /* fill windows close on time too */
    bool wb = (bundle_va >> 63) == 0;
    if (m->psr & PSR_IT) {
        const MercedTlbEntry *e = itlb_entry(m, bundle_va);
        wb = !e || ((e->pte >> PTE_MA_SHIFT) & 7) == 0;
    }
    const MercedIcResult icr = merced_icache_fetch(m, model_pa(m, pa), wb, lo, hi);
    if (icr == MERCED_IC_MCA && merced_cache_take_mca(m)) {
        deliver_mca(m);
        return FETCH_MCA;
    }
    if (icr == MERCED_IC_HIT)
        return FETCH_SERVED;
    if (wb && m->cache.observe && merced_cache_fetch(m, model_pa(m, pa), lo, hi))
        return FETCH_SERVED;
    return FETCH_MEMORY;
}

/* Whether a B-unit slot is a branch for the taken-branch and lower-privilege
 * traps: br.cond/ia/ret and br.call, not rfi, epc, cover and the like. */
static bool slot_is_branch(uint64_t raw) {
    unsigned major = (unsigned)bits(raw, 37, 4);
    if (major == 0) {
        unsigned x6 = (unsigned)bits(raw, 27, 6);
        return x6 == 0x20 || x6 == 0x21;
    }
    return major == 1 || major == 4 || major == 5;
}

/* The traps an instruction that has just completed without faulting can
 * raise (SDM vol. 2, 5.8.3): Lower-Privilege Transfer when a branch lowers the
 * privilege level with PSR.lp set, Taken Branch with PSR.tb, and Single Step
 * with PSR.ss, in that priority order.  A trap reports where execution
 * resumes, so IP moves on to the next instruction first; ISR.ei names the
 * instruction that completed.  Returns true when a trap was delivered. */
static bool post_slot_trap(Merced *m, char unit, uint64_t raw, unsigned slot,
                           uint64_t psr_before, uint64_t bundle_va,
                           unsigned slot_stop_after) {
    bool branch = unit == 'B' && m->taken && slot_is_branch(raw);
    unsigned cpl_before = (unsigned)((psr_before >> PSR_CPL_SHIFT) & 3);
    unsigned cpl_now = (unsigned)((m->psr >> PSR_CPL_SHIFT) & 3);
    uint32_t vec = 0;
    uint64_t code = 0;
    if (branch && (psr_before & PSR_LP) && cpl_now > cpl_before) {
        vec = 0x5E00; code = 0x2;
    } else if (branch && (psr_before & PSR_TB)) {
        vec = 0x5F00; code = 0x4;
    } else if ((psr_before & PSR_SS) &&
               !(unit == 'B' && bits(raw, 37, 4) == 0 &&
                 (bits(raw, 27, 6) == 0x08 || bits(raw, 27, 6) == 0x09))) {
        vec = 0x6000; code = 0x8;
    }
    if (!vec)
        return false;
    if (m->taken) {
        m->taken = 0;
    } else {
        m->group_start = slot_stop_after;
        if (unit == 'X' || slot == 2)
            m->ip = bundle_va + 16;
        else
            m->ip = bundle_va | (slot + 1);
    }
    deliver_fault(m, vec, code, 0, false);
    m->cr[CR_ISR] = (m->cr[CR_ISR] & ~(UINT64_C(3) << 41)) |
                    ((uint64_t)slot << 41);
    return true;
}

/* The floating-point register partitions an instruction names: bit 0 for
 * f2-f31, bit 1 for f32-f127 (f0 and f1 are always accessible).  Only
 * encodings whose register fields are certain are classified. */
static unsigned fp_bank(unsigned f) { return f >= 32 ? 2u : f >= 2 ? 1u : 0u; }

static unsigned fp_banks_used(char unit, uint64_t raw) {
    unsigned major = (unsigned)bits(raw, 37, 4);
    unsigned f1 = (unsigned)bits(raw, 6, 7), f2 = (unsigned)bits(raw, 13, 7);
    unsigned f3 = (unsigned)bits(raw, 20, 7), f4 = (unsigned)bits(raw, 27, 7);
    if (unit == 'F') {
        if (major == 0 || major == 1) {
            unsigned x = (unsigned)bits(raw, 33, 1);
            unsigned x6 = (unsigned)bits(raw, 27, 6);
            if (!x && (x6 == 0x00 || x6 == 0x01 || x6 == 0x04 ||
                       x6 == 0x05 || x6 == 0x08))
                return 0;
            if (x6 >= 0x18 && x6 <= 0x1C && (x || x6 != 0x1C || !x))
                return fp_bank(f1) | fp_bank(f2);
            return fp_bank(f1) | fp_bank(f2) | fp_bank(f3);
        }
        if (major == 4) return fp_bank(f2) | fp_bank(f3);     /* fcmp */
        if (major == 5) return fp_bank(f2);                   /* fclass */
        if (major >= 8)
            return fp_bank(f1) | fp_bank(f2) | fp_bank(f3) | fp_bank(f4);
        return 0;
    }
    if (unit == 'M') {
        unsigned x6 = (unsigned)bits(raw, 30, 6), xbit = (unsigned)bits(raw, 27, 1);
        if (major == 4 && xbit && !bits(raw, 36, 1) && x6 >= 0x1C && x6 <= 0x1F)
            return fp_bank(f2);                               /* getf */
        if (major == 6 || major == 7) {
            if (x6 >= 0x30 && x6 <= 0x3B)                     /* stf */
                return fp_bank(f2);
            if (major == 6 && xbit && x6 >= 0x1C && x6 <= 0x1F)
                return fp_bank(f1);                           /* setf */
            if (x6 <= 0x0F || x6 == 0x1B || (x6 >= 0x20 && x6 <= 0x27))
                return fp_bank(f1) | (xbit && major == 6 ? fp_bank(f2) : 0);
        }
    }
    return 0;
}

/* ISR.code of a Disabled Floating-point Register fault the instruction takes
 * under the current PSR.dfl/dfh, or 0. */
static unsigned fp_disabled_code(const Merced *m, char unit, uint64_t raw) {
    unsigned banks = fp_banks_used(unit, raw), code = 0;
    if ((banks & 1u) && (m->psr & PSR_DFL)) code |= 1u;
    if ((banks & 2u) && (m->psr & PSR_DFH)) code |= 2u;
    return code;
}

/* PSR.id, da, dd, ia and ed each suppress a fault for one instruction: they
 * clear once an instruction other than rfi, which installs them, completes. */
#define PSR_ONE_SHOT (PSR_DA | PSR_DD | PSR_IA | PSR_ED | PSR_ID)

static inline bool slot_is_rfi(char unit, uint64_t raw) {
    return unit == 'B' && bits(raw, 37, 4) == 0 &&
           (bits(raw, 27, 6) == 0x08 || bits(raw, 27, 6) == 0x09);
}

MercedStatus merced_step(Merced *m) {
    if (m->translation_cache)
        ((MercedTranslationCache *)m->translation_cache)->cur_tb = NULL;
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
     * surrounding native firmware, but PSR.is does.  Older i2000 snapshots
     * also contain EFI applications in the synthetic high-DRAM aperture
     * (00000eXX...), before the application's IVT is installed. */
    bool ami_ivt_handoff = ami_ivt_handoff_pending(m);

    /* A PMI outranks every external interrupt and ignores PSR.i. */
    if (m->group_start && pmi_deliverable(m)) {
        deliver_pmi(m);
        m->ninsts++;
        return MERCED_OK;
    }

    if (m->psr & PSR_IS) {
        /* IA-32 instructions are interruption points too.  This check must
         * precede the dispatch to merced_ia32_step(): otherwise a pending
         * PIT/ExtINT remains invisible for the entire IA-32 run and is first
         * recognized only after an instruction intercept has already moved
         * execution into the native IVT.  The SDV BIOS then saves a native
         * intercept frame as though it were the interrupted x86 frame and
         * eventually IRETs into its F3:F4/data sentinel at F000:71A8. */
        /* While IA-32 instructions execute, external interrupts are enabled
         * when PSR.i and (!CFLG.if or EFLAG.if) (SDM Vol.2, IA-32 interrupt
         * masking): EFLAG.if only matters once the firmware has set CFLG.if.
         * AMI's IA-32 BIOS issues an IDE command under cli and then polls the
         * 8259's IRR for IRQ14/15; delivering the interrupt inside that
         * window would let EFI acknowledge it first. */
        bool ia32_if = !(m->ar[27] & (UINT64_C(1) << 7)) ||
                       (m->ar[24] & (UINT64_C(1) << 9));
        if (!ami_ivt_handoff && ia32_if && (m->psr & PSR_I) &&
            (m->psr & PSR_IC) && ext_highest_unmasked(m) >= 0) {
            MercedStatus ist = deliver_fault(m, VEC_EXTINT, 0, 0, false);
            m->ninsts++;
            return ist;
        }
        if (!ami_ivt_handoff && ia32_if && m->timer_pending &&
            !(m->cr[CR_ITV] & (1ull << 16)) &&
            interrupt_unmasked(m, (uint8_t)m->cr[CR_ITV]) &&
            (m->psr & PSR_I) && (m->psr & PSR_IC)) {
            MercedStatus ist = deliver_fault(m, VEC_EXTINT, 0, 0, false);
            m->ninsts++;
            return ist;
        }
        MercedStatus ia32_status = merced_ia32_step(m);
        if (!m->external_itc)
            itc_advance(m, m->slot_cycles);
        return ia32_status;
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
        (m->psr & PSR_I) && (m->psr & PSR_IC) &&
        ext_highest_unmasked(m) >= 0) {
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
    if (__builtin_expect(m->psr & PSR_DB, 0) &&
        !(m->psr & PSR_IS) && inst_debug_fault(m, bundle_va, &st)) {
        m->ninsts++;
        return st;
    }
    const bool uncached = fetch_uncached(m);
    const bool refetch = slot == 0 && uncached;
    bool served = false;
    uint64_t lo, hi;
    unsigned tmpl;
    uint64_t slots[3];
    MercedTranslationCache *tc = translation_cache_on
                               ? m->translation_cache : NULL;
    MercedTbEntry *tb = tc ? &tc->entry[tb_index(bundle_va)] : NULL;
    /* Slots 1/2 normally reuse the bundle just consumed by slot 0.  Test
     * this compact CPU-local cache before hashing into the much larger TB
     * table; it keeps the common two-of-three accesses in L1. */
    if (!refetch && m->bundle_cache_valid && m->bundle_cache_translation_valid &&
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
    } else if (!uncached && tb && tb->valid && tb->va == bundle_va &&
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
        m->cache.served = false;
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
        if (!refetch && m->bundle_cache_valid && m->bundle_cache_va == bundle_va &&
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
            if (fetch_observed(m)) {
                ModelFetch mf = model_fetch(m, bundle_va, pa, &lo, &hi);
                if (mf == FETCH_MCA) {
                    m->ninsts++;
                    m->group_start = 1;
                    return MERCED_OK;
                }
                served = mf == FETCH_SERVED;
            }
            m->cache.served = served;
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
        /* Not memory's bundle: an entry would outlive the line. */
        if (tb && !served) {
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
            tb_decode(tb);
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
     * machine memory in one operation.  With data translation enabled,
     * consecutive virtual pages can have unrelated physical mappings and
     * each store can fault.  Leave those loops to normal execution rather
     * than translating just the first address for a physical bulk fill. */
    if (slot == 0 && !(m->psr & PSR_DT) && m->bus.fill && !m->cache.observe &&
        units[0] == 'M' && units[1] == 'M' &&
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
                itc_advance(m, iterations * 3 * m->slot_cycles);
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
                itc_advance(m, executed * m->slot_cycles);
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
    uint64_t psr_before = m->psr;

    /* Keep the many one-off firmware probes entirely off the normal
     * execution path.  Individually-predictable false branches still add
     * up when this function is called billions of times. */
    if (debug_hooks_on || m->trace_n) {

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





    /* Production i2000 firmware CDB/FMM error propagation path.  These
     * addresses are deliberately only diagnostics: unlike the older SDV
     * image, fwver130 does not use the validator entry points above. */






    }

    unsigned fp_off = (qp && __builtin_expect(m->psr & (PSR_DFL | PSR_DFH), 0))
                    ? fp_disabled_code(m, unit, raw) : 0;
    if (fp_off) {
        st = deliver_fault(m, VEC_DISABLED_FP, fp_off, 0, false);
    } else if (slot_is_nop(unit, raw)) {
        st = MERCED_OK;
    } else {
        switch (unit) {
        case 'M': st = exec_m(m, raw, qp); break;
        case 'I': st = exec_i(m, raw, qp); break;
        case 'B': st = exec_b(m, raw, qp); break;
        case 'F': st = exec_f(m, raw, qp); break;
        case 'X': st = exec_x(m, raw, lraw, qp); break;
        default:  st = mhalt(m, "internal: unit %c", unit); break;
        }
    }
    if (__builtin_expect(oof_write_pending, 0)) {
        oof_write_pending = false;
        st = deliver_fault(m, VEC_GENERAL, 0, 0, false);
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

    /* A cache-array error during this instruction's access (merced_cache.c):
     * the instruction is abandoned and PALE_CHECK entered with IIP on it. */
    if (__builtin_expect(m->cache.observe, 0) && merced_cache_take_mca(m))
        deliver_mca(m);

    m->ninsts++;
    if (__builtin_expect(pmu_count_on, 0) && m->nfaults == faults_before)
        pmu_retire(m, slot_is_nop(unit, raw), qp, m->slot_stop_after);
    if (!m->external_itc)
        itc_advance(m, m->slot_cycles);
    if (m->nfaults == faults_before)
        m->last_successful_bundle = bundle_va;
    if (__builtin_expect(m->psr & PSR_ONE_SHOT, 0) &&
        m->nfaults == faults_before && !slot_is_rfi(unit, raw))
        m->psr &= ~PSR_ONE_SHOT;

    if (__builtin_expect(psr_before & (PSR_LP | PSR_TB | PSR_SS), 0) &&
        m->nfaults == faults_before &&
        post_slot_trap(m, unit, raw, slot, psr_before, bundle_va,
                       m->slot_stop_after)) {
        m->group_start = 1;
        m->taken = 0;
        return MERCED_OK;
    }

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

/* Stop the current merced_run_fast() after the slot now executing - for
 * platform events (a reset request from a bus write) that the caller must
 * handle before the next instruction, as it would between merced_step()
 * calls. */
void merced_request_exit(Merced *m) {
    MercedTranslationCache *tc = m->translation_cache;
    if (tc) tc->exit_request = true;
}

/* va_translate(m, va, false, false, access, ...) for a data reference,
 * answered inline when data translation is off or the soft TLB hits and
 * none of va_translate()'s watches can fire; anything else goes to
 * va_translate() itself. */
static inline bool fast_dtranslate(Merced *m, uint64_t va, uint64_t access,
                                   uint64_t *pa, MercedStatus *st) {
    if (!(mmfault_armed && mmfault_hi) &&
        !(va >= watch_va_base && va < watch_va_end) &&
        !m->cache.observe) {            /* the cache-array model needs va_translate() */
        if (!(m->psr & PSR_DT)) {
            *st = MERCED_OK;
            *pa = va & MERCED_PHYS_MASK;
            return true;
        }
        if (xlate_cacheable(m, va) &&
            xlate_lookup(m, va, xlate_kind(m, false, access), pa)) {
            *st = MERCED_OK;
            return true;
        }
    }
    return va_translate(m, va, false, false, access, pa, st);
}

/* The specialized handlers tb_decode_op() chose: each is the generic
 * handler's own path for that encoding, minus the decoding.  Operand reads
 * the generic code makes and then ignores are left out, which is exact
 * because gr_read() has no side effect while fast_step() runs (its one
 * debug print, R29_DEBUG, keeps merced_run_fast() off fast_step()). */
static inline MercedStatus fast_op(Merced *m, const MercedTbEntry *tb,
                                   unsigned s, int qp) {
    unsigned r1 = tb->a1[s], r2 = tb->a2[s], r3 = tb->a3[s];
    unsigned x = tb->x[s];
    int64_t imm = tb->imm[s];
    uint8_t n2, n3;
    uint64_t a, b, res, pa;
    MercedStatus st;

    switch (tb->op[s]) {
    case FOP_ADD: case FOP_SUB: case FOP_AND: case FOP_ANDCM:
    case FOP_OR: case FOP_XOR: case FOP_SHLADD:
        if (!qp) return MERCED_OK;
        b = gr_read_inl(m, r3, &n3);
        a = gr_read_inl(m, r2, &n2);
        switch (tb->op[s]) {
        case FOP_ADD:   res = a + b + (uint64_t)imm; break;
        case FOP_SUB:   res = a - b - (uint64_t)imm; break;
        case FOP_AND:   res = a & b; break;
        case FOP_ANDCM: res = a & ~b; break;
        case FOP_OR:    res = a | b; break;
        case FOP_XOR:   res = a ^ b; break;
        default:        res = (a << imm) + b; break;
        }
        gr_write_inl(m, r1, res, n2 | n3);
        return MERCED_OK;

    case FOP_ADDI: case FOP_SUBI: case FOP_ANDI: case FOP_ANDCMI:
    case FOP_ORI: case FOP_XORI:
        if (!qp) return MERCED_OK;
        b = gr_read_inl(m, r3, &n3);
        switch (tb->op[s]) {
        case FOP_ADDI:   res = (uint64_t)imm + b; break;
        case FOP_SUBI:   res = (uint64_t)imm - b; break;
        case FOP_ANDI:   res = (uint64_t)imm & b; break;
        case FOP_ANDCMI: res = (uint64_t)imm & ~b; break;
        case FOP_ORI:    res = (uint64_t)imm | b; break;
        default:         res = (uint64_t)imm ^ b; break;
        }
        gr_write_inl(m, r1, res, n3);
        return MERCED_OK;

    case FOP_CMP: {
        /* r1/r2 hold p1/p2; imm the immediate or the r2 register */
        b = gr_read_inl(m, r3, &n3);
        uint8_t nat = n3;
        int64_t sa;
        if (x & CMPF_IMM) {
            sa = imm;
            a = (uint64_t)sa;
        } else {
            a = gr_read_inl(m, (unsigned)imm, &n2);
            sa = (int64_t)a;
            nat |= n2;
        }
        int64_t sb = (int64_t)b;
        if (x & CMPF_CMP4) {
            sa = (int32_t)sa; sb = (int32_t)sb;
            a = (uint32_t)a;  b = (uint32_t)b;
        }
        int rel;
        switch (x & CMPF_REL) {
        case CMPF_LT:  rel = sa < sb; break;
        case CMPF_LTU: rel = a < b; break;
        default:       rel = a == b; break;
        }
        if (nat && qp) {
            pr_write(m, r1, 0);
            pr_write(m, r2, 0);
            return MERCED_OK;
        }
        set_preds(m, r1, r2, qp, rel, (x & CMPF_UNC) ? 1 : 0);
        return MERCED_OK;
    }

    case FOP_LD: {
        if (!qp) return MERCED_OK;
        uint64_t va = gr_read_inl(m, r3, &n3);
        uint64_t reg_inc = 0;
        uint8_t reg_inc_nat = 0;
        if (x & LDST_INC_REG)
            reg_inc = gr_read_inl(m, r2, &reg_inc_nat);
        if (n3)
            return deliver_fault(m, VEC_NAT, ISR_NA | ISR_R | UINT64_C(0x10),
                                 0, false);
        /* These are non-speculative loads.  Translation failure transfers
         * control to the fault handler without committing either output. */
        if (!fast_dtranslate(m, va, ISR_R, &pa, &st)) return st;
        gr_write_inl(m, r1, phys_read(m, pa, x & LDST_SIZE), 0);
        if (x & (LDST_INC_IMM | LDST_INC_REG)) {
            uint64_t base = gr_read_inl(m, r3, &n3);
            if (x & LDST_INC_IMM)
                gr_write_inl(m, r3, base + (uint64_t)imm, n3);
            else
                gr_write_inl(m, r3, base + reg_inc, n3 | reg_inc_nat);
        }
        return MERCED_OK;
    }

    case FOP_ST: {
        /* r1 is the postincrement register, r2 the value */
        if (!qp) return MERCED_OK;
        uint64_t va = gr_read_inl(m, r3, &n3);
        uint64_t reg_inc = 0;
        uint8_t reg_inc_nat = 0;
        if (x & LDST_INC_REG)
            reg_inc = gr_read_inl(m, r1, &reg_inc_nat);
        if (n3)
            return deliver_fault(m, VEC_NAT, ISR_NA | ISR_W | UINT64_C(0x10),
                                 0, false);
        uint64_t v = gr_read_inl(m, r2, &n2);
        if (n2)
            return deliver_fault(m, VEC_NAT, ISR_W | UINT64_C(0x10), 0, false);
        if (!fast_dtranslate(m, va, ISR_W, &pa, &st)) return st;
        phys_write(m, pa, v, x & LDST_SIZE);
        if (x & LDST_INC_IMM)
            gr_write_inl(m, r3, va + (uint64_t)imm, n3);
        else if (x & LDST_INC_REG)
            gr_write_inl(m, r3, va + reg_inc, n3 | reg_inc_nat);
        return MERCED_OK;
    }

    case FOP_SETF_SIG: {
        if (!qp) return MERCED_OK;
        MercedFpReg f = {0, 0, 0, 0};
        f.sig = gr_read_inl(m, r2, &n2);
        f.exp = 0x1003E;
        f.nat = n2;
        fr_write(m, r1, f);
        return MERCED_OK;
    }

    case FOP_GETF_SIG: {
        if (!qp) return MERCED_OK;
        MercedFpReg f = fr_read(m, r2);
        gr_write_inl(m, r1, f.sig, f.nat);
        return MERCED_OK;
    }

    case FOP_EXT:
        if (!qp) return MERCED_OK;
        a = gr_read_inl(m, r3, &n3);
        switch (x) {
        case 0x10: a = (uint8_t)a; break;
        case 0x11: a = (uint16_t)a; break;
        case 0x12: a = (uint32_t)a; break;
        case 0x14: a = (uint64_t)(int64_t)(int8_t)a; break;
        case 0x15: a = (uint64_t)(int64_t)(int16_t)a; break;
        default:   a = (uint64_t)(int64_t)(int32_t)a; break;
        }
        gr_write_inl(m, r1, a, n3);
        return MERCED_OK;

    case FOP_BR_COND:
        if (qp) {
            m->ip = (m->ip & ~UINT64_C(0xF)) + (uint64_t)imm;
            m->taken = 1;
        }
        return MERCED_OK;

    case FOP_BR_CLOOP:
        /* exec_b()'s br.cloop doesn't consult qp either */
        if (m->ar[AR_LC] != 0) {
            m->ar[AR_LC]--;
            m->ip = (m->ip & ~UINT64_C(0xF)) + (uint64_t)imm;
            m->taken = 1;
        }
        return MERCED_OK;
    }
    return tb->fn[s](m, tb->slots[s], qp);
}

/* The common case of merced_step(), many slots per call.  It handles a slot
 * itself only when none of merced_step()'s special cases can apply (debug
 * hooks, IA-32, the firmware entry-point shims, an undecoded or reserved
 * bundle, the slot-0 loop idioms); otherwise it hands that slot to
 * merced_step() before touching any state.  What it does do - interrupt
 * recognition, the fetch-cache bookkeeping, dispatch and the end-of-slot
 * accounting - is merced_step()'s code in the same order, so the two run
 * the guest identically. */
static inline MercedStatus fast_step(Merced *m, MercedTranslationCache *tc) {
    uint64_t ip = m->ip;
    if ((m->psr & (PSR_IS | PSR_DB)) ||
        (ip & UINT64_C(0x1FFFFFFFFFFFFFF0)) == UINT64_C(0x00000000FFF05000))
        return merced_step(m);
    uint64_t bundle_va = ip & ~0xFull;
    unsigned slot = (unsigned)(ip & 0xF);
    if (slot > 2) return merced_step(m);
    bool from_bundle_cache = m->bundle_cache_valid &&
                             m->bundle_cache_translation_valid &&
                             m->bundle_cache_va == bundle_va;
    /* Every fetch the cache-array model sees goes through merced_step(). */
    if (__builtin_expect(fetch_uncached(m), 0) &&
        (slot == 0 || !from_bundle_cache))
        return merced_step(m);
    MercedTbEntry *tb;
    if (from_bundle_cache) {
        tb = tc->cur_tb;            /* checked on an earlier slot? */
        if (!tb || tb->va != bundle_va) {
            /* The decode is a pure function of the bundle bits, so any
             * entry holding the same bits will do. */
            tb = &tc->entry[tb_index(bundle_va)];
            if (!(tb->valid && tb->va == bundle_va &&
                  tb->lo == m->bundle_cache_lo && tb->hi == m->bundle_cache_hi))
                return merced_step(m);
            tc->cur_tb = tb;
        }
    } else {
        tb = &tc->entry[tb_index(bundle_va)];
        if (!(tb->valid && tb->va == bundle_va &&
              tb->translation_generation == tc->translation_generation &&
              tb->code_generation == tc->page_generation[tb_page_index(tb->pa)]))
            return merced_step(m);
    }
    if (tb->reserved || (slot == 0 && tb->slow_slot0)) return merced_step(m);

    /* -- from here on, merced_step()'s own sequence -- */
    /* Nothing can be delivered unless a PMI, an external interrupt or the
     * timer is pending, so the checks below (and the firmware-handoff test
     * that gates them) are skipped on the common slot. */
    if (m->group_start &&
        (m->ext_pending_count || m->timer_pending ||
         (m->msr[MSR_PMI_LATCH] & PMI_VECTORS))) {
        bool ami_ivt_handoff = ami_ivt_handoff_pending(m);
        if (pmi_deliverable(m)) {
            deliver_pmi(m);
            m->ninsts++;
            return MERCED_OK;
        }
        if (!ami_ivt_handoff && (m->psr & PSR_I) && (m->psr & PSR_IC) &&
            ext_highest_unmasked(m) >= 0) {
            MercedStatus ist = deliver_fault(m, VEC_EXTINT, 0, 0, false);
            m->ninsts++;
            return ist;
        }
        if (!ami_ivt_handoff &&
            m->timer_pending && !(m->cr[CR_ITV] & (1ull << 16)) &&
            interrupt_unmasked(m, (uint8_t)m->cr[CR_ITV]) &&
            (m->psr & PSR_I) && (m->psr & PSR_IC)) {
            MercedStatus ist = deliver_fault(m, VEC_EXTINT, 0, 0, false);
            m->ninsts++;
            return ist;
        }
    }

    if (!from_bundle_cache) {
        m->bundle_cache_valid = true;
        m->bundle_cache_translation_valid = true;
        m->bundle_cache_va = bundle_va;
        m->bundle_cache_pa = tb->pa;
        m->bundle_cache_lo = tb->lo;
        m->bundle_cache_hi = tb->hi;
        m->bundle_cache_tmpl = tb->tmpl;
        m->bundle_cache_slots[0] = tb->slots[0];
        m->bundle_cache_slots[1] = tb->slots[1];
        m->bundle_cache_slots[2] = tb->slots[2];
        tc->cur_tb = tb;
    }
    unsigned kind = tb->kind[slot];
    m->slot_stop_after = tb->stop[slot];
    int qp = pr_read(m, tb->qpr[slot]);
    m->taken = 0;
    uint64_t faults_before = m->nfaults;
    uint64_t psr_before = m->psr;

    MercedStatus st;
    unsigned fp_off = (qp && kind == SLOT_EXEC &&
                       __builtin_expect(m->psr & (PSR_DFL | PSR_DFH), 0))
                    ? fp_disabled_code(m, bundle_units[tb->tmpl][slot],
                                       tb->slots[slot]) : 0;
    if (fp_off)
        st = deliver_fault(m, VEC_DISABLED_FP, fp_off, 0, false);
    else if (kind == SLOT_EXEC)
        st = tb->op[slot] == FOP_GENERIC ?
             tb->fn[slot](m, tb->slots[slot], qp) : fast_op(m, tb, slot, qp);
    else if (kind == SLOT_NOP)
        st = MERCED_OK;
    else
        st = exec_x(m, tb->slots[2], tb->slots[1], qp);
    if (__builtin_expect(oof_write_pending, 0)) {
        oof_write_pending = false;
        st = deliver_fault(m, VEC_GENERAL, 0, 0, false);
    }
    if (st != MERCED_OK) {
        if (st == MERCED_HALT_UNIMPL) {
            char unit = kind == SLOT_X ? 'X' : bundle_units[tb->tmpl][slot];
            uint64_t raw = kind == SLOT_X ? tb->slots[2] : tb->slots[slot];
            size_t len = strlen(m->halt_msg);
            snprintf(m->halt_msg + len, sizeof(m->halt_msg) - len,
                     " [%c-slot %u tmpl %s raw=0x%011" PRIX64 "]",
                     unit, slot, bundle_units[tb->tmpl], raw);
        }
        return st;
    }

    if (__builtin_expect(m->cache.observe, 0) && merced_cache_take_mca(m))
        deliver_mca(m);

    m->ninsts++;
    if (__builtin_expect(pmu_count_on, 0) && m->nfaults == faults_before)
        pmu_retire(m, kind == SLOT_NOP, qp, m->slot_stop_after);
    if (!m->external_itc)
        itc_advance(m, m->slot_cycles);
    if (m->nfaults == faults_before)
        m->last_successful_bundle = bundle_va;
    if (__builtin_expect(m->psr & PSR_ONE_SHOT, 0) &&
        m->nfaults == faults_before &&
        !slot_is_rfi(kind == SLOT_X ? 'X' : bundle_units[tb->tmpl][slot],
                     kind == SLOT_X ? tb->slots[2] : tb->slots[slot]))
        m->psr &= ~PSR_ONE_SHOT;
    if (__builtin_expect(psr_before & (PSR_LP | PSR_TB | PSR_SS), 0) &&
        m->nfaults == faults_before &&
        post_slot_trap(m, kind == SLOT_X ? 'X' : bundle_units[tb->tmpl][slot],
                       kind == SLOT_X ? tb->slots[2] : tb->slots[slot], slot,
                       psr_before, bundle_va, m->slot_stop_after)) {
        m->group_start = 1;
        m->taken = 0;
        return MERCED_OK;
    }
    if (m->taken) {
        m->group_start = 1;
        m->taken = 0;
        return MERCED_OK;
    }
    m->group_start = m->slot_stop_after;
    if (kind == SLOT_X || slot == 2)
        m->ip = bundle_va + 16;
    else
        m->ip = bundle_va | (slot + 1);
    return MERCED_OK;
}

#define MERCED_JIT_INCLUDED
#include "merced_jit.c"

/* Like merced_run(), but through fast_step() when no debug facility is
 * active - or through translated code, with -accel jit.  In addition to the
 * masked stop address, an optional range lets the platform observe an EFI
 * application handoff before its first slot. */
MercedStatus merced_run_fast_range(Merced *m, unsigned max_slots,
                                   unsigned *executed, uint64_t stop_mask,
                                   uint64_t stop_value, uint64_t stop_lo,
                                   uint64_t stop_hi) {
    MercedTranslationCache *tc = translation_cache_on ? m->translation_cache : NULL;
    unsigned n = 0;
    MercedStatus st = MERCED_OK;
    bool fast = tc && !debug_hooks_on && !trace_hi && !watch_ip_count &&
                !heartbeat_on;
    if (fast && tc->jit && tc->jit->enabled && !m->trace_n)
        return mj_run(m, tc, max_slots, executed, stop_mask, stop_value,
                      stop_lo, stop_hi);
    if (tc) tc->exit_request = false;
    while (n < max_slots) {
        if (fast && !m->trace_n)
            st = fast_step(m, tc);
        else
            st = merced_step(m);
        n++;
        if (st != MERCED_OK || (tc && tc->exit_request) ||
            (stop_mask && (m->ip & stop_mask) == stop_value) ||
            (m->ip >= stop_lo && m->ip < stop_hi))
            break;
    }
    if (executed)
        *executed = n;
    return st;
}

MercedStatus merced_run_fast(Merced *m, unsigned max_slots, unsigned *executed,
                             uint64_t stop_mask, uint64_t stop_value) {
    return merced_run_fast_range(m, max_slots, executed, stop_mask, stop_value,
                                 0, 0);
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
      "  PR %016" PRIX64 "\n",
      CFM_SOF(m->cfm), CFM_SOL(m->cfm), CFM_SOR(m->cfm),
      CFM_RRB_GR(m->cfm), CFM_RRB_FR(m->cfm), CFM_RRB_PR(m->cfm),
      m->bof, m->bof_total, m->pr);
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
