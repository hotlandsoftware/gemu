/* Save-state registration for the Rage 128 and its VGA core: see
 * rage128_state.h. */
#include "rage128_state.h"

#include <stdio.h>

/* prefix + part, for a nested struct's members ("host.", "cce."). */
static bool sub_prefix(char *buf, size_t len, const char *prefix, const char *part) {
    int n = snprintf(buf, len, "%s%s", prefix, part);
    return n >= 0 && (size_t)n < len;
}

/* One item per member; `ok` and `d` are the caller's. */
#define M(obj, pre, member) (ok &= GEMU_STATE_MEMBER(d, pre, obj, member))

bool rage128_state_add(GemuDevice *d, const char *prefix, Rage128 *r) {
    /* Not registered:
     *   wiring     host pointers (clock, irq, opaque) and clock_hz, the
     *              machine's wiring, rebuilt by rage128_init/rage128_rebind
     *   vga        host pointer to the VGA core (registered separately with
     *              vga_state_add)
     *   rom        host pointer, the borrowed BIOS image
     *   rom_len    the length of that borrowed image, set by rage128_set_rom
     *              from the machine's live buffer: restoring a saved value
     *              could make ROM BAR reads overrun the buffer actually
     *              attached, and the image itself isn't saved here either */
    bool ok = true;
    char p[64];
    M(r, prefix, device_id);
    M(r, prefix, revision);
    M(r, prefix, cfg);
    M(r, prefix, regs);
    M(r, prefix, pll);
    M(r, prefix, palette);

    if (!sub_prefix(p, sizeof p, prefix, "host."))
        return false;
    R128HostOp *h = &r->host;
    M(h, p, active);
    M(h, p, x0);
    M(h, p, x);
    M(h, p, y);
    M(h, p, w);
    M(h, p, h);
    M(h, p, xdir);
    M(h, p, ydir);
    M(h, p, left);
    M(h, p, byte_aligned);
    M(h, p, bits);
    M(h, p, skip);

    M(r, prefix, vblank_save_clear);
    M(r, prefix, int_vblank_clear);
    M(r, prefix, int_vline_clear);
    M(r, prefix, irq_out);

    if (!sub_prefix(p, sizeof p, prefix, "cce."))
        return false;
    Rage128Cce *c = &r->cce;
    M(c, p, fifo);
    M(c, p, head);
    M(c, p, count);
    M(c, p, packet);
    M(c, p, have);
    M(c, p, need);
    M(c, p, microcode);
    M(c, p, mode24);
    M(c, p, reserved);
    return ok;
}

bool vga_state_add(GemuDevice *d, const char *prefix, Vga *v) {
    /* Not registered:
     *   disp_gen, vram_dirty, plane_dirty
     *              redraw hints for the renderer, not guest state (after a
     *              load the machine should vga_touch_all() for a full redraw)
     *   vram_size  the size of the machine's VRAM buffer, set by
     *              vga_init/vga_attach_vram from the live allocation: a saved
     *              value could disagree with the buffer actually attached and
     *              let accesses overrun it; the machine's VRAM item carries
     *              the size and a mismatch refuses the load
     *   vram       host pointer to that buffer (registered by the machine) */
    bool ok = true;
    M(v, prefix, seq_idx);
    M(v, prefix, seq);
    M(v, prefix, gc_idx);
    M(v, prefix, gc);
    M(v, prefix, crtc_idx);
    M(v, prefix, crtc);
    M(v, prefix, attr_idx);
    M(v, prefix, attr);
    M(v, prefix, attr_flipflop);
    M(v, prefix, misc);
    M(v, prefix, dac);
    M(v, prefix, dac_idx);
    M(v, prefix, dac_sub);
    M(v, prefix, dac_ridx);
    M(v, prefix, dac_rsub);
    M(v, prefix, pixel_mask);
    M(v, prefix, dac_reading);
    M(v, prefix, plane);
    M(v, prefix, latch);
    M(v, prefix, font_uploaded);
    M(v, prefix, retrace);
    return ok;
}
#undef M
