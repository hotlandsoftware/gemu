/* Save-state registration for the 82559 Ethernet controller: see
 * e100_state.h. */
#include "e100_state.h"

#include <stdio.h>

static bool sub_prefix(char *buf, size_t len, const char *prefix, const char *part) {
    int k = snprintf(buf, len, "%s%s", prefix, part);
    return k >= 0 && (size_t)k < len;
}

/* One item per member; `ok`, `d`, `prefix` and `n` are the caller's. */
#define M(member) (ok &= GEMU_STATE_MEMBER(d, prefix, n, member))

bool e100_state_add(GemuDevice *d, const char *prefix, E100 *n) {
    /* Not registered:
     *   wiring    host pointers (clock, irq, dma_read, dma_write, tx, rx,
     *             opaque) and clock_hz, the machine's wiring, rebuilt by
     *             e100_init/e100_rebind
     *   rom       host pointer, the borrowed flash image
     *   rom_len   the length of that borrowed image, set by e100_set_rom
     *             from the machine's live buffer: a saved value could make
     *             flash/ROM BAR reads overrun the buffer actually attached */
    bool ok = true;
    char p[64];

    M(silicon_id);
    M(silicon_rev);
    M(pci);
    M(rom_bar);
    M(clk_last);
    M(clk_rem);

    M(ee);
    M(ee_words);
    M(ee_abits);
    M(ee_reg);
    M(ee_do);
    M(ee_phase);
    M(ee_pend);
    M(ee_n);
    M(ee_op);
    M(ee_addr);
    M(ee_sh);
    M(ee_out);
    M(ee_wdata);
    M(ee_wen);
    M(ee_ia);
    M(ee_id);

    M(stat);
    M(ictl);
    M(gp);
    M(port_sh);
    M(port_last);
    M(mdi_sh);
    M(mdi);
    M(flash_ctl);
    M(eri);
    M(fc_thr);
    M(pmdr);
    M(gctl);
    M(xoff_req);
    M(rxbc);
    M(irq);

    M(cus);
    M(cu_base);
    M(cu_cb);
    M(cu_prev);
    M(cu_next);
    M(hpq_cb);
    M(hpq_next);
    M(hpqs);
    M(in_hpq);
    M(cu_resume_req);
    M(cid);
    M(cna_pending);
    M(cna_left);
    M(dump_addr);
    M(dump_set);

    M(rus);
    M(ru_base);
    M(ru_rfd);
    M(hds);

    static const GemuStateField rxq_fields[] = {
        GEMU_STATE_FIELD(E100RxEntry, len),
        GEMU_STATE_FIELD(E100RxEntry, st),
    };
    if (!sub_prefix(p, sizeof p, prefix, "rxq"))
        return false;
    ok &= GEMU_STATE_TABLE(d, p, n->rxq, rxq_fields);
    M(rxq_n);
    M(rxq_bytes);
    M(rxfifo);
    M(rx_overrun_counted);
    M(rx_credit);

    M(cfgb);
    M(ia);
    M(hash);
    M(sc);
    M(sc_tco_tx);
    M(sc_tco_rx);
    M(ucode);
    M(lpf);

    M(pause_left);
    M(paused);
    M(paused_low);
    M(fc_full);
    M(fc_restop_armed);
    M(fc_restop_left);

    M(bmcr);
    M(anar);
    M(r16w);
    M(r17);
    M(r26);
    M(r27);
    M(link);
    M(link_ll);
    M(an_done);
    M(aner_pr);
    M(spd100);
    M(fdx);
    M(phy_wait);
    M(phy_lp);
    M(phy_left);

    M(pstate);
    M(dsel);
    M(pme_en);
    M(pme_st);

    M(last_tx_status);
    M(last_rx_status);
    M(last_rx_len);
    M(last_tx_fcs);
    M(last_rx_fcs);

    M(cable_out);
    return ok;
}
#undef M
