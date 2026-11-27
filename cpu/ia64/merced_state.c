/* Save-state registration for the Merced core: see merced_state.h. */
#include "merced_state.h"
#include "merced.h"
#include "gemu/dev/state.h"
#include <stddef.h>

/* The anonymous element types of struct Merced's arrays of structs. */
typedef __typeof__(((Merced *)0)->alat[0])             MercedAlatEntry;


static const GemuStateField fp_fields[] = {
    GEMU_STATE_FIELD(MercedFpReg, sig),
    GEMU_STATE_FIELD(MercedFpReg, exp),
    GEMU_STATE_FIELD(MercedFpReg, sign),
    GEMU_STATE_FIELD(MercedFpReg, nat),
};

static const GemuStateField tlb_fields[] = {
    GEMU_STATE_FIELD(MercedTlbEntry, va_start),
    GEMU_STATE_FIELD(MercedTlbEntry, va_end),
    GEMU_STATE_FIELD(MercedTlbEntry, pfn_base),
    GEMU_STATE_FIELD(MercedTlbEntry, rid),
    GEMU_STATE_FIELD(MercedTlbEntry, ps),
    GEMU_STATE_FIELD(MercedTlbEntry, valid),
    GEMU_STATE_FIELD(MercedTlbEntry, pending_purge),
    GEMU_STATE_FIELD(MercedTlbEntry, itir),
    GEMU_STATE_FIELD(MercedTlbEntry, pte),
};

static const GemuStateField alat_fields[] = {
    GEMU_STATE_FIELD(MercedAlatEntry, phys_addr),
    GEMU_STATE_FIELD(MercedAlatEntry, size),
    GEMU_STATE_FIELD(MercedAlatEntry, reg),
    GEMU_STATE_FIELD(MercedAlatEntry, valid),
};

static const GemuStateField l1d_fields[] = {
    GEMU_STATE_FIELD(MercedL1DWay, chunk),
    GEMU_STATE_FIELD(MercedL1DWay, par),
    GEMU_STATE_FIELD(MercedL1DWay, tag),
    GEMU_STATE_FIELD(MercedL1DWay, tpar),
    GEMU_STATE_FIELD(MercedL1DWay, planted),
};

static const GemuStateField l1i_fields[] = {
    GEMU_STATE_FIELD(MercedL1IWay, word),
    GEMU_STATE_FIELD(MercedL1IWay, wpar),
    GEMU_STATE_FIELD(MercedL1IWay, tag),
    GEMU_STATE_FIELD(MercedL1IWay, tpar),
    GEMU_STATE_FIELD(MercedL1IWay, valid),
    GEMU_STATE_FIELD(MercedL1IWay, planted),
    GEMU_STATE_FIELD(MercedL1IWay, dec_ok),
    GEMU_STATE_FIELD(MercedL1IWay, dec),
    GEMU_STATE_FIELD(MercedL1IWay, tag_bad),
    GEMU_STATE_FIELD(MercedL1IWay, word_bad),
};

static const GemuStateField l3_fields[] = {
    GEMU_STATE_FIELD(MercedL3Line, pa),
    GEMU_STATE_FIELD(MercedL3Line, addr),
    GEMU_STATE_FIELD(MercedL3Line, data),
    GEMU_STATE_FIELD(MercedL3Line, seq),
    GEMU_STATE_FIELD(MercedL3Line, mask),
    GEMU_STATE_FIELD(MercedL3Line, ecc),
    GEMU_STATE_FIELD(MercedL3Line, state),
    GEMU_STATE_FIELD(MercedL3Line, way),
    GEMU_STATE_FIELD(MercedL3Line, planted),
    GEMU_STATE_FIELD(MercedL3Line, used),
    GEMU_STATE_FIELD(MercedL3Line, owned),
};

static const GemuStateField l2_fields[] = {
    GEMU_STATE_FIELD(MercedL2Way, chunk),
    GEMU_STATE_FIELD(MercedL2Way, ecc),
    GEMU_STATE_FIELD(MercedL2Way, tag),
    GEMU_STATE_FIELD(MercedL2Way, tag_seq),
    GEMU_STATE_FIELD(MercedL2Way, stword),
    GEMU_STATE_FIELD(MercedL2Way, tag_lo),
    GEMU_STATE_FIELD(MercedL2Way, tag_hi),
    GEMU_STATE_FIELD(MercedL2Way, parity),
    GEMU_STATE_FIELD(MercedL2Way, modified),
    GEMU_STATE_FIELD(MercedL2Way, owned),
};

#define NFIELDS(f) (sizeof (f) / sizeof (f)[0])
/* A 2-D array of structs as one table of all its elements. */
#define TABLE2D(name, arr, fields) \
    gemu_state_add_table(d, (name), &(arr)[0][0], sizeof (arr)[0][0], \
                         sizeof (arr) / sizeof (arr)[0][0], (fields), NFIELDS(fields))

