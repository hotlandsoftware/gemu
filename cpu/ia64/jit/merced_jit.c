/*
 * Merced dynamic binary translator (-accel jit): IA-64 -> x86-64.
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
 *   - native:  inline x86-64 for the common integer ops (A-unit ALU and
 *              compares, zxt/sxt, movl) and IP-relative br.cond/br.cloop;
 *   - helper:  a call to mj_helper(), which runs the interpreter's own
 *              handler for the slot with exactly fast_step()'s bookkeeping.
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
 *     slots might, so after each one mj_helper() re-checks them (plus the
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

#include "gemu/jit_x64.h"

#define MJ_MAX_BUNDLES 16
#define MJ_MAX_SLOTS   (MJ_MAX_BUNDLES * 3)
#define MJ_CODE_SIZE   (64u << 20)
#define MJ_HASH_BITS   15
#define MJ_HASH_SIZE   (1u << MJ_HASH_BITS)
#define MJ_MAX_EXITS   (MJ_MAX_SLOTS * 2 + 4)

typedef struct MercedJitBlock MercedJitBlock;
typedef unsigned (*MercedJitFn)(Merced *m, MercedJitBlock *b);

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
    MercedJitFn code;
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
    uint8_t *code_mem;
    X64Buf buf;
    MercedJitBlock *hash[MJ_HASH_SIZE];
    MercedJitBlock *all;
    MercedStatus status;    /* set by mj_helper() when a slot halts */
    /* A flush requested while a block runs (a helper slot can reach
     * tb_flush()) frees the blocks once it has returned. */
    bool in_block, flush_pending;
    struct {
        uint64_t compiled, flushes, blocks_run, slots_jit, slots_interp;
        uint64_t helper_calls, native_slots, helper_slots, nop_slots;
        uint64_t direct_slots, mem_slow, mem_fill, mem_nofill_xlate,
                 mem_nofill_page;
        uint64_t reverify_ok, reverify_fail, retranslate;
    } stats;
    /* MERCED_JIT_STATS: helper calls by unit/major/x6, to show what to
     * translate natively next. */
    uint64_t *helper_profile;
} MercedJit;

/* CFM fields that select GR/PR physical locations: sor, rrb.gr, rrb.pr
 * (rrb.fr only matters to FP handlers, which read CFM themselves). */
#define MJ_CFM_KEY_MASK ((UINT64_C(0xF) << 14) | (UINT64_C(0x7F) << 18) | \
                         (UINT64_C(0x3F) << 32))

static inline uint64_t mj_key(const Merced *m) {
    return (m->cfm & MJ_CFM_KEY_MASK) |
           ((m->psr & PSR_BN) ? UINT64_C(1) << 63 : 0);
}

