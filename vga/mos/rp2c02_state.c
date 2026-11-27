/* The 2C02 PPU's save state: see rp2c02_state.h. */
#include "rp2c02_state.h"
#include "gemu/dev/state.h"

#define M(member) (ok &= GEMU_STATE_MEMBER(d, prefix, x, member))

/* Rp2c02, in order.  Excluded:
 *   pixels_argb       the frame in ARGB: worked out from pixels and the
 *                     palette (the machine regenerates it after a load)
 *   alt_palette_rgb   a host pointer to the VS. System's palette (wiring)
 *   dirty             the front end's "a frame is ready" flag, not the chip's
 *   chr_read, chr_write, chr_ud, nt_read, nt_write, nt_ud,
 *   irq_scanline, irq_ud, cart_open, cart_open_ud
 *                     callbacks and their contexts: the machine's wiring
 *                     (cart_open follows the NesPins debugging setting)
 *   debug             a host setting
 * Everything else is registered, including pixels (the palette-index frame:
 * a loaded game shows its frame before the next one is drawn) and the
 * region settings lines_total, vblank_line and no_odd_skip (set by the
 * machine at init: the same values on the same machine). */
bool rp2c02_state_add(GemuDevice *d, const char *prefix, Rp2c02 *x) {
    bool ok = true;
    M(vram); M(oam); M(palette);
    M(v); M(t); M(x); M(w);
    M(ppuctrl); M(ppumask); M(ppumask_pending); M(ppumask_delay); M(ppustatus);
    M(oamaddr); M(read_buf); M(open_bus);
    M(scanline); M(dot); M(lines_total); M(vblank_line); M(frame); M(odd_frame);
    M(no_odd_skip);
    M(nmi_pending);
    M(nt_latch); M(at_latch); M(pt_lo_latch); M(pt_hi_latch);
    M(bg_shift_lo); M(bg_shift_hi); M(bg_attr_lo); M(bg_attr_hi);
    M(at_latch_lo); M(at_latch_hi);
    M(oam2);
    M(eval_s); M(eval_slot); M(eval_byte); M(eval_ovf_copy); M(eval_done);
    M(n_spr); M(spr0_next); M(spr0_active);
    M(spr_is_zero); M(spr_shift_lo); M(spr_shift_hi); M(spr_attr); M(spr_x);
    M(pixels);
    M(mirror);
    return ok;
}
