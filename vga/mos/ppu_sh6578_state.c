/* The SH6578 PPU's save state: see ppu_sh6578_state.h. */
#include "ppu_sh6578_state.h"
#include "gemu/dev/state.h"

#define M(member) (ok &= GEMU_STATE_MEMBER(d, prefix, x, member))

/* Sh6578Ppu, in order.  Excluded: cycles_per_scanline, lines_total,
 * vblank_line (configuration: the clocks and the line counts). */
bool ppu_sh6578_state_add(GemuDevice *d, const char *prefix, Sh6578Ppu *x) {
    bool ok = true;
    M(vram); M(oam); M(palette);
    M(v); M(t); M(x); M(w); M(vram_addr);
    M(ppuctrl); M(ppumask); M(ppustatus); M(oamaddr); M(colsel_pntstart); M(read_buf);
    M(data_latch);
    M(cycle_acc); M(scanline); M(frame); M(dirty); M(nmi_pending); M(scanline_tick);
    M(pixels); M(pixels_argb);
    return ok;
}