static inline unsigned mj_hash(uint64_t ip, uint64_t key) {
    return (unsigned)(((ip >> 4) ^ (ip >> 19) ^ (key >> 14) ^ (key >> 51)) &
                      (MJ_HASH_SIZE - 1));
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
    if (j->code_mem)
        x64_init(&j->buf, j->code_mem, MJ_CODE_SIZE);
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
    gemu_jit_free_exec(j->code_mem, MJ_CODE_SIZE);
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

/* ── helper: one interpreted slot inside a translated block ───────────── */

/* Runs block slot `i` through the interpreter's handler with fast_step()'s
 * per-slot sequence.  Returns 0 to continue with the next slot, or 1 to
 * leave the block (the slot halted, redirected IP, or changed something the
 * rest of the block was translated or admitted under). */
static unsigned mj_helper(Merced *m, MercedJitBlock *b, unsigned i) {
    MercedTranslationCache *tc = m->translation_cache;
    MercedJit *j = tc->jit;
    const MercedJitSlot *js = &b->slots[i];
    const MercedTbEntry *tb = &b->tbs[js->bundle];
    unsigned slot = js->slot;
    uint64_t bundle_va = js->ip & ~UINT64_C(0xF);
    unsigned kind = tb->kind[slot];

    j->stats.helper_calls++;
    if (j->helper_profile) {
        uint64_t raw = kind == SLOT_X ? tb->slots[2] : tb->slots[slot];
        unsigned u = kind == SLOT_X ? 4 :
                     (unsigned)(strchr("MIBF", bundle_units[tb->tmpl][slot]) - "MIBF");
        unsigned x6 = (unsigned)(u == 0 ? bits(raw, 30, 6) : bits(raw, 27, 6));
        j->helper_profile[(u << 10) | ((unsigned)bits(raw, 37, 4) << 6) | x6]++;
    }
    m->ip = js->ip;
    m->slot_stop_after = tb->stop[slot];
    int qp = pr_read(m, tb->qpr[slot]);
    m->taken = 0;
    uint64_t faults_before = m->nfaults;

    MercedStatus st;
    if (kind == SLOT_EXEC)
        st = tb->op[slot] == FOP_GENERIC ?
             tb->fn[slot](m, tb->slots[slot], qp) : fast_op(m, tb, slot, qp);
    else
        st = exec_x(m, tb->slots[2], tb->slots[1], qp);
    if (st != MERCED_OK) {
        if (st == MERCED_HALT_UNIMPL) {
            char unit = kind == SLOT_X ? 'X' : bundle_units[tb->tmpl][slot];
            uint64_t raw = kind == SLOT_X ? tb->slots[2] : tb->slots[slot];
            size_t len = strlen(m->halt_msg);
            snprintf(m->halt_msg + len, sizeof(m->halt_msg) - len,
                     " [%c-slot %u tmpl %s raw=0x%011" PRIX64 "]",
                     unit, slot, bundle_units[tb->tmpl], raw);
        }
        j->status = st;
        return 1;
    }

    m->ninsts++;
    if (!m->external_itc)
        itc_advance(m, m->slot_cycles);
    if (m->nfaults == faults_before)
        m->last_successful_bundle = bundle_va;
    if (m->taken) {
        m->group_start = 1;
        m->taken = 0;
        return 1;
    }
    m->group_start = m->slot_stop_after;
    if (kind == SLOT_X || slot == 2)
        m->ip = bundle_va + 16;
    else
        m->ip = bundle_va | (slot + 1);

    /* Is the rest of the block still admissible? */
    if (tc->exit_request || j->flush_pending || (m->psr & PSR_IS) ||
        mj_key(m) != b->key || fetch_observed(m) ||
        tc->translation_generation != b->tgen ||
        tc->page_generation[tb_page_index(b->pa)] != b->code_gen ||
        mj_irq_blocks(m) ||
        mj_timer_in_window(m, b->n_slots - (i + 1)))
        return 1;
    return 0;
}

/* fast_op() for a direct call (it's inlined into fast_step()). */
static MercedStatus mj_fast_op(Merced *m, const MercedTbEntry *tb,
                               unsigned s, int qp) {
    return fast_op(m, tb, s, qp);
}

/* A directly called slot stopped the CPU: finish as fast_step() would. */
static void mj_halt(Merced *m, MercedJitBlock *b, unsigned i,
                    MercedStatus st) {
    MercedJit *j = mj_of(m);
    const MercedJitSlot *js = &b->slots[i];
    const MercedTbEntry *tb = &b->tbs[js->bundle];
    if (st == MERCED_HALT_UNIMPL) {
        bool x = tb->kind[js->slot] == SLOT_X;
        char unit = x ? 'X' : bundle_units[tb->tmpl][js->slot];
        uint64_t raw = x ? tb->slots[2] : tb->slots[js->slot];
        size_t len = strlen(m->halt_msg);
        snprintf(m->halt_msg + len, sizeof(m->halt_msg) - len,
                 " [%c-slot %u tmpl %s raw=0x%011" PRIX64 "]",
                 unit, js->slot, bundle_units[tb->tmpl], raw);
    }
    j->status = st;
}

/* After a memory reference: may the block go on?  (mj_helper()'s checks,
 * minus those only system instructions can affect.) */
static unsigned mj_mem_guard(Merced *m, MercedJitBlock *b) {
    MercedTranslationCache *tc = m->translation_cache;
    return tc->exit_request ||
           tc->page_generation[tb_page_index(b->pa)] != b->code_gen ||
           mj_irq_blocks(m);
}

/* ── code generation ──────────────────────────────────────────────────── */

#define MOFF(field) ((int32_t)offsetof(Merced, field))

typedef struct {
    X64Buf *b;
    bool bn;
    unsigned sor8, rrb_gr, rrb_pr;
    unsigned pending;           /* native slots not yet charged to ninsts */
    size_t exits[MJ_MAX_EXITS]; /* jmp rel32 fields to patch to the epilogue */
    unsigned n_exits;
    bool too_many_exits;
    uint8_t last_stop;          /* stop bit of the slot being translated */
} MJGen;

static void mj_jmp_epilogue(MJGen *g) {
    size_t at = x64_jmp(g->b);
    if (g->n_exits < MJ_MAX_EXITS) g->exits[g->n_exits++] = at;
    else g->too_many_exits = true;
}

static unsigned mj_pr_bit(const MJGen *g, unsigned p) {
    if (p < 16) return p;
    return 16 + ((p - 16 + g->rrb_pr) % 48);
}

static unsigned mj_stacked_index(const MJGen *g, unsigned r) {
    unsigned idx = r - 32;
    if (g->sor8 && idx < g->sor8)
        idx = (idx + g->rrb_gr) % g->sor8;
    return idx;
}

/* R10 = physical gr_stack index of stacked register r. */
static void mj_stacked_phys(MJGen *g, unsigned r) {
    x64_load32(g->b, X64_R10, X64_RBX, X64_NOREG, 1, MOFF(bof));
    x64_alu_ri32(g->b, X64_ADD, X64_R10, (int32_t)mj_stacked_index(g, r));
    x64_alu_ri32(g->b, X64_AND, X64_R10, MERCED_RSE_CAPACITY - 1);
}

/* val = GR[r]; nat (if >= 0) = its NaT bit.  Clobbers R10. */
static void mj_gr_read(MJGen *g, unsigned r, int val, int nat) {
    X64Buf *b = g->b;
    if (r == 0) {
        x64_mov_imm(b, val, 0);
        if (nat >= 0) x64_mov_imm(b, nat, 0);
    } else if (r < 16 || (r < 32 && g->bn)) {
        x64_load64(b, val, X64_RBX, X64_NOREG, 1, MOFF(gr_static) + 8 * (int32_t)r);
        if (nat >= 0)
            x64_load8zx(b, nat, X64_RBX, X64_NOREG, 1, MOFF(nat_static) + (int32_t)r);
    } else if (r < 32) {
        x64_load64(b, val, X64_RBX, X64_NOREG, 1, MOFF(gr_bank0) + 8 * (int32_t)(r - 16));
        if (nat >= 0)
            x64_load8zx(b, nat, X64_RBX, X64_NOREG, 1, MOFF(nat_bank0) + (int32_t)(r - 16));
    } else {
        mj_stacked_phys(g, r);
        x64_load64(b, val, X64_RBX, X64_R10, 8, MOFF(gr_stack));
        if (nat >= 0)
            x64_load8zx(b, nat, X64_RBX, X64_R10, 1, MOFF(nat_stack));
    }
}

/* GR[r] = val, NaT = nat (a byte register, or -1 for 0).  gr_write_inl()'s
 * sequence, including the RSE dirty-boundary update.  Clobbers R10, R11. */
static void mj_gr_write(MJGen *g, unsigned r, int val, int nat) {
    X64Buf *b = g->b;
    int32_t voff, noff;
    int index = X64_NOREG;
    if (r == 0)
        return;
    if (r < 16 || (r < 32 && g->bn)) {
        voff = MOFF(gr_static) + 8 * (int32_t)r;
        noff = MOFF(nat_static) + (int32_t)r;
    } else if (r < 32) {
        voff = MOFF(gr_bank0) + 8 * (int32_t)(r - 16);
        noff = MOFF(nat_bank0) + (int32_t)(r - 16);
    } else {
        mj_stacked_phys(g, r);
        index = X64_R10;
        voff = MOFF(gr_stack);
        noff = MOFF(nat_stack);
    }
    x64_store64(b, val, X64_RBX, index, 8, voff);
    if (nat >= 0) x64_store8(b, nat, X64_RBX, index, 1, noff);
    else          x64_store8_imm(b, X64_RBX, index, 1, noff, 0);
    if (r >= 32) {
        /* if (bof_total + logical < rse_flushed_regs) rse_flushed_regs = ...; */
        x64_load64(b, X64_R11, X64_RBX, X64_NOREG, 1, MOFF(bof_total));
        x64_alu_ri(b, X64_ADD, X64_R11, (int32_t)mj_stacked_index(g, r));
        x64_alu_rm(b, X64_CMP, X64_R11, X64_RBX, MOFF(rse_flushed_regs));
        size_t skip = x64_jcc(b, X64_CC_GE);
        x64_store64(b, X64_R11, X64_RBX, X64_NOREG, 1, MOFF(rse_flushed_regs));
        x64_here(b, skip);
    }
}

/* Branch past the slot when its qualifying predicate is false.  Returns the
 * rel32 to patch, or SIZE_MAX for p0. */
static size_t mj_skip_unless_qp(MJGen *g, unsigned qp) {
    if (qp == 0) return SIZE_MAX;
    x64_bt_mi(g->b, 4, X64_RBX, MOFF(pr), (uint8_t)mj_pr_bit(g, qp));
    return x64_jcc(g->b, X64_CC_AE);            /* CF = 0: predicate false */
}

static void mj_end_skip(MJGen *g, size_t skip) {
    if (skip != SIZE_MAX) x64_here(g->b, skip);
}

/* PR[p] = value register `src` (0/1), optionally inverted.  Clobbers RDX. */
static void mj_pr_write_reg(MJGen *g, unsigned p, int src, bool invert) {
    if (p == 0) return;
    unsigned bit = mj_pr_bit(g, p);
    x64_bt_mi(g->b, 6, X64_RBX, MOFF(pr), (uint8_t)bit);            /* btr */
    x64_mov_rr32(g->b, X64_RDX, src);
    if (invert) x64_alu_ri32(g->b, X64_XOR, X64_RDX, 1);
    if (bit) x64_shl_ri(g->b, X64_RDX, (uint8_t)bit);
    x64_alu_mr(g->b, X64_OR, X64_RBX, MOFF(pr), X64_RDX);
}

static void mj_pr_clear(MJGen *g, unsigned p) {
    if (p == 0) return;
    x64_bt_mi(g->b, 6, X64_RBX, MOFF(pr), (uint8_t)mj_pr_bit(g, p));
}

/* Charge `k` slots executed natively since the last sync point. */
static void mj_account(MJGen *g, unsigned k) {
    X64Buf *b = g->b;
    if (!k) return;
    x64_alu_mi(b, X64_ADD, X64_RBX, MOFF(ninsts), (int32_t)k);
    x64_cmp8_mi(b, X64_RBX, MOFF(external_itc), 0);
    size_t skip = x64_jcc(b, X64_CC_NE);
    x64_load32(b, X64_RAX, X64_RBX, X64_NOREG, 1, MOFF(slot_cycles));
    if (k != 1) x64_imul_rri(b, X64_RAX, X64_RAX, (int32_t)k);
    x64_alu_mr(b, X64_ADD, X64_RBX, MOFF(ar) + 8 * AR_ITC, X64_RAX);
    x64_here(b, skip);
}

static void mj_store_imm64(MJGen *g, int32_t off, uint64_t v) {
    if ((int64_t)v == (int32_t)v) {
        x64_store64_imm32(g->b, X64_RBX, X64_NOREG, 1, off, (int32_t)v);
    } else {
        x64_mov_imm(g->b, X64_RAX, v);
        x64_store64(g->b, X64_RAX, X64_RBX, X64_NOREG, 1, off);
    }
}

/* Leave the block after slot index `idx`, which executed natively (or after
 * a helper slot when native_last is false): charge pending slots, set the
 * successor state merced_step() would, return idx + 1 slots executed. */
static void mj_exit(MJGen *g, unsigned idx, unsigned pending,
                    uint64_t last_bundle, bool native_last,
                    uint8_t group_start, uint64_t next_ip) {
    mj_account(g, pending);
    if (native_last) {
        /* merced_step() leaves the last slot's stop bit here */
        x64_store8_imm(g->b, X64_RBX, X64_NOREG, 1, MOFF(slot_stop_after),
                       g->last_stop);
        mj_store_imm64(g, MOFF(last_successful_bundle), last_bundle);
        x64_store8_imm(g->b, X64_RBX, X64_NOREG, 1, MOFF(group_start), group_start);
        mj_store_imm64(g, MOFF(ip), next_ip);
    }
    x64_mov_imm(g->b, X64_RAX, idx + 1);
    mj_jmp_epilogue(g);
}

/* Native forms.  Each returns false if it doesn't handle the slot. */

static void mj_gen_alu(MJGen *g, const MercedTbEntry *tb, unsigned s) {
    X64Buf *b = g->b;
    unsigned op = tb->op[s];
    int64_t imm = tb->imm[s];
    size_t skip = mj_skip_unless_qp(g, tb->qpr[s]);
    mj_gr_read(g, tb->a3[s], X64_RAX, X64_RCX);              /* b = r3 */
    switch (op) {
    case FOP_ADD: case FOP_SUB: case FOP_AND: case FOP_ANDCM:
    case FOP_OR: case FOP_XOR: case FOP_SHLADD:
        mj_gr_read(g, tb->a2[s], X64_RDX, X64_R8);           /* a = r2 */
        switch (op) {
        case FOP_ADD:
            x64_alu_rr(b, X64_ADD, X64_RAX, X64_RDX);
            if (imm) x64_alu_ri(b, X64_ADD, X64_RAX, (int32_t)imm);
            break;
        case FOP_SUB:
            x64_alu_rr(b, X64_SUB, X64_RDX, X64_RAX);
            if (imm) x64_alu_ri(b, X64_SUB, X64_RDX, (int32_t)imm);
            x64_mov_rr(b, X64_RAX, X64_RDX);
            break;
        case FOP_AND:   x64_alu_rr(b, X64_AND, X64_RAX, X64_RDX); break;
        case FOP_ANDCM: x64_not(b, X64_RAX);
                        x64_alu_rr(b, X64_AND, X64_RAX, X64_RDX); break;
        case FOP_OR:    x64_alu_rr(b, X64_OR,  X64_RAX, X64_RDX); break;
        case FOP_XOR:   x64_alu_rr(b, X64_XOR, X64_RAX, X64_RDX); break;
        default:        /* SHLADD */
            x64_shl_ri(b, X64_RDX, (uint8_t)imm);
            x64_alu_rr(b, X64_ADD, X64_RAX, X64_RDX);
            break;
        }
        x64_alu_rr32(b, X64_OR, X64_RCX, X64_R8);
        break;
    default:
        switch (op) {
        case FOP_ADDI:   x64_alu_ri(b, X64_ADD, X64_RAX, (int32_t)imm); break;
        case FOP_SUBI:   x64_neg(b, X64_RAX);
                         x64_alu_ri(b, X64_ADD, X64_RAX, (int32_t)imm); break;
        case FOP_ANDI:   x64_alu_ri(b, X64_AND, X64_RAX, (int32_t)imm); break;
        case FOP_ANDCMI: x64_not(b, X64_RAX);
                         x64_alu_ri(b, X64_AND, X64_RAX, (int32_t)imm); break;
        case FOP_ORI:    x64_alu_ri(b, X64_OR,  X64_RAX, (int32_t)imm); break;
        default:         x64_alu_ri(b, X64_XOR, X64_RAX, (int32_t)imm); break;
        }
        break;
    }
    mj_gr_write(g, tb->a1[s], X64_RAX, X64_RCX);
    mj_end_skip(g, skip);
}

static void mj_gen_ext(MJGen *g, const MercedTbEntry *tb, unsigned s) {
    X64Buf *b = g->b;
    size_t skip = mj_skip_unless_qp(g, tb->qpr[s]);
    mj_gr_read(g, tb->a3[s], X64_RAX, X64_RCX);
    switch (tb->x[s]) {
    case 0x10: x64_movzx8(b, X64_RAX, X64_RAX); break;
    case 0x11: x64_movzx16(b, X64_RAX, X64_RAX); break;
    case 0x12: x64_mov_rr32(b, X64_RAX, X64_RAX); break;
    case 0x14: x64_movsx8(b, X64_RAX, X64_RAX); break;
    case 0x15: x64_movsx16(b, X64_RAX, X64_RAX); break;
    default:   x64_movsxd(b, X64_RAX, X64_RAX); break;
    }
    mj_gr_write(g, tb->a1[s], X64_RAX, X64_RCX);
    mj_end_skip(g, skip);
}

/* fast_op()'s FOP_CMP: runs whatever the predicate (.unc clears both
 * targets when it's false). */
static void mj_gen_cmp(MJGen *g, const MercedTbEntry *tb, unsigned s) {
    X64Buf *b = g->b;
    unsigned x = tb->x[s], p1 = tb->a1[s], p2 = tb->a2[s];
    unsigned qpr = tb->qpr[s];
    bool unc = (x & CMPF_UNC) != 0;

    if (qpr) {                                  /* R9 = qp */
        x64_bt_mi(b, 4, X64_RBX, MOFF(pr), (uint8_t)mj_pr_bit(g, qpr));
        x64_setcc(b, X64_CC_B, X64_R9);
    }
    mj_gr_read(g, tb->a3[s], X64_RAX, X64_RCX);                 /* b */
    if (x & CMPF_IMM) {
        x64_mov_imm(b, X64_RDX, (uint64_t)(int64_t)tb->imm[s]); /* a */
    } else {
        mj_gr_read(g, (unsigned)tb->imm[s], X64_RDX, X64_R8);
        x64_alu_rr32(b, X64_OR, X64_RCX, X64_R8);
    }
    if (x & CMPF_CMP4) x64_alu_rr32(b, X64_CMP, X64_RDX, X64_RAX);
    else               x64_alu_rr(b, X64_CMP, X64_RDX, X64_RAX);
    int cc = (x & CMPF_REL) == CMPF_LT ? X64_CC_L :
             (x & CMPF_REL) == CMPF_LTU ? X64_CC_B : X64_CC_E;
    x64_setcc(b, cc, X64_R11);
    x64_movzx8(b, X64_R11, X64_R11);

    size_t qp_false = SIZE_MAX;
    if (qpr) {
        x64_test_rr8(b, X64_R9, X64_R9);        /* setcc wrote only R9B */
        qp_false = x64_jcc(b, X64_CC_E);
    }
    x64_test_rr32(b, X64_RCX, X64_RCX);
    size_t nat = x64_jcc(b, X64_CC_NE);
    mj_pr_write_reg(g, p1, X64_R11, false);
    mj_pr_write_reg(g, p2, X64_R11, true);
    size_t done = x64_jmp(b);
    /* NaT source with a true predicate clears both; so does a false
     * predicate for .unc.  A false predicate on a normal compare: nothing. */
    x64_here(b, nat);
    if (unc && qp_false != SIZE_MAX) x64_here(b, qp_false);
    mj_pr_clear(g, p1);
    mj_pr_clear(g, p2);
    x64_here(b, done);
    if (!unc && qp_false != SIZE_MAX) x64_here(b, qp_false);
}

static void mj_gen_movl(MJGen *g, const MercedTbEntry *tb, unsigned s) {
    uint64_t raw = tb->slots[2], lraw = tb->slots[1];
    uint64_t imm = (bits(raw, 36, 1) << 63) | (lraw << 22) |
                   (bits(raw, 21, 1) << 21) | (bits(raw, 22, 5) << 16) |
                   (bits(raw, 27, 9) << 7) | bits(raw, 13, 7);
    size_t skip = mj_skip_unless_qp(g, tb->qpr[s]);
    x64_mov_imm(g->b, X64_RAX, imm);
    mj_gr_write(g, (unsigned)bits(raw, 6, 7), X64_RAX, -1);
    mj_end_skip(g, skip);
}

/* How a slot is translated:
 *   MJ_NOP      nothing;
 *   MJ_NATIVE   inline host code;
 *   MJ_DIRECT   inline bookkeeping around a direct call of the interpreter
 *               handler - for instructions that only touch GR/FR/PR/BR and
 *               the application registers the I unit can name, so nothing
 *               the block was admitted under can change (they may still
 *               fault or branch: chk.s, break);
 *   MJ_MEM      as MJ_DIRECT, plus a re-check after the access of what a
 *               memory reference can change: the block's code bytes, a
 *               device's interrupt or exit request;
 *   MJ_HELPER   mj_helper(), which re-checks everything (system
 *               instructions, branches other than br.cond/br.cloop). */
typedef enum { MJ_NOP, MJ_NATIVE, MJ_DIRECT, MJ_MEM, MJ_HELPER } MJHow;

static MJHow mj_classify(const MercedTbEntry *tb, unsigned s) {
    switch (tb->kind[s]) {
    case SLOT_NOP:
        return MJ_NOP;
    case SLOT_X:
        return bits(tb->slots[2], 37, 4) == 6 ? MJ_NATIVE : MJ_HELPER;
    default:
        break;
    }
    switch (tb->op[s]) {
    case FOP_ADD: case FOP_SUB: case FOP_AND: case FOP_ANDCM:
    case FOP_OR: case FOP_XOR: case FOP_SHLADD:
    case FOP_ADDI: case FOP_SUBI: case FOP_ANDI: case FOP_ANDCMI:
    case FOP_ORI: case FOP_XORI:
    case FOP_CMP: case FOP_EXT:
    case FOP_BR_COND: case FOP_BR_CLOOP:
        return MJ_NATIVE;
    case FOP_LD: case FOP_ST:
        return MJ_MEM;
    case FOP_SETF_SIG: case FOP_GETF_SIG:
        return MJ_DIRECT;
    default:
        break;
    }
    uint64_t raw = tb->slots[s];
    unsigned major = (unsigned)bits(raw, 37, 4);
    switch (bundle_units[tb->tmpl][s]) {
    case 'F':
        return MJ_DIRECT;
    case 'I':
        if (major == 0 && bits(raw, 33, 3) == 0) {
            unsigned x6 = (unsigned)bits(raw, 27, 6);
            unsigned ar3 = (unsigned)bits(raw, 20, 7);
            /* mov.i to an AR: only ar.pfs/lc/ec (64+) are I-unit ARs */
            if (x6 == 0x0A || x6 == 0x2A)
                return ar3 >= 64 ? MJ_DIRECT : MJ_HELPER;
            /* mov.i from ar.itc reads the slot-accurate ITC */
            if (x6 == 0x32)
                return ar3 == AR_ITC ? MJ_HELPER : MJ_DIRECT;
        }
        return MJ_DIRECT;
    case 'M':
        if (major >= 4 && major <= 7) return MJ_MEM;
        if (major >= 8) return MJ_DIRECT;              /* A-unit on M */
        return MJ_HELPER;
    default:
        return MJ_HELPER;
    }
}

/* Does nothing after this slot ever execute (an unconditional branch)? */
static bool mj_ends_block(const MercedTbEntry *tb, unsigned s) {
    if (tb->qpr[s] != 0) return false;
    if (tb->kind[s] == SLOT_X)
        return bits(tb->slots[2], 37, 4) >= 0xC;          /* brl */
    if (tb->kind[s] != SLOT_EXEC) return false;
    if (bundle_units[tb->tmpl][s] != 'B') return false;
    return tb->op[s] != FOP_BR_CLOOP;
}

static uint64_t mj_next_ip(const MercedJitSlot *js, const MercedTbEntry *tb) {
    uint64_t bva = js->ip & ~UINT64_C(0xF);
    if (tb->kind[js->slot] == SLOT_X || js->slot == 2) return bva + 16;
    return bva | (js->slot + 1u);
}

/* Before a slot that calls into the interpreter: bring the CPU up to date
 * with the natively executed slots before it. */
static void mj_gen_sync(MJGen *g, const MercedJitBlock *blk, unsigned i) {
    if (g->pending) {
        const MercedJitSlot *prev = &blk->slots[i - 1];
        mj_account(g, g->pending);
        mj_store_imm64(g, MOFF(last_successful_bundle),
                       prev->ip & ~UINT64_C(0xF));
        g->pending = 0;
    }
    if (i > 0) {
        const MercedJitSlot *prev = &blk->slots[i - 1];
        const MercedTbEntry *ptb = &blk->tbs[prev->bundle];
        x64_store8_imm(g->b, X64_RBX, X64_NOREG, 1, MOFF(group_start),
                       ptb->stop[prev->slot]);
    }
}

/* Slot i through a direct call of its interpreter handler (fast_op_fn for
 * fast_op() encodings), with fast_step()'s bookkeeping inline.  Expects
 * mj_gen_sync(); leaves the slot charged as pending. */
static void mj_gen_call(MJGen *g, const MercedJitBlock *blk, unsigned i,
                        bool mem, MercedStatus (*fast_op_fn)(
                            Merced *, const MercedTbEntry *, unsigned, int)) {
    X64Buf *b = g->b;
    const MercedJitSlot *js = &blk->slots[i];
    const MercedTbEntry *tb = &blk->tbs[js->bundle];
    unsigned s = js->slot;
    uint64_t bva = js->ip & ~UINT64_C(0xF);

    /* fast_step()'s pre-dispatch state */
    mj_store_imm64(g, MOFF(ip), js->ip);
    x64_store8_imm(b, X64_RBX, X64_NOREG, 1, MOFF(slot_stop_after), tb->stop[s]);
    x64_store8_imm(b, X64_RBX, X64_NOREG, 1, MOFF(taken), 0);
    x64_load64(b, X64_R13, X64_RBX, X64_NOREG, 1, MOFF(nfaults));
    int qp_reg = tb->op[s] == FOP_GENERIC ? X64_ARG2 : X64_ARG3;
    if (tb->qpr[s]) {
        x64_bt_mi(b, 4, X64_RBX, MOFF(pr), (uint8_t)mj_pr_bit(g, tb->qpr[s]));
        x64_setcc(b, X64_CC_B, X64_RAX);
        x64_movzx8(b, qp_reg, X64_RAX);
    } else {
        x64_mov_imm(b, qp_reg, 1);
    }
    x64_mov_rr(b, X64_ARG0, X64_RBX);
    if (tb->op[s] == FOP_GENERIC) {
        x64_mov_imm(b, X64_ARG1, tb->slots[s]);
        x64_call_abs(b, (const void *)tb->fn[s]);
    } else {
        x64_mov_imm(b, X64_ARG1, (uint64_t)(uintptr_t)tb);
        x64_mov_imm(b, X64_ARG2, s);
        x64_call_abs(b, (const void *)fast_op_fn);
    }
    /* halted? */
    x64_test_rr32(b, X64_RAX, X64_RAX);
    size_t ok = x64_jcc(b, X64_CC_E);
    x64_mov_rr32(b, X64_ARG3, X64_RAX);
    x64_mov_rr(b, X64_ARG0, X64_RBX);
    x64_mov_rr(b, X64_ARG1, X64_R12);
    x64_mov_imm(b, X64_ARG2, i);
    x64_call_abs(b, (const void *)mj_halt);
    x64_mov_imm(b, X64_RAX, i + 1);
    mj_jmp_epilogue(g);
    x64_here(b, ok);
    /* redirected (branch or delivered fault)? */
    x64_cmp8_mi(b, X64_RBX, MOFF(taken), 0);
    size_t straight = x64_jcc(b, X64_CC_E);
    mj_account(g, 1);
    x64_alu_rm(b, X64_CMP, X64_R13, X64_RBX, MOFF(nfaults));
    size_t faulted = x64_jcc(b, X64_CC_NE);
    mj_store_imm64(g, MOFF(last_successful_bundle), bva);
    x64_here(b, faulted);
    x64_store8_imm(b, X64_RBX, X64_NOREG, 1, MOFF(group_start), 1);
    x64_store8_imm(b, X64_RBX, X64_NOREG, 1, MOFF(taken), 0);
    x64_mov_imm(b, X64_RAX, i + 1);
    mj_jmp_epilogue(g);
    x64_here(b, straight);
    if (mem) {
        x64_mov_rr(b, X64_ARG0, X64_RBX);
        x64_mov_rr(b, X64_ARG1, X64_R12);
        x64_call_abs(b, (const void *)mj_mem_guard);
        x64_test_rr32(b, X64_RAX, X64_RAX);
        size_t cont = x64_jcc(b, X64_CC_E);
        mj_exit(g, i, 1, bva, true, tb->stop[s], mj_next_ip(js, tb));
        x64_here(b, cont);
    }
}

/* ── direct RAM access ────────────────────────────────────────────────────
 * mj_gen_mem() is FOP_LD/FOP_ST with the access done inline when the page
 * is in the host TLB, and mj_gen_call() through mj_mem_slow() otherwise,
 * which runs fast_op() and then (re)fills the host TLB entry from the
 * translation fast_op() just used.  An entry carries what makes the
 * interpreter's soft-TLB entry for that page valid - generation, the PSR
 * bits the permission checks read, the region register, and IVA (which
 * selects the access kind and whether the address is cacheable at all) -
 * so it's valid exactly when that soft-TLB entry is, and only pages the
 * machine calls plain RAM are entered. */

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

static MercedStatus mj_mem_slow(Merced *m, const MercedTbEntry *tb,
                                unsigned s, int qp) {
    uint64_t va = gr_read(m, tb->a3[s], NULL);
    uint64_t faults = m->nfaults;
    mj_of(m)->stats.mem_slow++;
    MercedStatus st = fast_op(m, tb, s, qp);
    if (st == MERCED_OK && qp && !m->taken && m->nfaults == faults)
        mj_htlb_fill(m, mj_of(m), va, tb->op[s] == FOP_ST);
    return st;
}

static void mj_htlb_flush(MercedJit *j) {
    memset(j->htlb, 0, sizeof(j->htlb));
}

/* Inline host-TLB probe: RAX = va.  On a hit RDX = the entry and R8 = the
 * host address; otherwise jumps to the returned rel32 (to be patched to the
 * slow path).  Clobbers R9, R11. */
static void mj_gen_htlb(MJGen *g, MercedJit *j, unsigned size, bool write,
                        size_t *miss, unsigned *n_miss) {
    X64Buf *b = g->b;
    MercedTranslationCache *tc = j->tc;
    x64_mov_rr(b, X64_RDX, X64_RAX);
    x64_shr_ri(b, X64_RDX, 12);
    x64_alu_ri32(b, X64_AND, X64_RDX, MJ_HTLB_SIZE - 1);
    x64_shl_ri(b, X64_RDX, 6);                     /* sizeof(MjTlbEntry) */
    x64_mov_imm(b, X64_R11, (uint64_t)(uintptr_t)
                &j->htlb[write ? MJ_HTLB_SIZE : 0]);
    x64_alu_rr(b, X64_ADD, X64_RDX, X64_R11);
    /* tag: page | access */
    x64_mov_rr(b, X64_R8, X64_RAX);
    x64_alu_ri(b, X64_AND, X64_R8, -4096);
    x64_alu_ri(b, X64_OR, X64_R8, write ? 2 : 1);
    x64_alu_rmx(b, X64_CMP, X64_R8, X64_RDX, X64_NOREG, 1,
                (int32_t)offsetof(MjTlbEntry, tag));
    miss[(*n_miss)++] = x64_jcc(b, X64_CC_NE);
    /* within the page, and not at the page's bus-only offset */
    x64_mov_rr32(b, X64_R9, X64_RAX);
    x64_alu_ri32(b, X64_AND, X64_R9, 0xFFF);
    x64_alu_ri32(b, X64_CMP, X64_R9, (int32_t)(0x1000 - size));
    miss[(*n_miss)++] = x64_jcc(b, X64_CC_A);
    x64_alu_rm32(b, X64_CMP, X64_R9, X64_RDX, X64_NOREG, 1,
                 (int32_t)offsetof(MjTlbEntry, slow_off));
    miss[(*n_miss)++] = x64_jcc(b, X64_CC_E);
    /* psr key */
    x64_mov_imm(b, X64_R9, XLATE_PSR_KEY);
    x64_alu_rm(b, X64_AND, X64_R9, X64_RBX, MOFF(psr));
    x64_alu_rmx(b, X64_CMP, X64_R9, X64_RDX, X64_NOREG, 1,
                (int32_t)offsetof(MjTlbEntry, psr_key));
    miss[(*n_miss)++] = x64_jcc(b, X64_CC_NE);
    /* region register */
    x64_mov_rr(b, X64_R9, X64_RAX);
    x64_shr_ri(b, X64_R9, 61);
    x64_load64(b, X64_R9, X64_RBX, X64_R9, 8, MOFF(rr));
    x64_alu_rmx(b, X64_CMP, X64_R9, X64_RDX, X64_NOREG, 1,
                (int32_t)offsetof(MjTlbEntry, rr));
    miss[(*n_miss)++] = x64_jcc(b, X64_CC_NE);
    /* IVA */
    x64_load64(b, X64_R9, X64_RBX, X64_NOREG, 1, MOFF(cr) + 8 * CR_IVA);
    x64_alu_rmx(b, X64_CMP, X64_R9, X64_RDX, X64_NOREG, 1,
                (int32_t)offsetof(MjTlbEntry, iva));
    miss[(*n_miss)++] = x64_jcc(b, X64_CC_NE);
    /* soft-TLB generation */
    x64_mov_imm(b, X64_R11, (uint64_t)(uintptr_t)&tc->xlate_generation);
    x64_load32(b, X64_R9, X64_R11, X64_NOREG, 1, 0);
    x64_alu_rm32(b, X64_CMP, X64_R9, X64_RDX, X64_NOREG, 1,
                 (int32_t)offsetof(MjTlbEntry, gen));
    miss[(*n_miss)++] = x64_jcc(b, X64_CC_NE);
    /* host address */
    x64_load64(b, X64_R8, X64_RDX, X64_NOREG, 1,
               (int32_t)offsetof(MjTlbEntry, addend));
    x64_alu_rr(b, X64_ADD, X64_R8, X64_RAX);
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
    X64Buf *b = g->b;
    const MercedJitSlot *js = &blk->slots[i];
    const MercedTbEntry *tb = &blk->tbs[js->bundle];
    unsigned s = js->slot;
    uint64_t bva = js->ip & ~UINT64_C(0xF);
    unsigned x = tb->x[s], size = x & LDST_SIZE;
    unsigned r1 = tb->a1[s], r2 = tb->a2[s], r3 = tb->a3[s];
    int64_t imm = tb->imm[s];
    bool store = tb->op[s] == FOP_ST;
    size_t miss[16];
    unsigned n_miss = 0;
    MercedTranslationCache *tc = j->tc;

    size_t skip = mj_skip_unless_qp(g, tb->qpr[s]);
    mj_gr_read(g, r3, X64_RAX, X64_RCX);                 /* va, n3 */
    x64_test_rr32(b, X64_RCX, X64_RCX);
    miss[n_miss++] = x64_jcc(b, X64_CC_NE);
    if (store) {
        mj_gr_read(g, r2, X64_R14, X64_R13);             /* value */
        x64_test_rr32(b, X64_R13, X64_R13);
        miss[n_miss++] = x64_jcc(b, X64_CC_NE);
        /* ALAT entries must see the store */
        x64_op_mem(b, 0, 0x83, -1, 7, X64_RBX, X64_NOREG, 1,
                   MOFF(alat_valid_mask), false);
        x64_b(b, 0);
        miss[n_miss++] = x64_jcc(b, X64_CC_NE);
    }
    mj_gen_htlb(g, j, size, store, miss, &n_miss);

    if (!store) {
        switch (size) {
        case 1:  x64_load8zx(b, X64_R8, X64_R8, X64_NOREG, 1, 0); break;
        case 2:  x64_load16zx(b, X64_R8, X64_R8, X64_NOREG, 1, 0); break;
        case 4:  x64_load32(b, X64_R8, X64_R8, X64_NOREG, 1, 0); break;
        default: x64_load64(b, X64_R8, X64_R8, X64_NOREG, 1, 0); break;
        }
        /* reg_inc is read before r1 is written (r2 may be r1) */
        if (x & LDST_INC_REG) mj_gr_read(g, r2, X64_R14, X64_R13);
        x64_mov_rr(b, X64_RAX, X64_R8);
        mj_gr_write(g, r1, X64_RAX, -1);
        if (x & (LDST_INC_IMM | LDST_INC_REG)) {
            /* base re-read after the load wrote r1 (r1 may be r3) */
            mj_gr_read(g, r3, X64_RAX, X64_RCX);
            if (x & LDST_INC_IMM) {
                x64_alu_ri(b, X64_ADD, X64_RAX, (int32_t)imm);
            } else {
                x64_alu_rr(b, X64_ADD, X64_RAX, X64_R14);
                x64_alu_rr32(b, X64_OR, X64_RCX, X64_R13);
            }
            mj_gr_write(g, r3, X64_RAX, X64_RCX);
        }
    } else {
        switch (size) {
        case 1:  x64_store8(b, X64_R14, X64_R8, X64_NOREG, 1, 0); break;
        case 2:  x64_store16(b, X64_R14, X64_R8, X64_NOREG, 1, 0); break;
        case 4:  x64_store32(b, X64_R14, X64_R8, X64_NOREG, 1, 0); break;
        default: x64_store64(b, X64_R14, X64_R8, X64_NOREG, 1, 0); break;
        }
        /* phys_write(): page generation, then the fetch cache */
        x64_load32(b, X64_R9, X64_RDX, X64_NOREG, 1,
                   (int32_t)offsetof(MjTlbEntry, pgidx));
        x64_mov_imm(b, X64_R11, (uint64_t)(uintptr_t)tc->page_generation);
        x64_inc32_m(b, X64_R11, X64_R9, 4, 0);
        x64_cmp8_mi(b, X64_RBX, MOFF(bundle_cache_valid), 0);
        size_t no_bc = x64_jcc(b, X64_CC_E);
        x64_mov_rr32(b, X64_R9, X64_RAX);                /* pa */
        x64_alu_ri32(b, X64_AND, X64_R9, 0xFFF);
        x64_alu_rmx(b, X64_OR, X64_R9, X64_RDX, X64_NOREG, 1,
                    (int32_t)offsetof(MjTlbEntry, pa_page));
        x64_load64(b, X64_R11, X64_RBX, X64_NOREG, 1, MOFF(bundle_cache_pa));
        x64_lea_x(b, X64_R10, X64_R11, X64_NOREG, 1, 16);  /* bc_pa + 16 */
        x64_alu_rr(b, X64_CMP, X64_R9, X64_R10);          /* pa < bc+16 */
        size_t no1 = x64_jcc(b, X64_CC_AE);
        x64_lea_x(b, X64_R10, X64_R9, X64_NOREG, 1, (int32_t)size);
        x64_alu_rr(b, X64_CMP, X64_R11, X64_R10);         /* bc < pa+size */
        size_t no2 = x64_jcc(b, X64_CC_AE);
        x64_store8_imm(b, X64_RBX, X64_NOREG, 1, MOFF(bundle_cache_valid), 0);
        x64_here(b, no1);
        x64_here(b, no2);
        x64_here(b, no_bc);
        /* post-increment from the original va (and NaT n3 = 0) */
        if (x & (LDST_INC_IMM | LDST_INC_REG)) {
            if (x & LDST_INC_IMM) {
                x64_alu_ri(b, X64_ADD, X64_RAX, (int32_t)imm);
                mj_gr_write(g, r3, X64_RAX, -1);
            } else {
                mj_gr_read(g, r1, X64_R14, X64_R13);
                x64_alu_rr(b, X64_ADD, X64_RAX, X64_R14);
                mj_gr_write(g, r3, X64_RAX, X64_R13);
            }
        }
        /* a store into this block's own page: leave, re-verify */
        x64_mov_imm(b, X64_R11, (uint64_t)(uintptr_t)
                    &tc->page_generation[tb_page_index(blk->pa)]);
        x64_load32(b, X64_R9, X64_R11, X64_NOREG, 1, 0);
        x64_alu_rm32(b, X64_CMP, X64_R9, X64_R12, X64_NOREG, 1,
                     (int32_t)offsetof(MercedJitBlock, code_gen));
        size_t same = x64_jcc(b, X64_CC_E);
        mj_exit(g, i, 1, bva, true, tb->stop[s], mj_next_ip(js, tb));
        x64_here(b, same);
    }
    size_t done = x64_jmp(b);

    for (unsigned k = 0; k < n_miss; k++)
        x64_here(b, miss[k]);
    mj_gen_call(g, blk, i, true, mj_mem_slow);
    x64_here(b, done);
    mj_end_skip(g, skip);
}

/* Emit the block's host code into j->buf.  Returns NULL if it didn't fit. */
static MercedJitFn mj_codegen(MercedJit *j, MercedJitBlock *blk) {
    MJGen gen, *g = &gen;
    memset(g, 0, sizeof(*g));
    g->b = &j->buf;
    g->bn = (blk->key >> 63) & 1;
    uint64_t cfm = blk->key & MJ_CFM_KEY_MASK;
    g->sor8 = CFM_SOR(cfm) * 8;
    g->rrb_gr = CFM_RRB_GR(cfm);
    g->rrb_pr = CFM_RRB_PR(cfm);
    X64Buf *b = g->b;

    /* 16-byte align each block's entry */
    while (x64_pos(b) & 15) x64_b(b, 0x90);
    uint8_t *entry = b->p;

    /* prologue: rbx = m, r12 = block; 16-byte aligned with Win64 shadow */
    x64_push(b, X64_RBX);
    x64_push(b, X64_R12);
    x64_push(b, X64_R13);
    x64_push(b, X64_R14);
    x64_alu_ri(b, X64_SUB, X64_RSP, 40);
    x64_mov_rr(b, X64_RBX, X64_ARG0);
    x64_mov_rr(b, X64_R12, X64_ARG1);

    for (unsigned i = 0; i < blk->n_slots; i++) {
        const MercedJitSlot *js = &blk->slots[i];
        const MercedTbEntry *tb = &blk->tbs[js->bundle];
        unsigned s = js->slot;
        uint64_t bva = js->ip & ~UINT64_C(0xF);
        g->last_stop = tb->stop[s];
        switch (mj_classify(tb, s)) {
        case MJ_NOP:
            j->stats.nop_slots++;
            g->pending++;
            break;

        case MJ_HELPER: {
            j->stats.helper_slots++;
            /* sync: the interpreter's view of everything before slot i */
            if (g->pending) {
                const MercedJitSlot *prev = &blk->slots[i - 1];
                mj_account(g, g->pending);
                mj_store_imm64(g, MOFF(last_successful_bundle),
                               prev->ip & ~UINT64_C(0xF));
                g->pending = 0;
            }
            if (i > 0) {
                const MercedJitSlot *prev = &blk->slots[i - 1];
                const MercedTbEntry *ptb = &blk->tbs[prev->bundle];
                x64_store8_imm(b, X64_RBX, X64_NOREG, 1, MOFF(group_start),
                               ptb->stop[prev->slot]);
            }
            x64_mov_rr(b, X64_ARG0, X64_RBX);
            x64_mov_rr(b, X64_ARG1, X64_R12);
            x64_mov_imm(b, X64_ARG2, i);
            x64_call_abs(b, (const void *)mj_helper);
            x64_test_rr32(b, X64_RAX, X64_RAX);
            size_t cont = x64_jcc(b, X64_CC_E);
            x64_mov_imm(b, X64_RAX, i + 1);
            mj_jmp_epilogue(g);
            x64_here(b, cont);
            break;
        }

        case MJ_DIRECT:
        case MJ_MEM: {
            bool mem = mj_classify(tb, s) == MJ_MEM;
            j->stats.direct_slots++;
            mj_gen_sync(g, blk, i);
            if (mem && mj_direct_mem_ok(j, tb, s))
                mj_gen_mem(g, j, blk, i);
            else
                mj_gen_call(g, blk, i, mem, mj_fast_op);
            g->pending = 1;
            break;
        }

        case MJ_NATIVE:
            j->stats.native_slots++;
            if (tb->kind[s] == SLOT_X) {
                mj_gen_movl(g, tb, s);
            } else switch (tb->op[s]) {
            case FOP_CMP: mj_gen_cmp(g, tb, s); break;
            case FOP_EXT: mj_gen_ext(g, tb, s); break;
            case FOP_BR_COND: {
                uint64_t target = bva + (uint64_t)(int64_t)tb->imm[s];
                size_t skip = mj_skip_unless_qp(g, tb->qpr[s]);
                mj_exit(g, i, g->pending + 1, bva, true, 1, target);
                mj_end_skip(g, skip);
                break;
            }
            case FOP_BR_CLOOP: {
                uint64_t target = bva + (uint64_t)(int64_t)tb->imm[s];
                x64_alu_mi(b, X64_CMP, X64_RBX, MOFF(ar) + 8 * AR_LC, 0);
                size_t skip = x64_jcc(b, X64_CC_E);
                x64_dec_m(b, X64_RBX, MOFF(ar) + 8 * AR_LC);
                mj_exit(g, i, g->pending + 1, bva, true, 1, target);
                x64_here(b, skip);
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
        bool native_last = mj_classify(tb, js->slot) != MJ_HELPER;
        if (g->pending || native_last)
            mj_exit(g, last, g->pending, js->ip & ~UINT64_C(0xF), true,
                    tb->stop[js->slot], mj_next_ip(js, tb));
        else
            mj_exit(g, last, 0, 0, false, 0, 0);
    }

    /* epilogue */
    size_t epi = x64_pos(b);
    x64_alu_ri(b, X64_ADD, X64_RSP, 40);
    x64_pop(b, X64_R14);
    x64_pop(b, X64_R13);
    x64_pop(b, X64_R12);
    x64_pop(b, X64_RBX);
    x64_ret(b);
    for (unsigned k = 0; k < g->n_exits; k++)
        x64_patch(b, g->exits[k], epi);

    if (b->overflow || g->too_many_exits)
        return NULL;
    return (MercedJitFn)(void *)entry;
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
        blk->code = mj_codegen(j, blk);
        if (!blk->code) {
            /* out of code space: start over with an empty cache */
            free(blk);
            mj_flush(j);
            return NULL;
        }
    }
    j->stats.compiled++;
    blk->all_next = j->all;
    j->all = blk;
    unsigned h = mj_hash(ip, key);
    blk->hash_next = j->hash[h];
    j->hash[h] = blk;
    return blk;
}

static void mj_unlink(MercedJit *j, MercedJitBlock *blk) {
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
    if ((m->psr & PSR_IS) || (ip & 0xF) > 2 || mj_sal_shim_bundle(bva) ||
        mj_irq_blocks(m) || fetch_observed(m))
        return MJ_INTERPRET;

    uint64_t key = mj_key(m);
    MercedJitBlock *blk = j->hash[mj_hash(ip, key)];
    while (blk && !(blk->va == ip && blk->key == key))
        blk = blk->hash_next;
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
            }
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
            uint64_t pa = blk->pa;
            mj_unlink(j, blk);
            blk = mj_translate(m, j, ip, pa, key);
            if (!blk) return MJ_INTERPRET;
        }
    }

    if (!blk->n_slots || blk->n_slots > budget ||
        mj_timer_in_window(m, blk->n_slots))
        return MJ_INTERPRET;
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
            unsigned k = blk->code(m, blk);
            j->in_block = false;
            if (j->flush_pending)
                mj_flush(j);
            n += k;
            st = j->status;
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
    fprintf(stderr, "merced-jit: memory slow path=%" PRIu64 " fills=%" PRIu64
            " not cacheable=%" PRIu64 " not RAM=%" PRIu64 "\n",
            j->stats.mem_slow, j->stats.mem_fill, j->stats.mem_nofill_xlate,
            j->stats.mem_nofill_page);
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
#ifndef GEMU_JIT_X64
    fprintf(stderr, "merced: the JIT needs an x86-64 host; "
            "using the interpreter\n");
    return MERCED_ACCEL_INTERPRETER;
#else
    if (!tc->jit) {
        MercedJit *j = calloc(1, sizeof(*j));
        if (!j) return MERCED_ACCEL_INTERPRETER;
        j->code_mem = gemu_jit_alloc_exec(MJ_CODE_SIZE);
        if (!j->code_mem) {
            free(j);
            fprintf(stderr, "merced: cannot allocate executable memory for "
                    "the JIT; using the interpreter\n");
            return MERCED_ACCEL_INTERPRETER;
        }
        x64_init(&j->buf, j->code_mem, MJ_CODE_SIZE);
        j->tc = tc;
        tc->jit = j;
        if (getenv("MERCED_JIT_STATS") && !mj_stats_jit) {
            mj_stats_on = true;
            static bool registered;
            mj_stats_jit = j;
            j->helper_profile = calloc(5u << 10, sizeof(uint64_t));
            if (!registered) atexit(mj_print_stats);
            registered = true;
        }
    }
    tc->jit->enabled = true;
    return MERCED_ACCEL_JIT;
#endif
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
