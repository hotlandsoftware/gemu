/*
 * Merced dynamic binary translator (-accel jit): IA-64 guest code to host
 * code through the host-neutral interface in jit/jit_backend.h.  Nothing in
 * this file knows the host; jit/jit_backend_<host>.c does.
 *
 * This file is #included by merced.c (it needs the interpreter's static
 * internals) and is not compiled on its own.
 *
 * ── Model ────────────────────────────────────────────────────────────────
 * A translated block (MercedJitBlock) starts at any slot IP and covers up
 * to MJ_MAX_BUNDLES consecutive bundles of one 4 KiB page.  Each slot is
 * translated one of three ways:
 *
 *   - nop:     nothing at all;
 *   - native:  inline host code for the common integer ops (A-unit ALU and
 *              compares, zxt/sxt, movl), plain loads and stores through a
 *              host TLB, and IP-relative br.cond/br.cloop;
 *   - helper:  a call to mj_slot(), which runs the slot through fast_slot(),
 *              the interpreter's own per-slot code.
 *
 * Everything the native subset doesn't cover goes through a helper, so any
 * guest code runs translated from the start, and native coverage can grow
 * one instruction at a time.
 *
 * ── Exactness ────────────────────────────────────────────────────────────
 * Translated code must leave the machine exactly as merced_step() would,
 * slot for slot (GEMU_CPI runs compare bit-for-bit against the interpreter).
 * The pieces that make that hold:
 *
 *   - Blocks are specialized on PSR.bn and CFM's rotation fields (sor and
 *     the rrb's), which fixes the physical location of every GR and
 *     predicate the block names.  The key is part of the lookup.
 *   - A block is entered only if it fits the caller's remaining slot
 *     budget, cannot cross the ITM deadline (ITC follows the instruction
 *     count unless the machine drives it), stays out of the caller's stop
 *     range, and no interrupt could be delivered at any group boundary in
 *     it - otherwise the slot goes to fast_step().
 *   - While the cache-array model observes (merced_cache.h) every fetch and
 *     data access must reach it, which translated code's direct RAM access
 *     and fetch-free dispatch bypass: no block is entered, and a helper
 *     slot that starts the model's observation ends the block.
 *   - Native slots never change anything those conditions read.  Helper
 *     slots might, so after each one mj_slot() re-checks them (plus the
 *     block's own validity: key, code page, translation generation) and
 *     leaves the block if anything changed or the slot redirected IP.
 *   - ninsts/ITC accounting for runs of native slots is batched and flushed
 *     before each helper call and at every exit.
 *
 * ── Validity ─────────────────────────────────────────────────────────────
 * Blocks remember the physical address and the bytes they were translated
 * from.  The interpreter's per-page store generation (bumped by every store
 * to a page hashing to that slot) and its instruction-translation
 * generation (bumped by srlz.i and every interruption) are checked on
 * entry; on a mismatch the translation or the bytes are re-verified rather
 * than recompiling, since both bump far more often than code changes.
 */

#ifndef MERCED_JIT_INCLUDED
#error "merced_jit.c is included by merced.c"
#endif

#include "jit_backend.h"

/* Blocks are short and end at every branch: each entry point (branch
 * target, return address, resume after an interruption) gets its own block,
 * so a long block duplicates the code after it once per entry.  At 16
 * bundles through conditional branches NT's boot filled a 1 GiB code buffer
 * every ~20 s; 8 bundles ending at branches ran ~45% more guest code than
 * that (64 MiB) in the same time. */
#define MJ_MAX_BUNDLES 8
#define MJ_MAX_SLOTS   (MJ_MAX_BUNDLES * 3)
#define MJ_CODE_SIZE   (256u << 20)    /* address space; pages fill as used */
#define MJ_HASH_BITS   18
#define MJ_JMP_CACHE   4096         /* direct-mapped front of the hash */
#define MJ_HASH_SIZE   (1u << MJ_HASH_BITS)

typedef struct MercedJitBlock MercedJitBlock;

typedef struct {
    uint64_t ip;            /* slot IP (bundle | slot) */
    uint8_t  bundle;        /* index into tbs[] */
    uint8_t  slot;          /* 0..2 */
} MercedJitSlot;

struct MercedJitBlock {
    MercedJitBlock *hash_next, *all_next;
    uint64_t va, pa, key;
    uint32_t code_gen;      /* page_generation[] the bytes were checked at */
    uint32_t tgen;          /* translation_generation the pa was checked at */
    uint32_t n_slots;       /* 0: not translatable, always interpret */
    uint32_t n_bundles;
    JitFn code;             /* returns the number of slots executed */
    MercedTbEntry *tbs;     /* decoded bundles (also the bytes to re-verify) */
    MercedJitSlot *slots;
};

/* Host TLB entry for direct RAM access (see mj_gen_mem()). */
#define MJ_HTLB_SIZE 256
typedef struct {
    uint64_t tag;           /* va page | 1 (read) or 2 (write); 0 = empty */
    uint64_t psr_key, rr, iva;
    uint64_t addend;        /* host address = va + addend */
    uint64_t pa_page;
    uint32_t gen;           /* tc->xlate_generation at fill */
    uint32_t slow_off;      /* page offset that must use the bus, or 0xFFFF */
    uint32_t pgidx;         /* tb_page_index(pa_page) */
    uint32_t pad;
} MjTlbEntry;
_Static_assert(sizeof(MjTlbEntry) == 64, "mj_gen_htlb() indexes by << 6");

typedef struct MercedJit {
    bool enabled;
    MercedTranslationCache *tc;
    MercedDirectPageFn direct_page;
    MjTlbEntry htlb[2 * MJ_HTLB_SIZE];   /* reads, then writes */
    JitBackend *be;         /* host code buffer (jit/jit_backend.h) */
    MercedJitBlock *hash[MJ_HASH_SIZE];
    /* The last block entered at each (hashed) IP: most entries hit here
     * without walking a hash chain.  Cleared with the hash. */
    MercedJitBlock *jmp_cache[MJ_JMP_CACHE];
    MercedJitBlock *all;
    MercedStatus status;    /* set by mj_slot() when a slot halts */
    /* A flush requested while a block runs (a helper slot can reach
     * tb_flush()) frees the blocks once it has returned. */
    bool in_block, flush_pending;
    struct {
        uint64_t compiled, flushes, blocks_run, slots_jit, slots_interp;
        uint64_t helper_calls, native_slots, helper_slots, nop_slots;
        uint64_t why_is, why_irq, why_extras, why_budget, why_empty;
        uint64_t direct_slots, mem_slow, mem_fill, mem_nofill_xlate,
                 mem_nofill_page;
        uint64_t reverify_ok, reverify_fail, retranslate;
        uint64_t flush_full, code_bytes, mid_entry;
        uint64_t why_x[9];      /* is db one-shot lp tb ss pmu observe fetch */
        uint64_t comp_new, comp_pa, comp_bytes;
    } stats;
    /* MERCED_JIT_STATS: compiles by block entry (hashed), to tell key
     * churn from flushes */
    struct MjCompProf { uint64_t ip, key; uint32_t n, keychanges; } *comp_prof;
    /* MERCED_JIT_STATS: helper calls by unit/major/x6, to show what to
     * translate natively next. */
    uint64_t *helper_profile;
} MercedJit;

/* A block is specialized on PSR.bn and CFM.sor (which stacked registers
 * rotate).  sof isn't part of it - at a function's entry it is the caller's
 * frame, so keying on it compiled one copy per caller; translated code
 * checks it before writing a stacked register instead (mj_gen_oof_guard()).  The rotation bases rrb.gr and
 * rrb.pr are read at run time, so a software-pipelined loop - which rotates
 * them every iteration - keeps one translation; keying on them compiled one
 * copy per rotation state (100+ for some of NT's loops).  Bases outside the
 * range rotation keeps them in (rrb.gr < sor * 8, rrb.pr < 48), which only
 * software writing CFM can produce, are keyed and compiled statically.
 * (rrb.fr only matters to FP handlers, which read CFM themselves.) */
#define MJ_CFM_FRAME_MASK  (UINT64_C(0xF) << 14)
#define MJ_CFM_RRB_MASK    ((UINT64_C(0x7F) << 18) | (UINT64_C(0x3F) << 32))
#define MJ_KEY_STATIC_RRB  (UINT64_C(1) << 62)
#define MJ_CFM_KEY_MASK    (MJ_CFM_FRAME_MASK | MJ_CFM_RRB_MASK)

static inline uint64_t mj_key(const Merced *m) {
    uint64_t cfm = m->cfm;
    unsigned sor8 = CFM_SOR(cfm) * 8;
    bool normal = (sor8 == 0 || CFM_RRB_GR(cfm) < sor8) &&
                  CFM_RRB_PR(cfm) < 48;
    uint64_t key = (cfm & MJ_CFM_FRAME_MASK) |
                   ((m->psr & PSR_BN) ? UINT64_C(1) << 63 : 0);
    if (!normal)
        key |= (cfm & MJ_CFM_RRB_MASK) | MJ_KEY_STATIC_RRB;
    return key;
}

static inline unsigned mj_hash(uint64_t ip, uint64_t key) {
    uint64_t h = (ip ^ (key * UINT64_C(0x9E3779B97F4A7C15))) *
                 UINT64_C(0xFF51AFD7ED558CCD);
    return (unsigned)(h >> (64 - MJ_HASH_BITS));
}

/* Slot IPs differ in the low four bits (slot) and the bundle address. */
static inline unsigned mj_jmp_index(uint64_t ip) {
    return (unsigned)(((ip >> 4) ^ (ip >> 16) ^ (ip << 2)) & (MJ_JMP_CACHE - 1));
}

static inline MercedJit *mj_of(Merced *m) {
    MercedTranslationCache *tc = m->translation_cache;
    return tc ? tc->jit : NULL;
}

static bool mj_stats_on;

static void mj_free_blocks(MercedJit *j) {
    MercedJitBlock *b = j->all;
    while (b) {
        MercedJitBlock *next = b->all_next;
        free(b);
        b = next;
    }
    j->all = NULL;
    memset(j->hash, 0, sizeof(j->hash));
    memset(j->jmp_cache, 0, sizeof(j->jmp_cache));
}