bool merced_state_add(GemuDevice *d, Merced *m) {
    /*
     * Not registered (everything else in struct Merced and MercedCache is):
     *   bus                 host pointers and function pointers (the
     *                       machine's wiring, set up by merced_create())
     *   msr_polls           debug history, not machine state (MSR poll
     *                       report counters)
     *   trace_n             debug history, not machine state (stderr trace
     *                       countdown)
     *   trace_history, trace_history_ext, trace_history_next
     *                       debug history, not machine state
     *   call_history, call_history_next
     *                       debug history, not machine state
     *   translation_cache   host pointer: the decoded/JIT code cache, dropped
     *                       with merced_flush_translation_cache() after a load
     */
    bool ok = true;
#define M(f)  (ok &= GEMU_STATE_MEMBER(d, NULL, m, f))
#define C(f)  (ok &= GEMU_STATE_MEMBER(d, "cache.", &m->cache, f))

    /* architectural state */
    M(ip);
    M(psr);
    M(psr_ic_inflight);
    M(cfm);
    M(pr);
    M(gr_static);
    M(gr_bank0);
    M(nat_static);
    M(nat_bank0);
    M(gr_stack);
    M(nat_stack);
    M(bof);
    M(bof_total);
    M(rse_anchor_addr);
    M(rse_anchor_regs);
    M(rse_flushed_regs);
    ok &= GEMU_STATE_TABLE(d, "fr", m->fr, fp_fields);
    M(br);
    M(ar);
    M(cr);
    M(rr);
    M(pkr);
    M(dbr);
    M(ibr);
    M(pmc);
    M(pmd);
    M(cpuid);
    M(msr);
    M(msr_written);
    ok &= GEMU_STATE_TABLE(d, "itr", m->itr, tlb_fields);
    ok &= GEMU_STATE_TABLE(d, "dtr", m->dtr, tlb_fields);
    ok &= GEMU_STATE_TABLE(d, "itc", m->itc, tlb_fields);
    ok &= GEMU_STATE_TABLE(d, "dtc", m->dtc, tlb_fields);
    M(itc_next);
    M(dtc_next);
    M(itr_hint);
    M(dtr_hint);
    M(itc_hint);
    M(dtc_hint);
    M(tlb_purge_pending);
    M(bundle_cache_valid);
    M(bundle_cache_translation_valid);
    M(bundle_cache_va);
    M(bundle_cache_pa);
    M(bundle_cache_lo);
    M(bundle_cache_hi);
    M(bundle_cache_tmpl);
    M(bundle_cache_slots);
    ok &= GEMU_STATE_TABLE(d, "alat", m->alat, alat_fields);
    M(alat_valid_mask);
    M(timer_pending);
    M(external_pending);
    M(ext_pending_count);
    M(external_itc);

    /* bookkeeping */
    M(taken);
    M(group_start);
    M(slot_stop_after);
    M(ninsts);
    M(nfaults);
    M(last_successful_bundle);
    M(halt_msg);
    M(halt_ip);
    M(warned);
    M(cpu_revision);
    M(cpu_model);
    M(ia32_int10_shim);
    M(clock_ratio_code);
    M(clock_ratio_strapped);
    M(slot_cycles);
    M(pal_mc_save_addr);
    M(pal_pmi_entry);
    M(pal_interrupt_block_addr);
    M(pal_io_block_addr);
    M(pal_mc_expected);
    M(region7_directmap_limit);
    M(ext_in_service);
    M(time_base);
    M(merced_msrs);

    /* cache arrays (MercedCache) */
    C(observe);
    C(ic_track);
    C(served);
    C(access_uc);
    C(seq);
    C(mca_pending);
    C(l2mode);
    C(nl1d);
    C(nl1d_planted);
    C(l1d_known);
    C(l1d_loads);
    C(l1d_plru);
    C(l1d_valid);
    C(l1d_wp);
    ok &= TABLE2D("cache.l1d", m->cache.l1d, l1d_fields);
    C(icctl);
    C(nplanted);
    C(ic_known);
    C(ic_unknown_logged);
    C(ic_lost_ip);
    C(ic_lost_psr);
    C(ic_fetches);
    C(plru);
    ok &= TABLE2D("cache.l1i", m->cache.l1i, l1i_fields);
    C(testmode);
    ok &= GEMU_STATE_TABLE(d, "cache.inj", m->cache.inj, l3_fields);
    C(ninj);
    C(nl3_planted);
    C(l3_dirty);
    C(l3_head);
    C(l3_next);
    C(l3_known);
    C(l3_loads);
    C(l3_plru);
    C(nvalid);
    C(nstate);
    C(l1d_t0);
    C(l2_t0);
    C(l3_t0);
    C(ic_t0);
    C(l2_known);
    C(l2_accesses);
    C(l2ctl);
    C(l2inv);
    C(l2_order);
    C(l2_inpair);
    C(port_ip);
    C(port_ri);
    C(port);
    ok &= TABLE2D("cache.l2", m->cache.l2, l2_fields);

#undef M
#undef C
    return ok;
}
