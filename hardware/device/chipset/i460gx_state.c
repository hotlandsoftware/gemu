/* Save-state registration for the 460GX chipset model: see i460gx_state.h. */
#include "i460gx_state.h"

#include <stdio.h>

/* prefix + part, for a nested struct's members ("smbus.", "pm."). */
static bool sub_prefix(char *buf, size_t len, const char *prefix, const char *part) {
    int n = snprintf(buf, len, "%s%s", prefix, part);
    return n >= 0 && (size_t)n < len;
}

/* One item per member; `ok` and `d` are the caller's. */
#define M(obj, pre, member) (ok &= GEMU_STATE_MEMBER(d, pre, obj, member))

bool i460gx_state_add(GemuDevice *d, const char *prefix, I460GX *s) {
    /* Not registered:
     *   config                     I460GXConfig: the board's wiring (bus and
     *                              device numbers, expanders, clock, I2C
     *                              segments, flash image, callbacks, opaque,
     *                              cached debug flags), set by i460gx_init
     *   fwh[].reports,
     *   fwh[].register_reports     debug message counters (rate limit for
     *                              the debug output)
     *   fwh[].debug                debug flag, set by fwh82802_init from the
     *                              board's config */
    bool ok = true;
    char p[64];

    M(s, prefix, sac_cbnr);
    M(s, prefix, sac_ccsr);
    M(s, prefix, iosapic_regsel);
    M(s, prefix, iosapic_rte);
    M(s, prefix, pid_cfg);
    M(s, prefix, ifb_cfg);
    M(s, prefix, ifb_usb_cfg);
    M(s, prefix, ifb_smbus_cfg);

    if (!sub_prefix(p, sizeof p, prefix, "smbus."))
        return false;
    I460GXSmbus *sm = &s->smbus;
    M(sm, p, hst_sts);
    M(sm, p, slv_sts);
    M(sm, p, hst_cnt);
    M(sm, p, hst_cmd);
    M(sm, p, hst_add);
    M(sm, p, hst_dat0);
    M(sm, p, hst_dat1);
    M(sm, p, block);
    M(sm, p, block_index);
    M(sm, p, slv_cnt);
    M(sm, p, shdw_cmd);
    M(sm, p, slv_evt);
    M(sm, p, slv_dat);

    M(s, prefix, chipset_bus);
    M(s, prefix, chipset_cfg);
    M(s, prefix, memcard_cfg);

    if (!sub_prefix(p, sizeof p, prefix, "pm."))
        return false;
    I460GXPm *pm = &s->pm;
    M(pm, p, pm1_sts);
    M(pm, p, pm1_en);
    M(pm, p, pm1_cnt);
    M(pm, p, gpe0_sts);
    M(pm, p, gpe0_en);
    M(pm, p, gpe1_sts);
    M(pm, p, gpe1_en);
    M(pm, p, glb_ctl);
    M(pm, p, glb_sts);
    M(pm, p, tmr);
    M(pm, p, apmc);
    M(pm, p, apms);
    M(pm, p, gp_out);
    M(pm, p, gp_data);
    M(pm, p, gp_ttl);
    M(pm, p, gp_blink);
    M(pm, p, gp_lock);
    M(pm, p, gp_inv);
    M(pm, p, gp_smi);
    M(pm, p, gp_pulse);
    M(pm, p, gp_core);
    M(pm, p, gp_pullup);
    M(pm, p, pulse_latch);
    M(pm, p, in_prev);
    M(pm, p, ext_mask);
    M(pm, p, ext_level);
    M(pm, p, clock_last);
    M(pm, p, clock_rem);
    M(pm, p, swsmi_left);
    M(pm, p, min_acc);
    M(pm, p, pwrbtn_held);
    M(pm, p, pwrbtn_down);
    M(pm, p, pwrbtn_overridden);
    M(pm, p, thrm_connected);
    M(pm, p, thrm_level);
    M(pm, p, thrm_prev);
    M(pm, p, legacy_usb);
    M(pm, p, bioswen_ble);
    M(pm, p, state);
    M(pm, p, sci);
    M(pm, p, smi);
    M(pm, p, sci_irq);
    M(pm, p, sci_line);

    static const GemuStateField fwh_fields[] = {
        GEMU_STATE_FIELD(Fwh82802, gpi),
        GEMU_STATE_FIELD(Fwh82802, mode),
        GEMU_STATE_FIELD(Fwh82802, setup),
        GEMU_STATE_FIELD(Fwh82802, status),
        GEMU_STATE_FIELD(Fwh82802, op),
        GEMU_STATE_FIELD(Fwh82802, busy_until),
        GEMU_STATE_FIELD(Fwh82802, remaining),
        GEMU_STATE_FIELD(Fwh82802, suspended),
        GEMU_STATE_FIELD(Fwh82802, lock),
        GEMU_STATE_FIELD(Fwh82802, rng_status),
        GEMU_STATE_FIELD(Fwh82802, rng),
        GEMU_STATE_FIELD(Fwh82802, changed_off),
        GEMU_STATE_FIELD(Fwh82802, changed_len),
    };
    if (!sub_prefix(p, sizeof p, prefix, "fwh"))
        return false;
    ok &= GEMU_STATE_TABLE(d, p, s->fwh, fwh_fields);

    M(s, prefix, flash_changed_off);
    M(s, prefix, flash_changed_len);
    M(s, prefix, uhci_regs);
    M(s, prefix, uhci_frame_itc);
    return ok;
}
#undef M