/* Drop every translation (code buffer full, or tb_flush()). */
static void mj_flush(MercedJit *j) {
    if (!j) return;
    memset(j->htlb, 0, sizeof(j->htlb));
    if (j->in_block) {
        /* tb_flush() reset the generations the running block checks after
         * each helper, so it leaves at the next one */
        j->flush_pending = true;
        return;
    }
    j->flush_pending = false;
    mj_free_blocks(j);
    if (j->be)
        jit_backend_reset(j->be);
    j->stats.flushes++;
}

static MercedJit *mj_stats_jit;
static void mj_print_stats(void);

static void mj_destroy(MercedJit *j) {
    if (!j) return;
    if (j == mj_stats_jit) {
        mj_print_stats();
        mj_stats_jit = NULL;
    }
    free(j->helper_profile);
    mj_free_blocks(j);
    jit_backend_free(j->be);
    free(j);
}

/* ── interrupt / timer conditions ─────────────────────────────────────── */

/* Could merced_step() deliver an interruption at a group boundary in the
 * current state?  (fast_step()'s conditions without the group_start test.)
 * While IVA is 0, ami_ivt_handoff_pending() depends on the IP, which moves
 * through a block - leave any pending interrupt to the interpreter then. */
static bool mj_irq_blocks(Merced *m) {
    if (!(m->ext_pending_count || m->timer_pending ||
          (m->msr[MSR_PMI_LATCH] & PMI_VECTORS)))
        return false;
    if (m->cr[CR_IVA] == 0)
        return true;
    if (pmi_deliverable(m))
        return true;
    if (!(m->psr & PSR_I) || !(m->psr & PSR_IC))
        return false;
    if (ext_highest_unmasked(m) >= 0)
        return true;
    return m->timer_pending && !(m->cr[CR_ITV] & (1ull << 16)) &&
           interrupt_unmasked(m, (uint8_t)m->cr[CR_ITV]);
}

/* Would charging `slots` more slots to the ITC cross cr.itm (latching the
 * timer, which itc_advance() checks per slot and batched accounting
 * doesn't)? */
static bool mj_timer_in_window(const Merced *m, uint64_t slots) {
    if (m->external_itc)
        return false;
    uint64_t deadline = m->cr[CR_ITM];
    uint64_t itc = m->ar[AR_ITC];
    if (deadline == 0 || (int64_t)(itc - deadline) >= 0)
        return false;
    return (int64_t)(itc + slots * m->slot_cycles - deadline) >= 0;
}

static void mj_htlb_fill(Merced *m, MercedJit *j, uint64_t va, bool write);

/* ── interpreted slots inside a translated block ──────────────────────── */

/* While any of these holds, fast_slot() does per-slot work translated code
 * doesn't reproduce (one-shot PSR bits, single-step / taken-branch /
 * lower-privilege traps, performance counting, the cache-array model), or
 * fast_step() would hand the slot to merced_step() (IA-32, debug
 * registers, uncached fetch).  No block is entered then, and a slot that
 * brings one about ends its block. */
static inline bool mj_extras(const Merced *m) {
    return (m->psr & (PSR_IS | PSR_DB | PSR_ONE_SHOT | PSR_LP | PSR_TB |
                      PSR_SS)) ||
           pmu_count_on || m->cache.observe || fetch_uncached(m);
}

/* Block slot `i` through fast_slot(), the interpreter's own per-slot code.
 * Returns 0 to continue with the next slot, or 1 to leave the block (the
 * slot halted or redirected IP, or changed something the rest of the block
 * was translated or admitted under).  With `fill`, a plain load or store
 * that completed refills the host TLB for its page. */
static inline unsigned mj_slot_common(Merced *m, MercedJitBlock *b,
                                      unsigned i, bool fill) {
    MercedTranslationCache *tc = m->translation_cache;
    MercedJit *j = tc->jit;
    const MercedJitSlot *js = &b->slots[i];
    const MercedTbEntry *tb = &b->tbs[js->bundle];
    unsigned slot = js->slot;
    uint64_t bundle_va = js->ip & ~UINT64_C(0xF);

    j->stats.helper_calls++;
    if (j->helper_profile) {
        bool x = tb->kind[slot] == SLOT_X;
        uint64_t raw = x ? tb->slots[2] : tb->slots[slot];
        unsigned u = x ? 4 :
                     (unsigned)(strchr("MIBF", bundle_units[tb->tmpl][slot]) - "MIBF");
        unsigned x6 = (unsigned)(u == 0 ? bits(raw, 30, 6) : bits(raw, 27, 6));
        j->helper_profile[(u << 10) | ((unsigned)bits(raw, 37, 4) << 6) | x6]++;
    }
    uint64_t va = fill ? gr_read(m, tb->a3[slot], NULL) : 0;
    if (fill) j->stats.mem_slow++;
    uint64_t faults = m->nfaults;
    m->ip = js->ip;
    bool redirected = false;
    MercedStatus st = fast_slot(m, tb, slot, bundle_va, &redirected);
    if (st != MERCED_OK) {
        j->status = st;
        return 1;
    }
    if (redirected)
        return 1;
    if (fill && m->nfaults == faults)
        mj_htlb_fill(m, j, va, tb->op[slot] == FOP_ST);

    /* Is the rest of the block still admissible? */
    if (tc->exit_request || j->flush_pending || mj_extras(m) ||
        mj_key(m) != b->key ||
        tc->translation_generation != b->tgen ||
        tc->page_generation[tb_page_index(b->pa)] != b->code_gen ||
        mj_irq_blocks(m) ||
        mj_timer_in_window(m, b->n_slots - (i + 1)))
        return 1;
    return 0;
}

static unsigned mj_slot(Merced *m, MercedJitBlock *b, unsigned i) {
    return mj_slot_common(m, b, i, false);
}

static unsigned mj_slot_fill(Merced *m, MercedJitBlock *b, unsigned i) {
    return mj_slot_common(m, b, i, true);
}

/* ── code generation ──────────────────────────────────────────────────── */

/* All host code is emitted through jit/jit_backend.h; nothing below knows
 * the host.  Virtual registers by role:
 *   T0/T1  a GR value and its NaT        T2/T3  a second value and NaT
 *   T4/T5  scratch for GR and predicate access (mj_gr_write() clobbers both,
 *          mj_gr_read() clobbers T5)
 *   S0/S1  values that live across a host TLB probe or a call */

#define MOFF(field) ((int32_t)offsetof(Merced, field))

typedef struct {
    JitBackend *b;
    bool bn;
    unsigned sor8, rrb_gr, rrb_pr, sof;
    bool dyn_rrb;               /* rotation bases read from CFM at run time */
    unsigned pending;           /* native slots not yet charged to ninsts */
    uint8_t last_stop;          /* stop bit of the slot being translated */
} MJGen;

static unsigned mj_stacked_index(const MJGen *g, unsigned r) {
    unsigned idx = r - 32;
    if (g->sor8 && idx < g->sor8)
        idx = (idx + g->rrb_gr) % g->sor8;
    return idx;
}

/* Does stacked register r rotate under run-time rrb.gr? */
static bool mj_gr_dyn(const MJGen *g, unsigned r) {
    return g->dyn_rrb && g->sor8 && r - 32 < g->sor8;
}

/* d = the logical (rotated) stacked index of r, stacked_phys() before the
 * bof: idx + rrb.gr, wrapped once (both are below sor * 8 here). */
static void mj_stacked_logical(MJGen *g, unsigned r, JitReg d) {
    JitBackend *b = g->b;
    if (!mj_gr_dyn(g, r)) {
        jit_movi(b, d, mj_stacked_index(g, r));
        return;
    }
    JitLabel inrange = jit_label(b);
    jit_ld_env(b, d, MOFF(cfm), 8);
    jit_shri(b, d, d, 18);
    jit_opi(b, JIT_AND, d, d, 0x7F);
    jit_opi(b, JIT_ADD, d, d, r - 32);
    jit_bri(b, JIT_LTU, d, g->sor8, 64, inrange);
    jit_opi(b, JIT_SUB, d, d, g->sor8);
    jit_bind(b, inrange);
}

/* T5 = physical gr_stack index of stacked register r. */
static void mj_stacked_phys(MJGen *g, unsigned r) {
    JitBackend *b = g->b;
    if (mj_gr_dyn(g, r)) {
        mj_stacked_logical(g, r, JIT_T5);
        jit_ld_env(b, JIT_T4, MOFF(bof), 4);  /* NB: callers allow T4 too */
        jit_op(b, JIT_ADD, JIT_T5, JIT_T5, JIT_T4);
    } else {
        jit_ld_env(b, JIT_T5, MOFF(bof), 4);
        jit_opi(b, JIT_ADD, JIT_T5, JIT_T5, (int64_t)mj_stacked_index(g, r));
    }
    jit_opi(b, JIT_AND, JIT_T5, JIT_T5, MERCED_RSE_CAPACITY - 1);
}

/* val = GR[r]; nat (unless JIT_NREGS) = its NaT bit.  Clobbers T5, and T4
 * for a rotating register. */
static void mj_gr_read(MJGen *g, unsigned r, JitReg val, JitReg nat) {
    JitBackend *b = g->b;
    bool want_nat = nat != JIT_NREGS;
    if (r == 0) {
        jit_movi(b, val, 0);
        if (want_nat) jit_movi(b, nat, 0);
    } else if (r < 16 || (r < 32 && g->bn)) {
        jit_ld_env(b, val, MOFF(gr_static) + 8 * (int32_t)r, 8);
        if (want_nat) jit_ld_env(b, nat, MOFF(nat_static) + (int32_t)r, 1);
    } else if (r < 32) {
        jit_ld_env(b, val, MOFF(gr_bank0) + 8 * (int32_t)(r - 16), 8);
        if (want_nat) jit_ld_env(b, nat, MOFF(nat_bank0) + (int32_t)(r - 16), 1);
    } else {
        mj_stacked_phys(g, r);
        jit_ld_envx(b, val, MOFF(gr_stack), JIT_T5, 8, 8);
        if (want_nat) jit_ld_envx(b, nat, MOFF(nat_stack), JIT_T5, 1, 1);
    }
}

