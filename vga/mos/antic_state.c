/* ANTIC and GTIA's save state: see antic_state.h. */
#include "antic_state.h"
#include "gemu/dev/state.h"

#define M(member) (ok &= GEMU_STATE_MEMBER(d, prefix, x, member))

/* Antic, in order.  Excluded:
 *   lines_total, pal     configuration (NTSC or PAL)
 *   mem, mem_ud          the wiring
 *   pixels_argb          worked out from pixels (antic_refresh_argb())
 *   warned_modes         which unsupported modes have been reported */
bool antic_state_add(GemuDevice *d, const char *prefix, Antic *x) {
    bool ok = true;
    M(dmactl); M(chactl); M(hscrol); M(vscrol); M(pmbase); M(chbase);
    M(nmien); M(nmist); M(dlist); M(scanline);
    M(colpm); M(colpf); M(colbk);
    M(prior); M(gractl); M(consol_w);
    M(hposp); M(hposm); M(sizep); M(sizem); M(grafp); M(grafm);
    M(pixels);
    return ok;
}