/* GR[r] = val, NaT = nat (JIT_NREGS for 0).  gr_write_inl()'s sequence,
 * including the RSE dirty-boundary update.  Clobbers T4, T5. */
static void mj_gr_write(MJGen *g, unsigned r, JitReg val, JitReg nat) {
    JitBackend *b = g->b;
    if (r == 0)
        return;
    if (r < 32) {
        bool st = r < 16 || g->bn;
        int32_t voff = st ? MOFF(gr_static) + 8 * (int32_t)r
                          : MOFF(gr_bank0) + 8 * (int32_t)(r - 16);
        int32_t noff = st ? MOFF(nat_static) + (int32_t)r
                          : MOFF(nat_bank0) + (int32_t)(r - 16);
        jit_st_env(b, voff, val, 8);
        if (nat != JIT_NREGS) jit_st_env(b, noff, nat, 1);
        else                  jit_st_envi(b, noff, 0, 1);
        return;
    }
    /* T4 = logical index, T5 = physical */
    mj_stacked_logical(g, r, JIT_T4);
    jit_ld_env(b, JIT_T5, MOFF(bof), 4);
    jit_op(b, JIT_ADD, JIT_T5, JIT_T5, JIT_T4);
    jit_opi(b, JIT_AND, JIT_T5, JIT_T5, MERCED_RSE_CAPACITY - 1);
    jit_st_envx(b, MOFF(gr_stack), JIT_T5, 8, val, 8);
    if (nat != JIT_NREGS) jit_st_envx(b, MOFF(nat_stack), JIT_T5, 1, nat, 1);
    else                  jit_st_envxi(b, MOFF(nat_stack), JIT_T5, 1, 0, 1);
    /* if (bof_total + logical < rse_flushed_regs) rse_flushed_regs = ...; */
    JitLabel skip = jit_label(b);
    jit_ld_env(b, JIT_T5, MOFF(bof_total), 8);
    jit_op(b, JIT_ADD, JIT_T4, JIT_T4, JIT_T5);
    jit_ld_env(b, JIT_T5, MOFF(rse_flushed_regs), 8);
    jit_br(b, JIT_GE, JIT_T4, JIT_T5, 64, skip);
    jit_st_env(b, MOFF(rse_flushed_regs), JIT_T4, 8);
    jit_bind(b, skip);
}

/* d = the PR bit holding predicate p: 16 + (p - 16 + rrb.pr) mod 48 for a
 * rotating predicate, read from CFM at run time unless the block is static
 * (both terms are below 48, so one wrap suffices). */
static unsigned mj_pr_bit(const MJGen *g, unsigned p) {
    if (p < 16) return p;
    return 16 + ((p - 16 + g->rrb_pr) % 48);
}

static void mj_pr_bitreg(MJGen *g, unsigned p, JitReg d) {
    JitBackend *b = g->b;
    if (p < 16 || !g->dyn_rrb) {
        jit_movi(b, d, mj_pr_bit(g, p));
        return;
    }
    JitLabel inrange = jit_label(b);
    jit_ld_env(b, d, MOFF(cfm), 8);
    jit_shri(b, d, d, 32);
    jit_opi(b, JIT_AND, d, d, 0x3F);
    jit_opi(b, JIT_ADD, d, d, p - 16);
    jit_bri(b, JIT_LTU, d, 48, 64, inrange);
    jit_opi(b, JIT_SUB, d, d, 48);
    jit_bind(b, inrange);
    jit_opi(b, JIT_ADD, d, d, 16);
}

/* Go to `l` when predicate p is false.  Clobbers T4, T5. */
static void mj_br_pred_false(MJGen *g, unsigned p, JitLabel l) {
    jit_ld_env(g->b, JIT_T4, MOFF(pr), 8);
    if (p < 16 || !g->dyn_rrb) {
        jit_brbit(g->b, JIT_T4, mj_pr_bit(g, p), false, l);
    } else {
        mj_pr_bitreg(g, p, JIT_T5);
        jit_brbitr(g->b, JIT_T4, JIT_T5, false, l);
    }
}

/* A label the slot jumps to when its qualifying predicate is false, or -1
 * for p0.  Clobbers T4, T5. */
static JitLabel mj_skip_unless_qp(MJGen *g, unsigned qp) {
    if (qp == 0) return -1;
    JitLabel skip = jit_label(g->b);
    mj_br_pred_false(g, qp, skip);
    return skip;
}

static void mj_end_skip(MJGen *g, JitLabel skip) {
    if (skip >= 0) jit_bind(g->b, skip);
}

/* PR[p] = register src (0/1), optionally inverted.  Clobbers T4, T5. */
static void mj_pr_write_reg(MJGen *g, unsigned p, JitReg src, bool invert) {
    if (p == 0) return;
    JitBackend *b = g->b;
    JitLabel done = jit_label(b);
    jit_ld_env(b, JIT_T4, MOFF(pr), 8);
    mj_pr_bitreg(g, p, JIT_T5);
    jit_bitclr(b, JIT_T4, JIT_T5);
    jit_bri(b, invert ? JIT_NE : JIT_EQ, src, 0, 64, done);
    jit_bitset(b, JIT_T4, JIT_T5);
    jit_bind(b, done);
    jit_st_env(b, MOFF(pr), JIT_T4, 8);
}

static void mj_pr_clear(MJGen *g, unsigned p) {
    if (p == 0) return;
    jit_ld_env(g->b, JIT_T4, MOFF(pr), 8);
    mj_pr_bitreg(g, p, JIT_T5);
    jit_bitclr(g->b, JIT_T4, JIT_T5);
    jit_st_env(g->b, MOFF(pr), JIT_T4, 8);
}

/* Charge `k` slots executed natively since the last sync point.  Clobbers
 * T4. */
static void mj_account(MJGen *g, unsigned k) {
    JitBackend *b = g->b;
    if (!k) return;
    JitLabel skip = jit_label(b);
    jit_addi_env(b, MOFF(ninsts), (int32_t)k);
    jit_ld_env(b, JIT_T4, MOFF(external_itc), 1);
    jit_bri(b, JIT_NE, JIT_T4, 0, 32, skip);
    jit_ld_env(b, JIT_T4, MOFF(slot_cycles), 4);
    if (k != 1) jit_muli(b, JIT_T4, JIT_T4, (int32_t)k);
    jit_add_env(b, MOFF(ar) + 8 * AR_ITC, JIT_T4);
    jit_add_env(b, MOFF(time_base), JIT_T4);
    jit_bind(b, skip);
}

/* Leave the block after slot index `idx`, which executed natively (or after
 * an interpreted slot when native_last is false): charge pending slots, set
 * the successor state merced_step() would, return idx + 1 slots executed. */
static void mj_exit(MJGen *g, unsigned idx, unsigned pending,
                    uint64_t last_bundle, bool native_last,
                    uint8_t group_start, uint64_t next_ip) {
    JitBackend *b = g->b;
    mj_account(g, pending);
    if (native_last) {
        /* merced_step() leaves the last slot's stop bit here */
        jit_st_envi(b, MOFF(slot_stop_after), g->last_stop, 1);
        jit_st_envi(b, MOFF(last_successful_bundle), (int64_t)last_bundle, 8);
        jit_st_envi(b, MOFF(group_start), group_start, 1);
        jit_st_envi(b, MOFF(ip), (int64_t)next_ip, 8);
    }
    jit_reti(b, idx + 1);
}

/* Native forms */

static void mj_gen_alu(MJGen *g, const MercedTbEntry *tb, unsigned s) {
    JitBackend *b = g->b;
    unsigned op = tb->op[s];
    int64_t imm = tb->imm[s];
    JitLabel skip = mj_skip_unless_qp(g, tb->qpr[s]);
    mj_gr_read(g, tb->a3[s], JIT_T0, JIT_T1);                 /* b = r3 */
    switch (op) {
    case FOP_ADD: case FOP_SUB: case FOP_AND: case FOP_ANDCM:
    case FOP_OR: case FOP_XOR: case FOP_SHLADD:
        mj_gr_read(g, tb->a2[s], JIT_T2, JIT_T3);             /* a = r2 */
        switch (op) {
        case FOP_ADD:
            jit_op(b, JIT_ADD, JIT_T0, JIT_T0, JIT_T2);
            if (imm) jit_opi(b, JIT_ADD, JIT_T0, JIT_T0, imm);
            break;
        case FOP_SUB:
            jit_op(b, JIT_SUB, JIT_T0, JIT_T2, JIT_T0);
            if (imm) jit_opi(b, JIT_SUB, JIT_T0, JIT_T0, imm);
            break;
        case FOP_AND:   jit_op(b, JIT_AND, JIT_T0, JIT_T0, JIT_T2); break;
        case FOP_ANDCM: jit_op(b, JIT_ANDN, JIT_T0, JIT_T2, JIT_T0); break;
        case FOP_OR:    jit_op(b, JIT_OR, JIT_T0, JIT_T0, JIT_T2); break;
        case FOP_XOR:   jit_op(b, JIT_XOR, JIT_T0, JIT_T0, JIT_T2); break;
        default:        /* SHLADD */
            jit_shli(b, JIT_T2, JIT_T2, (unsigned)imm);
            jit_op(b, JIT_ADD, JIT_T0, JIT_T0, JIT_T2);
            break;
        }
        jit_op(b, JIT_OR, JIT_T1, JIT_T1, JIT_T3);
        break;
    default:
        switch (op) {
        case FOP_ADDI:   jit_opi(b, JIT_ADD, JIT_T0, JIT_T0, imm); break;
        case FOP_SUBI:   jit_neg(b, JIT_T0, JIT_T0);
                         jit_opi(b, JIT_ADD, JIT_T0, JIT_T0, imm); break;
        case FOP_ANDI:   jit_opi(b, JIT_AND, JIT_T0, JIT_T0, imm); break;
        case FOP_ANDCMI: jit_not(b, JIT_T0, JIT_T0);
                         jit_opi(b, JIT_AND, JIT_T0, JIT_T0, imm); break;
        case FOP_ORI:    jit_opi(b, JIT_OR, JIT_T0, JIT_T0, imm); break;
        default:         jit_opi(b, JIT_XOR, JIT_T0, JIT_T0, imm); break;
        }
        break;
    }
    mj_gr_write(g, tb->a1[s], JIT_T0, JIT_T1);
    mj_end_skip(g, skip);
}

static void mj_gen_ext(MJGen *g, const MercedTbEntry *tb, unsigned s) {
    static const JitExt ext[8] = {
        [0] = JIT_ZX8, [1] = JIT_ZX16, [2] = JIT_ZX32,
        [4] = JIT_SX8, [5] = JIT_SX16, [6] = JIT_SX32,
    };
    JitLabel skip = mj_skip_unless_qp(g, tb->qpr[s]);
    mj_gr_read(g, tb->a3[s], JIT_T0, JIT_T1);
    jit_ext(g->b, ext[tb->x[s] & 7], JIT_T0, JIT_T0);
    mj_gr_write(g, tb->a1[s], JIT_T0, JIT_T1);
    mj_end_skip(g, skip);
}

/* fast_op()'s FOP_CMP: a false predicate does nothing to a normal compare
 * and clears both targets of .unc; a NaT source (true predicate) clears
 * both. */
static void mj_gen_cmp(MJGen *g, const MercedTbEntry *tb, unsigned s) {
    JitBackend *b = g->b;
    unsigned x = tb->x[s], p1 = tb->a1[s], p2 = tb->a2[s];
    unsigned qpr = tb->qpr[s];
    bool unc = (x & CMPF_UNC) != 0;
    unsigned bits = (x & CMPF_CMP4) ? 32 : 64;
    JitLabel qp_false = jit_label(b), nat = jit_label(b), done = jit_label(b);

    if (qpr)
        mj_br_pred_false(g, qpr, qp_false);
    mj_gr_read(g, tb->a3[s], JIT_T0, JIT_T1);                 /* b */
    if (x & CMPF_IMM) {
        jit_movi(b, JIT_T2, (uint64_t)(int64_t)tb->imm[s]);   /* a */
    } else {
        mj_gr_read(g, (unsigned)tb->imm[s], JIT_T2, JIT_T3);
        jit_op(b, JIT_OR, JIT_T1, JIT_T1, JIT_T3);
    }
    JitCond cc = (x & CMPF_REL) == CMPF_LT ? JIT_LT :
                 (x & CMPF_REL) == CMPF_LTU ? JIT_LTU : JIT_EQ;
    jit_setcond(b, cc, JIT_T3, JIT_T2, JIT_T0, bits);         /* a rel b */
    jit_bri(b, JIT_NE, JIT_T1, 0, 64, nat);
    mj_pr_write_reg(g, p1, JIT_T3, false);
    mj_pr_write_reg(g, p2, JIT_T3, true);
    jit_jmp(b, done);
    jit_bind(b, nat);
    if (unc) jit_bind(b, qp_false);
    mj_pr_clear(g, p1);
    mj_pr_clear(g, p2);
    jit_bind(b, done);
    if (!unc) jit_bind(b, qp_false);
}

static void mj_gen_movl(MJGen *g, const MercedTbEntry *tb, unsigned s) {
    uint64_t raw = tb->slots[2], lraw = tb->slots[1];
    uint64_t imm = (bits(raw, 36, 1) << 63) | (lraw << 22) |
                   (bits(raw, 21, 1) << 21) | (bits(raw, 22, 5) << 16) |
                   (bits(raw, 27, 9) << 7) | bits(raw, 13, 7);
    JitLabel skip = mj_skip_unless_qp(g, tb->qpr[s]);
    jit_movi(g->b, JIT_T0, imm);
    mj_gr_write(g, (unsigned)bits(raw, 6, 7), JIT_T0, JIT_NREGS);
    mj_end_skip(g, skip);
}

/* How a slot is translated:
 *   MJ_NOP      nothing;
 *   MJ_NATIVE   inline host code;
 *   MJ_MEM      a plain load or store: inline through the host TLB when
 *               it hits, mj_slot_fill() otherwise;
 *   MJ_FPX      setf.sig / getf.sig: inline unless the FP bank they touch
 *               is disabled (PSR.dfl/dfh), when mj_slot() raises the fault;
 *   MJ_CALL     mj_slot(): fast_slot(), then the block's re-checks.
 * A native slot that writes a stacked register first checks it is inside
 * the current frame (mj_gen_oof_guard()); otherwise the block ends before
 * the slot and the interpreter raises the Illegal Operation fault. */
typedef enum { MJ_NOP, MJ_NATIVE, MJ_MEM, MJ_FPX, MJ_CALL } MJHow;

static MJHow mj_classify(const MercedTbEntry *tb, unsigned s) {
    switch (tb->kind[s]) {
    case SLOT_NOP:
        return MJ_NOP;
    case SLOT_X:
        return bits(tb->slots[2], 37, 4) == 6 ? MJ_NATIVE : MJ_CALL;
    default:
        break;
    }
    switch (tb->op[s]) {
    case FOP_ADD: case FOP_SUB: case FOP_AND: case FOP_ANDCM:
    case FOP_OR: case FOP_XOR: case FOP_SHLADD:
    case FOP_ADDI: case FOP_SUBI: case FOP_ANDI: case FOP_ANDCMI:
    case FOP_ORI: case FOP_XORI: case FOP_EXT:
    case FOP_CMP: case FOP_BR_COND: case FOP_BR_CLOOP:
        return MJ_NATIVE;
    case FOP_LD: case FOP_ST:
        return MJ_MEM;
    case FOP_SETF_SIG: case FOP_GETF_SIG:
        return MJ_FPX;
    default:
        return MJ_CALL;
    }
}

/* The highest stacked register a natively translated slot writes, or 0. */
static unsigned mj_stacked_dest(const MercedTbEntry *tb, unsigned s) {
    unsigned hi = 0, r;
    bool postinc = (tb->x[s] & (LDST_INC_IMM | LDST_INC_REG)) != 0;
    if (tb->kind[s] == SLOT_X) {
        r = (unsigned)bits(tb->slots[2], 6, 7);
        return r >= 32 ? r : 0;
    }
    switch (tb->op[s]) {
    case FOP_CMP: case FOP_BR_COND: case FOP_BR_CLOOP: case FOP_SETF_SIG:
        return 0;
    case FOP_ST:
        r = postinc ? tb->a3[s] : 0;
        return r >= 32 ? r : 0;
    case FOP_LD:
        hi = tb->a1[s];
        if (postinc && tb->a3[s] > hi) hi = tb->a3[s];
        return hi >= 32 ? hi : 0;
    default:
        r = tb->a1[s];
        return r >= 32 ? r : 0;
    }
}

/* Does the block end after this slot?  At any branch (see MJ_MAX_BUNDLES). */
static bool mj_ends_block(const MercedTbEntry *tb, unsigned s) {
    if (tb->kind[s] == SLOT_X)
        return bits(tb->slots[2], 37, 4) >= 0xC;          /* brl */
    return tb->kind[s] == SLOT_EXEC && bundle_units[tb->tmpl][s] == 'B';
}

static uint64_t mj_next_ip(const MercedJitSlot *js, const MercedTbEntry *tb) {
    uint64_t bva = js->ip & ~UINT64_C(0xF);
    if (tb->kind[js->slot] == SLOT_X || js->slot == 2) return bva + 16;
    return bva | (js->slot + 1u);
}

/* Before native slot i writes stacked register r: if r is beyond the
 * frame, leave the block with slot i not executed (the interpreter raises
 * the Illegal Operation fault).  Clobbers T4. */
static void mj_gen_oof_guard(MJGen *g, const MercedJitBlock *blk, unsigned i,
                             unsigned r) {
    JitBackend *b = g->b;
    JitLabel in_frame = jit_label(b);
    jit_ld_env(b, JIT_T4, MOFF(cfm), 8);
    jit_opi(b, JIT_AND, JIT_T4, JIT_T4, 0x7F);                  /* sof */
    jit_bri(b, JIT_GTU, JIT_T4, (int64_t)(r - 32), 64, in_frame);
    if (i == 0) {
        jit_reti(b, 0);             /* mj_run() interprets the slot */
    } else {
        const MercedJitSlot *prev = &blk->slots[i - 1];
        const MercedTbEntry *ptb = &blk->tbs[prev->bundle];
        if (g->pending) {
            mj_account(g, g->pending);
            jit_st_envi(b, MOFF(last_successful_bundle),
                        (int64_t)(prev->ip & ~UINT64_C(0xF)), 8);
        }
        jit_st_envi(b, MOFF(slot_stop_after), ptb->stop[prev->slot], 1);
        jit_st_envi(b, MOFF(group_start), ptb->stop[prev->slot], 1);
        jit_st_envi(b, MOFF(ip), (int64_t)blk->slots[i].ip, 8);
        jit_reti(b, i);
    }
    jit_bind(b, in_frame);
}

/* Before a slot that calls into the interpreter: bring the CPU up to date
 * with the natively executed slots before it. */
static void mj_gen_sync(MJGen *g, const MercedJitBlock *blk, unsigned i) {
    if (g->pending) {
        const MercedJitSlot *prev = &blk->slots[i - 1];
        mj_account(g, g->pending);
        jit_st_envi(g->b, MOFF(last_successful_bundle),
                    (int64_t)(prev->ip & ~UINT64_C(0xF)), 8);
        g->pending = 0;
    }
    if (i > 0) {
        const MercedJitSlot *prev = &blk->slots[i - 1];
        const MercedTbEntry *ptb = &blk->tbs[prev->bundle];
        jit_st_envi(g->b, MOFF(group_start), ptb->stop[prev->slot], 1);
    }
}

/* Slot i through mj_slot() (or mj_slot_fill()).  Expects mj_gen_sync(). */
static void mj_gen_call(MJGen *g, const MercedJitBlock *blk, unsigned i,
                        bool fill) {
    JitBackend *b = g->b;
    JitLabel cont = jit_label(b);
    jit_call(b, fill ? (const void *)mj_slot_fill : (const void *)mj_slot,
             (uintptr_t)blk, i);
    jit_bri(b, JIT_EQ, JIT_T0, 0, 32, cont);      /* mj_slot() is unsigned */
    jit_reti(b, i + 1);
    jit_bind(b, cont);
}

/* ── direct RAM access ────────────────────────────────────────────────────
 * mj_gen_mem() is FOP_LD/FOP_ST with the access done inline when the page
 * is in the host TLB, and mj_slot_fill() otherwise, which runs the slot
 * through fast_slot() and then (re)fills the host TLB entry from the
 * translation it just used.  An entry carries what makes the interpreter's
 * soft-TLB entry for that page valid - generation, the PSR bits the
 * permission checks read, the region register, and IVA (which selects the
 * access kind and whether the address is cacheable at all) - so it's valid
 * exactly when that soft-TLB entry is, and only pages the machine calls
 * plain RAM are entered. */

static void mj_htlb_fill(Merced *m, MercedJit *j, uint64_t va, bool write) {
    MercedTranslationCache *tc = m->translation_cache;
    uint64_t pa;
    if (!(m->psr & PSR_DT)) {
        pa = va & MERCED_PHYS_MASK;
    } else if (!xlate_cacheable(m, va) ||
               !xlate_lookup(m, va, xlate_kind(m, false, write ? ISR_W : ISR_R),
                             &pa)) {
        j->stats.mem_nofill_xlate++;
        return;
    }
    uint16_t slow_off = 0xFFFF;
    uint64_t pa_page = pa & ~UINT64_C(0xFFF);
    uint8_t *host = j->direct_page(m->bus.ud, pa_page, &slow_off);
    if (!host) {
        if (mj_stats_on && j->stats.mem_nofill_page++ < 20)
            fprintf(stderr, "merced-jit: not direct: va=%016" PRIX64
                    " pa=%016" PRIX64 " ip=%016" PRIX64 "\n", va, pa, m->ip);
        return;
    }
    j->stats.mem_fill++;
    MjTlbEntry *e = &j->htlb[((va >> 12) & (MJ_HTLB_SIZE - 1)) +
                             (write ? MJ_HTLB_SIZE : 0)];
    e->tag = (va & ~UINT64_C(0xFFF)) | (write ? 2 : 1);
    e->psr_key = m->psr & XLATE_PSR_KEY;
    e->rr = m->rr[va >> 61];
    e->iva = m->cr[CR_IVA];
    e->addend = (uint64_t)(uintptr_t)host - (va & ~UINT64_C(0xFFF));
    e->pa_page = pa_page;
    e->gen = tc->xlate_generation;
    e->slow_off = slow_off;
    e->pgidx = tb_page_index(pa_page);
}

static void mj_htlb_flush(MercedJit *j) {
    memset(j->htlb, 0, sizeof(j->htlb));
}

#define TOFF(field) ((int32_t)offsetof(MjTlbEntry, field))

/* Inline host-TLB probe for va in T0.  On a hit T2 = the entry and T3 = the
 * host address; otherwise goes to `miss`.  Clobbers T1, T4. */
static void mj_gen_htlb(MJGen *g, MercedJit *j, unsigned size, bool write,
                        JitLabel miss) {
    JitBackend *b = g->b;
    /* entry = &htlb[write half][(va >> 12) & (SIZE - 1)] */
    jit_shri(b, JIT_T2, JIT_T0, 12);
    jit_opi(b, JIT_AND, JIT_T2, JIT_T2, MJ_HTLB_SIZE - 1);
    jit_shli(b, JIT_T2, JIT_T2, 6);                     /* sizeof(MjTlbEntry) */
    jit_movi(b, JIT_T1, (uint64_t)(uintptr_t)&j->htlb[write ? MJ_HTLB_SIZE : 0]);
    jit_op(b, JIT_ADD, JIT_T2, JIT_T2, JIT_T1);
    /* tag: page | access */
    jit_opi(b, JIT_AND, JIT_T1, JIT_T0, -4096);
    jit_opi(b, JIT_OR, JIT_T1, JIT_T1, write ? 2 : 1);
    jit_ld(b, JIT_T4, JIT_T2, TOFF(tag), 8);
    jit_br(b, JIT_NE, JIT_T1, JIT_T4, 64, miss);
    /* within the page, and not at the page's bus-only offset */
    jit_opi(b, JIT_AND, JIT_T1, JIT_T0, 0xFFF);
    jit_bri(b, JIT_GTU, JIT_T1, (int64_t)(0x1000 - size), 64, miss);
    jit_ld(b, JIT_T4, JIT_T2, TOFF(slow_off), 4);
    jit_br(b, JIT_EQ, JIT_T1, JIT_T4, 64, miss);
    /* PSR key */
    jit_ld_env(b, JIT_T1, MOFF(psr), 8);
    jit_opi(b, JIT_AND, JIT_T1, JIT_T1, (int64_t)XLATE_PSR_KEY);
    jit_ld(b, JIT_T4, JIT_T2, TOFF(psr_key), 8);
    jit_br(b, JIT_NE, JIT_T1, JIT_T4, 64, miss);
    /* region register */
    jit_shri(b, JIT_T1, JIT_T0, 61);
    jit_ld_envx(b, JIT_T1, MOFF(rr), JIT_T1, 8, 8);
    jit_ld(b, JIT_T4, JIT_T2, TOFF(rr), 8);
    jit_br(b, JIT_NE, JIT_T1, JIT_T4, 64, miss);
    /* IVA */
    jit_ld_env(b, JIT_T1, MOFF(cr) + 8 * CR_IVA, 8);
    jit_ld(b, JIT_T4, JIT_T2, TOFF(iva), 8);
    jit_br(b, JIT_NE, JIT_T1, JIT_T4, 64, miss);
    /* soft-TLB generation */
    jit_movi(b, JIT_T1, (uint64_t)(uintptr_t)&j->tc->xlate_generation);
    jit_ld(b, JIT_T1, JIT_T1, 0, 4);
    jit_ld(b, JIT_T4, JIT_T2, TOFF(gen), 4);
    jit_br(b, JIT_NE, JIT_T1, JIT_T4, 32, miss);
    /* host address */
    jit_ld(b, JIT_T3, JIT_T2, TOFF(addend), 8);
    jit_op(b, JIT_ADD, JIT_T3, JIT_T3, JIT_T0);
}

static bool mj_direct_mem_ok(const MercedJit *j, const MercedTbEntry *tb,
                             unsigned s) {
    /* fast_dtranslate()/phys_write() debug watches need the slow path */
    return j->direct_page && (tb->op[s] == FOP_LD || tb->op[s] == FOP_ST) &&
           !mmfault_hi && !watch_va_end && !watch_pa_end;
}

/* fast_op()'s FOP_LD / FOP_ST, inline when the host TLB hits. */
static void mj_gen_mem(MJGen *g, MercedJit *j, const MercedJitBlock *blk,
                       unsigned i) {
    JitBackend *b = g->b;
    const MercedJitSlot *js = &blk->slots[i];
    const MercedTbEntry *tb = &blk->tbs[js->bundle];
    unsigned s = js->slot;
    uint64_t bva = js->ip & ~UINT64_C(0xF);
    unsigned x = tb->x[s], size = x & LDST_SIZE;
    unsigned r1 = tb->a1[s], r2 = tb->a2[s], r3 = tb->a3[s];
    int64_t imm = tb->imm[s];
    bool store = tb->op[s] == FOP_ST;
    MercedTranslationCache *tc = j->tc;
    JitLabel miss = jit_label(b), done = jit_label(b), called = jit_label(b);

    JitLabel skip = mj_skip_unless_qp(g, tb->qpr[s]);
    mj_gr_read(g, r3, JIT_T0, JIT_T1);                  /* va, n3 */
    jit_bri(b, JIT_NE, JIT_T1, 0, 64, miss);
    if (size > 1) {                     /* misaligned: fast_op() decides */
        jit_opi(b, JIT_AND, JIT_T1, JIT_T0, (int64_t)(size - 1));
        jit_bri(b, JIT_NE, JIT_T1, 0, 64, miss);
    }
    if (store) {
        mj_gr_read(g, r2, JIT_S1, JIT_S0);              /* value, NaT */
        jit_bri(b, JIT_NE, JIT_S0, 0, 64, miss);
        /* ALAT entries must see the store */
        jit_ld_env(b, JIT_T1, MOFF(alat_valid_mask), 4);
        jit_bri(b, JIT_NE, JIT_T1, 0, 32, miss);
    }
    mj_gen_htlb(g, j, size, store, miss);

    if (!store) {
        jit_ld(b, JIT_T1, JIT_T3, 0, size);
        /* reg_inc is read before r1 is written (r2 may be r1) */
        if (x & LDST_INC_REG) mj_gr_read(g, r2, JIT_S1, JIT_S0);
        mj_gr_write(g, r1, JIT_T1, JIT_NREGS);
        if (x & (LDST_INC_IMM | LDST_INC_REG)) {
            /* base re-read after the load wrote r1 (r1 may be r3) */
            mj_gr_read(g, r3, JIT_T0, JIT_T1);
            if (x & LDST_INC_IMM) {
                jit_opi(b, JIT_ADD, JIT_T0, JIT_T0, imm);
            } else {
                jit_op(b, JIT_ADD, JIT_T0, JIT_T0, JIT_S1);
                jit_op(b, JIT_OR, JIT_T1, JIT_T1, JIT_S0);
            }
            mj_gr_write(g, r3, JIT_T0, JIT_T1);
        }
    } else {
        jit_st(b, JIT_T3, 0, JIT_S1, size);
        /* phys_write(): page generation, then the fetch cache */
        JitLabel no_bc = jit_label(b);
        jit_ld(b, JIT_T1, JIT_T2, TOFF(pgidx), 4);
        jit_shli(b, JIT_T1, JIT_T1, 2);
        jit_movi(b, JIT_T4, (uint64_t)(uintptr_t)tc->page_generation);
        jit_op(b, JIT_ADD, JIT_T1, JIT_T1, JIT_T4);
        jit_ld(b, JIT_T4, JIT_T1, 0, 4);
        jit_opi(b, JIT_ADD, JIT_T4, JIT_T4, 1);
        jit_st(b, JIT_T1, 0, JIT_T4, 4);
        jit_ld_env(b, JIT_T1, MOFF(bundle_cache_valid), 1);
        jit_bri(b, JIT_EQ, JIT_T1, 0, 32, no_bc);
        jit_opi(b, JIT_AND, JIT_T1, JIT_T0, 0xFFF);       /* pa */
        jit_ld(b, JIT_T4, JIT_T2, TOFF(pa_page), 8);
        jit_op(b, JIT_OR, JIT_T1, JIT_T1, JIT_T4);
        jit_ld_env(b, JIT_T4, MOFF(bundle_cache_pa), 8);
        jit_opi(b, JIT_ADD, JIT_T5, JIT_T4, 16);
        jit_br(b, JIT_GEU, JIT_T1, JIT_T5, 64, no_bc);    /* pa >= bc + 16 */
        jit_opi(b, JIT_ADD, JIT_T5, JIT_T1, (int64_t)size);
        jit_br(b, JIT_GEU, JIT_T4, JIT_T5, 64, no_bc);    /* bc >= pa + size */
        jit_st_envi(b, MOFF(bundle_cache_valid), 0, 1);
        jit_bind(b, no_bc);
        /* post-increment from the original va (and NaT n3 = 0) */
        if (x & LDST_INC_IMM) {
            jit_opi(b, JIT_ADD, JIT_T0, JIT_T0, imm);
            mj_gr_write(g, r3, JIT_T0, JIT_NREGS);
        } else if (x & LDST_INC_REG) {
            mj_gr_read(g, r1, JIT_S1, JIT_S0);
            jit_op(b, JIT_ADD, JIT_T0, JIT_T0, JIT_S1);
            mj_gr_write(g, r3, JIT_T0, JIT_S0);
        }
        /* a store into this block's own page: leave, re-verify */
        JitLabel same = jit_label(b);
        jit_movi(b, JIT_T1, (uint64_t)(uintptr_t)
                 &tc->page_generation[tb_page_index(blk->pa)]);
        jit_ld(b, JIT_T1, JIT_T1, 0, 4);
        jit_movi(b, JIT_T4, (uint64_t)(uintptr_t)&blk->code_gen);
        jit_ld(b, JIT_T4, JIT_T4, 0, 4);
        jit_br(b, JIT_EQ, JIT_T1, JIT_T4, 32, same);
        mj_exit(g, i, 1, bva, true, tb->stop[s], mj_next_ip(js, tb));
        jit_bind(b, same);
    }
    jit_jmp(b, done);

    jit_bind(b, miss);
    mj_gen_call(g, blk, i, true);
    jit_jmp(b, called);

    /* the slot ran inline (or its predicate was false): finish it as
     * fast_slot() would */
    jit_bind(b, done);
    mj_end_skip(g, skip);
    mj_account(g, 1);
    jit_st_envi(b, MOFF(last_successful_bundle), (int64_t)bva, 8);
    jit_st_envi(b, MOFF(slot_stop_after), tb->stop[s], 1);
    jit_st_envi(b, MOFF(group_start), tb->stop[s], 1);
    jit_st_envi(b, MOFF(ip), (int64_t)mj_next_ip(js, tb), 8);
    jit_bind(b, called);
}

#define FOFF(field) ((int32_t)offsetof(MercedFpReg, field))

/* T5 = byte offset of FR[f] in m->fr (fr_phys(): f >= 32 rotates by
 * rrb.fr, below 96 except after software writes CFM - hence two wraps). */
static void mj_fr_offset(MJGen *g, unsigned f) {
    JitBackend *b = g->b;
    if (f < 32) {
        jit_movi(b, JIT_T5, (uint64_t)f * sizeof(MercedFpReg));
        return;
    }
    JitLabel w1 = jit_label(b), w2 = jit_label(b);
    jit_ld_env(b, JIT_T5, MOFF(cfm), 8);
    jit_shri(b, JIT_T5, JIT_T5, 25);
    jit_opi(b, JIT_AND, JIT_T5, JIT_T5, 0x7F);
    jit_opi(b, JIT_ADD, JIT_T5, JIT_T5, f - 32);
    jit_bri(b, JIT_LTU, JIT_T5, 96, 64, w1);
    jit_opi(b, JIT_SUB, JIT_T5, JIT_T5, 96);
    jit_bri(b, JIT_LTU, JIT_T5, 96, 64, w2);
    jit_opi(b, JIT_SUB, JIT_T5, JIT_T5, 96);
    jit_bind(b, w2);
    jit_bind(b, w1);
    jit_opi(b, JIT_ADD, JIT_T5, JIT_T5, 32);
    jit_muli(b, JIT_T5, JIT_T5, (int32_t)sizeof(MercedFpReg));
}

/* fast_op()'s FOP_SETF_SIG / FOP_GETF_SIG with fast_slot()'s FP-disabled
 * check in front: inline, or mj_slot() when the bank is disabled. */
static void mj_gen_fpx(MJGen *g, const MercedJitBlock *blk, unsigned i) {
    JitBackend *b = g->b;
    const MercedJitSlot *js = &blk->slots[i];
    const MercedTbEntry *tb = &blk->tbs[js->bundle];
    unsigned s = js->slot;
    uint64_t bva = js->ip & ~UINT64_C(0xF);
    unsigned r1 = tb->a1[s], r2 = tb->a2[s];
    bool setf = tb->op[s] == FOP_SETF_SIG;
    unsigned banks = fp_banks_used(bundle_units[tb->tmpl][s], tb->slots[s]);
    uint64_t disable = ((banks & 1u) ? PSR_DFL : 0) | ((banks & 2u) ? PSR_DFH : 0);
    JitLabel slow = jit_label(b), done = jit_label(b), called = jit_label(b);

    JitLabel skip = mj_skip_unless_qp(g, tb->qpr[s]);
    if (disable) {
        jit_ld_env(b, JIT_T4, MOFF(psr), 8);
        jit_opi(b, JIT_AND, JIT_T4, JIT_T4, (int64_t)disable);
        jit_bri(b, JIT_NE, JIT_T4, 0, 64, slow);
    }
    if (setf) {
        if (r1 >= 2) {                          /* fr_write() drops f0/f1 */
            mj_gr_read(g, r2, JIT_T0, JIT_T1);
            mj_fr_offset(g, r1);
            jit_st_envx(b, MOFF(fr) + FOFF(sig), JIT_T5, 1, JIT_T0, 8);
            jit_st_envxi(b, MOFF(fr) + FOFF(exp), JIT_T5, 1, 0x1003E, 4);
            /* sign = 0, nat = n2, padding = 0: the whole last word */
            jit_shli(b, JIT_T1, JIT_T1, 8 * (unsigned)(FOFF(nat) - FOFF(sign)));
            jit_st_envx(b, MOFF(fr) + FOFF(sign), JIT_T5, 1, JIT_T1, 4);
        }
    } else {
        if (r2 == 0) {
            jit_movi(b, JIT_T0, 0);
            jit_movi(b, JIT_T1, 0);
        } else if (r2 == 1) {
            jit_movi(b, JIT_T0, UINT64_C(0x8000000000000000));
            jit_movi(b, JIT_T1, 0);
        } else {
            mj_fr_offset(g, r2);
            jit_ld_envx(b, JIT_T0, MOFF(fr) + FOFF(sig), JIT_T5, 1, 8);
            jit_ld_envx(b, JIT_T1, MOFF(fr) + FOFF(nat), JIT_T5, 1, 1);
        }
        mj_gr_write(g, r1, JIT_T0, JIT_T1);
    }
    jit_jmp(b, done);

    jit_bind(b, slow);
    mj_gen_call(g, blk, i, false);
    jit_jmp(b, called);

    /* ran inline (or the predicate was false): finish as fast_slot() would */
    jit_bind(b, done);
    mj_end_skip(g, skip);
    mj_account(g, 1);
    jit_st_envi(b, MOFF(last_successful_bundle), (int64_t)bva, 8);
    jit_st_envi(b, MOFF(slot_stop_after), tb->stop[s], 1);
    jit_st_envi(b, MOFF(group_start), tb->stop[s], 1);
    jit_st_envi(b, MOFF(ip), (int64_t)mj_next_ip(js, tb), 8);
    jit_bind(b, called);
}

/* Emit the block's host code.  Returns NULL if the code buffer is full. */
static JitFn mj_codegen(MercedJit *j, MercedJitBlock *blk) {
    MJGen gen, *g = &gen;
    memset(g, 0, sizeof(*g));
    g->b = j->be;
    g->bn = (blk->key >> 63) & 1;
    uint64_t cfm = blk->key & MJ_CFM_KEY_MASK;
    g->sor8 = CFM_SOR(cfm) * 8;
    g->rrb_gr = CFM_RRB_GR(cfm);
    g->rrb_pr = CFM_RRB_PR(cfm);
    g->sof = CFM_SOF(cfm);
    g->dyn_rrb = !(blk->key & MJ_KEY_STATIC_RRB);
    JitBackend *b = g->b;

    jit_begin(b);
    for (unsigned i = 0; i < blk->n_slots; i++) {
        const MercedJitSlot *js = &blk->slots[i];
        const MercedTbEntry *tb = &blk->tbs[js->bundle];
        unsigned s = js->slot;
        uint64_t bva = js->ip & ~UINT64_C(0xF);
        g->last_stop = tb->stop[s];
        MJHow how = mj_classify(tb, s);
        unsigned oof_r = how == MJ_NOP || how == MJ_CALL ? 0
                       : mj_stacked_dest(tb, s);
        switch (how) {
        case MJ_NOP:
            j->stats.nop_slots++;
            g->pending++;
            break;

        case MJ_FPX:
            if (oof_r) mj_gen_oof_guard(g, blk, i, oof_r);
            mj_gen_sync(g, blk, i);
            j->stats.direct_slots++;
            mj_gen_fpx(g, blk, i);
            g->pending = 0;
            break;

        case MJ_CALL:
        case MJ_MEM:
            /* fast_slot() leaves the CPU fully up to date, and so does
             * mj_gen_mem()'s inline path */
            if (how == MJ_MEM && mj_direct_mem_ok(j, tb, s)) {
                if (oof_r) mj_gen_oof_guard(g, blk, i, oof_r);
                mj_gen_sync(g, blk, i);
                j->stats.direct_slots++;
                mj_gen_mem(g, j, blk, i);
            } else {
                mj_gen_sync(g, blk, i);
                j->stats.helper_slots++;
                mj_gen_call(g, blk, i, false);
            }
            g->pending = 0;
            break;

        case MJ_NATIVE:
            j->stats.native_slots++;
            if (oof_r) mj_gen_oof_guard(g, blk, i, oof_r);
            if (tb->kind[s] == SLOT_X) {
                mj_gen_movl(g, tb, s);
            } else switch (tb->op[s]) {
            case FOP_CMP: mj_gen_cmp(g, tb, s); break;
            case FOP_EXT: mj_gen_ext(g, tb, s); break;
            case FOP_BR_COND: {
                uint64_t target = bva + (uint64_t)(int64_t)tb->imm[s];
                JitLabel skip = mj_skip_unless_qp(g, tb->qpr[s]);
                mj_exit(g, i, g->pending + 1, bva, true, 1, target);
                mj_end_skip(g, skip);
                break;
            }
            case FOP_BR_CLOOP: {
                uint64_t target = bva + (uint64_t)(int64_t)tb->imm[s];
                JitLabel skip = jit_label(b);
                jit_ld_env(b, JIT_T4, MOFF(ar) + 8 * AR_LC, 8);
                jit_bri(b, JIT_EQ, JIT_T4, 0, 64, skip);
                jit_addi_env(b, MOFF(ar) + 8 * AR_LC, -1);
                mj_exit(g, i, g->pending + 1, bva, true, 1, target);
                jit_bind(b, skip);
                break;
            }
            default: mj_gen_alu(g, tb, s); break;
            }
            g->pending++;
            break;
        }
    }

    /* fell off the end of the block */
    {
        unsigned last = blk->n_slots - 1;
        const MercedJitSlot *js = &blk->slots[last];
        const MercedTbEntry *tb = &blk->tbs[js->bundle];
        MJHow how = mj_classify(tb, js->slot);
        bool native_last = how == MJ_NATIVE || how == MJ_NOP;
        if (g->pending || native_last)
            mj_exit(g, last, g->pending, js->ip & ~UINT64_C(0xF), true,
                    tb->stop[js->slot], mj_next_ip(js, tb));
        else
            mj_exit(g, last, 0, 0, false, 0, 0);
    }
    return jit_end(b);
}

/* ── translation ──────────────────────────────────────────────────────── */

static bool mj_sal_shim_bundle(uint64_t bva) {
    return (bva & UINT64_C(0x1FFFFFFFFFFFFFF0)) == UINT64_C(0x00000000FFF05000);
}

/* Fetch and decode the bundle at (va, pa) into tb, as merced_step() would. */
static void mj_fetch_bundle(Merced *m, MercedTbEntry *tb, uint64_t va,
                            uint64_t pa) {
    uint64_t lo = phys_fetch(m, pa, 8);
    uint64_t hi = phys_fetch(m, pa + 8, 8);
    memset(tb, 0, sizeof(*tb));
    tb->va = va;
    tb->pa = pa;
    tb->lo = lo;
    tb->hi = hi;
    tb->tmpl = (uint8_t)(lo & 0x1F);
    tb->slots[0] = (lo >> 5) & 0x1FFFFFFFFFFull;
    tb->slots[1] = ((lo >> 46) | (hi << 18)) & 0x1FFFFFFFFFFull;
    tb->slots[2] = (hi >> 23) & 0x1FFFFFFFFFFull;
    tb_decode(tb);
    tb->valid = true;
}

static MercedJitBlock *mj_translate(Merced *m, MercedJit *j, uint64_t ip,
                                   uint64_t pa_bundle, uint64_t key) {
    MercedTranslationCache *tc = m->translation_cache;
    MercedTbEntry tbs[MJ_MAX_BUNDLES];
    MercedJitSlot slots[MJ_MAX_SLOTS];
    unsigned nb = 0, ns = 0;
    uint64_t bva0 = ip & ~UINT64_C(0xF);
    uint64_t page_end = (bva0 | UINT64_C(0xFFF)) + 1;
    bool done = false;

    for (uint64_t bva = bva0; !done && nb < MJ_MAX_BUNDLES && bva < page_end;
         bva += 16) {
        if (mj_sal_shim_bundle(bva))
            break;
        MercedTbEntry *tb = &tbs[nb];
        mj_fetch_bundle(m, tb, bva, pa_bundle + (bva - bva0));
        if (tb->reserved)
            break;
        unsigned first = nb == 0 ? (unsigned)(ip & 0xF) : 0;
        if (first == 0 && tb->slow_slot0)
            break;
        for (unsigned s = first; s < 3; s++) {
            slots[ns].ip = bva | s;
            slots[ns].bundle = (uint8_t)nb;
            slots[ns].slot = (uint8_t)s;
            ns++;
            if (mj_ends_block(tb, s)) { done = true; break; }
            if (tb->kind[s] == SLOT_X) break;
        }
        nb++;
    }

    MercedJitBlock *blk = calloc(1, sizeof(*blk) + nb * sizeof(MercedTbEntry) +
                                    ns * sizeof(MercedJitSlot));
    if (!blk) return NULL;
    blk->tbs = (MercedTbEntry *)(blk + 1);
    blk->slots = (MercedJitSlot *)(blk->tbs + nb);
    memcpy(blk->tbs, tbs, nb * sizeof(MercedTbEntry));
    memcpy(blk->slots, slots, ns * sizeof(MercedJitSlot));
    blk->va = ip;
    blk->pa = pa_bundle;
    blk->key = key;
    blk->n_bundles = nb;
    blk->n_slots = ns;
    blk->tgen = tc->translation_generation;
    blk->code_gen = tc->page_generation[tb_page_index(pa_bundle)];

    if (ns) {
        size_t before = jit_backend_used(j->be);
        blk->code = mj_codegen(j, blk);
        j->stats.code_bytes += jit_backend_used(j->be) - before;
        if (!blk->code) {
            j->stats.flush_full++;
            /* out of code space: start over with an empty cache */
            free(blk);
            mj_flush(j);
            return NULL;
        }
    }
    j->stats.compiled++;
    if (j->comp_prof) {
        struct MjCompProf *c = &j->comp_prof[((ip >> 4) ^ (ip >> 20)) & 0xFFFF];
        if (c->ip != ip) { c->ip = ip; c->key = key; c->n = 0; c->keychanges = 0; }
        if (c->key != key) { c->keychanges++; c->key = key; }
        c->n++;
    }
    blk->all_next = j->all;
    j->all = blk;
    unsigned h = mj_hash(ip, key);
    blk->hash_next = j->hash[h];
    j->hash[h] = blk;
    return blk;
}

static void mj_unlink(MercedJit *j, MercedJitBlock *blk) {
    if (j->jmp_cache[mj_jmp_index(blk->va)] == blk)
        j->jmp_cache[mj_jmp_index(blk->va)] = NULL;
    MercedJitBlock **pp = &j->hash[mj_hash(blk->va, blk->key)];
    while (*pp && *pp != blk) pp = &(*pp)->hash_next;
    if (*pp) *pp = blk->hash_next;
    /* stays on j->all (its code stays in the buffer) until the next flush */
}

/* Are the bytes blk was translated from still in memory? */
static bool mj_bytes_match(Merced *m, const MercedJitBlock *blk) {
    for (unsigned i = 0; i < blk->n_bundles; i++) {
        const MercedTbEntry *tb = &blk->tbs[i];
        if (phys_fetch(m, tb->pa, 8) != tb->lo ||
            phys_fetch(m, tb->pa + 8, 8) != tb->hi)
            return false;
    }
    return true;
}

/* ── dispatch ─────────────────────────────────────────────────────────── */

typedef enum {
    MJ_INTERPRET,           /* run this slot through fast_step() */
    MJ_RUN,                 /* run *out */
    MJ_FETCH_FAULT,         /* the fetch translation faulted: *st, 1 slot */
} MJEnter;

static MJEnter mj_enter(Merced *m, MercedTranslationCache *tc, MercedJit *j,
                        unsigned budget, uint64_t stop_lo, uint64_t stop_hi,
                        MercedJitBlock **out, MercedStatus *st) {
    uint64_t ip = m->ip;
    uint64_t bva = ip & ~UINT64_C(0xF);
    if (m->psr & PSR_IS) { j->stats.why_is++; return MJ_INTERPRET; }
    if (mj_irq_blocks(m)) { j->stats.why_irq++; return MJ_INTERPRET; }
    if (mj_extras(m)) {
        j->stats.why_extras++;
        if (mj_stats_on) {
            static const uint64_t bit[6] = { PSR_IS, PSR_DB, PSR_ONE_SHOT,
                                             PSR_LP, PSR_TB, PSR_SS };
            for (int k = 0; k < 6; k++)
                if (m->psr & bit[k]) j->stats.why_x[k]++;
            if (pmu_count_on) j->stats.why_x[6]++;
            if (m->cache.observe) j->stats.why_x[7]++;
            if (fetch_uncached(m)) j->stats.why_x[8]++;
        }
        return MJ_INTERPRET;
    }
    if ((ip & 0xF) > 2 || mj_sal_shim_bundle(bva))
        return MJ_INTERPRET;

    uint64_t key = mj_key(m);
    MercedJitBlock **jc = &j->jmp_cache[mj_jmp_index(ip)];
    MercedJitBlock *blk = *jc;
    if (!blk || blk->va != ip || blk->key != key) {
        blk = j->hash[mj_hash(ip, key)];
        while (blk && !(blk->va == ip && blk->key == key))
            blk = blk->hash_next;
        if (blk) *jc = blk;
    }
    if (blk && blk->n_slots &&
        (blk->n_slots > budget || mj_timer_in_window(m, blk->n_slots)))
        return MJ_INTERPRET;

    /* Instruction-fetch translation, as merced_step() would find it. */
    if (!blk || blk->tgen != tc->translation_generation) {
        uint64_t pa;
        const MercedTbEntry *tbe = &tc->entry[tb_index(bva)];
        if (m->bundle_cache_valid && m->bundle_cache_translation_valid &&
            m->bundle_cache_va == bva) {
            pa = m->bundle_cache_pa;
        } else if (tbe->valid && tbe->va == bva &&
                   tbe->translation_generation == tc->translation_generation &&
                   tbe->code_generation ==
                       tc->page_generation[tb_page_index(tbe->pa)]) {
            pa = tbe->pa;
        } else if (!(m->psr & PSR_IT)) {
            pa = bva & MERCED_PHYS_MASK;
        } else if (!va_translate(m, bva, true, false, ISR_X, &pa, st)) {
            return MJ_FETCH_FAULT;
        }
        if (blk) {
            j->stats.retranslate++;
            if (blk->pa == pa) {
                blk->tgen = tc->translation_generation;
            } else {
                mj_unlink(j, blk);
                blk = NULL;
                j->stats.comp_pa++;
            }
        } else {
            j->stats.comp_new++;
            if (ip & 0xF) j->stats.mid_entry++;
        }
        if (!blk) {
            blk = mj_translate(m, j, ip, pa, key);
            if (!blk) return MJ_INTERPRET;
        }
    }

    /* Code bytes */
    uint32_t cg = tc->page_generation[tb_page_index(blk->pa)];
    if (blk->code_gen != cg) {
        if (mj_bytes_match(m, blk)) {
            blk->code_gen = cg;
            j->stats.reverify_ok++;
        } else {
            j->stats.reverify_fail++;
            j->stats.comp_bytes++;
            uint64_t pa = blk->pa;
            mj_unlink(j, blk);
            blk = mj_translate(m, j, ip, pa, key);
            if (!blk) return MJ_INTERPRET;
        }
    }

    if (!blk->n_slots) { j->stats.why_empty++; return MJ_INTERPRET; }
    if (blk->n_slots > budget || mj_timer_in_window(m, blk->n_slots)) {
        j->stats.why_budget++;
        return MJ_INTERPRET;
    }
    if (stop_hi > stop_lo) {
        uint64_t page = bva & ~UINT64_C(0xFFF);
        if (page < stop_hi && page + 0x1000 > stop_lo)
            return MJ_INTERPRET;
    }
    *out = blk;
    return MJ_RUN;
}

static MercedStatus mj_run(Merced *m, MercedTranslationCache *tc,
                           unsigned max_slots, unsigned *executed,
                           uint64_t stop_mask, uint64_t stop_value,
                           uint64_t stop_lo, uint64_t stop_hi) {
    MercedJit *j = tc->jit;
    unsigned n = 0;
    MercedStatus st = MERCED_OK;
    tc->exit_request = false;
    while (n < max_slots) {
        MercedJitBlock *blk = NULL;
        MJEnter how = stop_mask ? MJ_INTERPRET
                                : mj_enter(m, tc, j, max_slots - n, stop_lo,
                                           stop_hi, &blk, &st);
        if (how == MJ_RUN) {
            j->status = MERCED_OK;
            j->in_block = true;
            unsigned k = (unsigned)blk->code(m);
            j->in_block = false;
            if (j->flush_pending)
                mj_flush(j);
            st = j->status;
            if (k == 0) {           /* its first slot is the interpreter's */
                st = fast_step(m, tc);
                k = 1;
            }
            n += k;
            j->stats.blocks_run++;
            j->stats.slots_jit += k;
        } else if (how == MJ_FETCH_FAULT) {
            n++;
        } else {
            st = fast_step(m, tc);
            n++;
            j->stats.slots_interp++;
        }
        if (st != MERCED_OK || tc->exit_request ||
            (stop_mask && (m->ip & stop_mask) == stop_value) ||
            (m->ip >= stop_lo && m->ip < stop_hi))
            break;
    }
    if (executed)
        *executed = n;
    return st;
}

/* ── public ───────────────────────────────────────────────────────────── */

static void mj_print_stats(void) {
    MercedJit *j = mj_stats_jit;
    if (!j) return;
    uint64_t slots = j->stats.slots_jit + j->stats.slots_interp;
    uint64_t tr = j->stats.native_slots + j->stats.helper_slots +
                  j->stats.nop_slots + j->stats.direct_slots;
    fprintf(stderr,
            "merced-jit: blocks compiled=%" PRIu64 " run=%" PRIu64
            " flushes=%" PRIu64 "\n"
            "merced-jit: slots run translated=%" PRIu64 " (%.1f%%)"
            " interpreted=%" PRIu64 " helper calls=%" PRIu64 "\n"
            "merced-jit: translated slots native=%" PRIu64 " nop=%" PRIu64
            " direct=%" PRIu64 " helper=%" PRIu64 " (%.1f%% helper)\n"
            "merced-jit: re-checks bytes ok=%" PRIu64 " changed=%" PRIu64
            " fetch translations=%" PRIu64 "\n",
            j->stats.compiled, j->stats.blocks_run, j->stats.flushes,
            j->stats.slots_jit, slots ? 100.0 * j->stats.slots_jit / slots : 0.0,
            j->stats.slots_interp, j->stats.helper_calls,
            j->stats.native_slots, j->stats.nop_slots, j->stats.direct_slots,
            j->stats.helper_slots,
            tr ? 100.0 * j->stats.helper_slots / tr : 0.0,
            j->stats.reverify_ok, j->stats.reverify_fail,
            j->stats.retranslate);
    fprintf(stderr, "merced-jit: interpreted because IA-32=%" PRIu64
            " interrupt pending=%" PRIu64 " PSR/PMU/cache model=%" PRIu64
            " budget/timer=%" PRIu64 " untranslatable=%" PRIu64 "\n",
            j->stats.why_is, j->stats.why_irq, j->stats.why_extras,
            j->stats.why_budget, j->stats.why_empty);
    fprintf(stderr, "merced-jit: compiles: no block=%" PRIu64 " (entering mid-bundle %"
            PRIu64 ") physical page moved=%" PRIu64 " bytes changed=%" PRIu64 "\n",
            j->stats.comp_new, j->stats.mid_entry, j->stats.comp_pa,
            j->stats.comp_bytes);
    fprintf(stderr, "merced-jit: flushes for a full code buffer=%" PRIu64
            " code bytes/block=%.0f\n", j->stats.flush_full,
            j->stats.compiled ? (double)j->stats.code_bytes / j->stats.compiled : 0.0);
    fprintf(stderr, "merced-jit: memory slow path=%" PRIu64 " fills=%" PRIu64
            " not cacheable=%" PRIu64 " not RAM=%" PRIu64 "\n",
            j->stats.mem_slow, j->stats.mem_fill, j->stats.mem_nofill_xlate,
            j->stats.mem_nofill_page);
    fprintf(stderr, "merced-jit: extras: IS=%" PRIu64 " DB=%" PRIu64 " one-shot=%" PRIu64
            " LP=%" PRIu64 " TB=%" PRIu64 " SS=%" PRIu64 " PMU=%" PRIu64
            " observe=%" PRIu64 " fetch-uncached=%" PRIu64 "\n",
            j->stats.why_x[0], j->stats.why_x[1], j->stats.why_x[2], j->stats.why_x[3],
            j->stats.why_x[4], j->stats.why_x[5], j->stats.why_x[6], j->stats.why_x[7],
            j->stats.why_x[8]);
    if (j->comp_prof) {
        for (int k = 0; k < 8; k++) {
            int best = -1;
            for (int i = 0; i < 65536; i++)
                if (j->comp_prof[i].n && (best < 0 || j->comp_prof[i].n > j->comp_prof[best].n))
                    best = i;
            if (best < 0) break;
            fprintf(stderr, "merced-jit: compiled %u times (key changed %u): ip=%016" PRIX64
                    " key=%016" PRIX64 "\n", j->comp_prof[best].n,
                    j->comp_prof[best].keychanges, j->comp_prof[best].ip, j->comp_prof[best].key);
            j->comp_prof[best].n = 0;
        }
    }
    if (!j->helper_profile) return;
    fprintf(stderr, "merced-jit: top helper-executed encodings "
            "(unit major x6 [fast_op?]):\n");
    for (int k = 0; k < 25; k++) {
        unsigned best = 0;
        for (unsigned i = 1; i < 5u << 10; i++)
            if (j->helper_profile[i] > j->helper_profile[best]) best = i;
        if (!j->helper_profile[best]) break;
        fprintf(stderr, "merced-jit:   %c maj=%X x6=%02X  %12" PRIu64
                " (%.1f%%)\n", "MIBFX"[best >> 10], (best >> 6) & 0xF,
                best & 0x3F, j->helper_profile[best],
                100.0 * j->helper_profile[best] / j->stats.helper_calls);
        j->helper_profile[best] = 0;
    }
}

MercedAccel merced_set_accel(Merced *m, MercedAccel accel) {
    MercedTranslationCache *tc = m->translation_cache;
    if (!tc) return MERCED_ACCEL_INTERPRETER;
    if (accel != MERCED_ACCEL_JIT) {
        if (tc->jit) tc->jit->enabled = false;
        return MERCED_ACCEL_INTERPRETER;
    }
    if (!tc->jit) {
        MercedJit *j = calloc(1, sizeof(*j));
        if (!j) return MERCED_ACCEL_INTERPRETER;
        j->be = jit_backend_new(MJ_CODE_SIZE);
        if (!j->be) {
            free(j);
            fprintf(stderr, "merced: no JIT backend for this host (%s), or no "
                    "executable memory; using the interpreter\n",
                    jit_backend_name());
            return MERCED_ACCEL_INTERPRETER;
        }
        j->tc = tc;
        tc->jit = j;
        if (getenv("MERCED_JIT_STATS") && !mj_stats_jit) {
            mj_stats_on = true;
            static bool registered;
            mj_stats_jit = j;
            j->helper_profile = calloc(5u << 10, sizeof(uint64_t));
            j->comp_prof = calloc(65536, sizeof(*j->comp_prof));
            if (!registered) atexit(mj_print_stats);
            registered = true;
        }
    }
    tc->jit->enabled = true;
    return MERCED_ACCEL_JIT;
}

MercedAccel merced_get_accel(const Merced *m) {
    const MercedTranslationCache *tc = m->translation_cache;
    return tc && tc->jit && tc->jit->enabled ? MERCED_ACCEL_JIT
                                              : MERCED_ACCEL_INTERPRETER;
}

void merced_set_direct_ram(Merced *m, MercedDirectPageFn page) {
    MercedJit *j = mj_of(m);
    if (!j) return;
    j->direct_page = page;
    mj_htlb_flush(j);
    /* blocks already translated keep using the bus until flushed */
}

void merced_direct_ram_changed(Merced *m) {
    MercedJit *j = mj_of(m);
    if (j) mj_htlb_flush(j);
}
