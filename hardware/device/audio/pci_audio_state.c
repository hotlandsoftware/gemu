/* Save-state registration for the PCI audio controllers: see
 * pci_audio_state.h. */
#include "pci_audio_state.h"

#include <stdio.h>

/* prefix + part, for a nested struct's members ("cs.", "ich."). */
static bool sub_prefix(char *buf, size_t len, const char *prefix, const char *part) {
    int n = snprintf(buf, len, "%s%s", prefix, part);
    return n >= 0 && (size_t)n < len;
}

/* One item per member; `ok` and `d` are the caller's. */
#define M(obj, pre, member) (ok &= GEMU_STATE_MEMBER(d, pre, obj, member))

bool pci_audio_state_add(GemuDevice *d, const char *prefix, PciAudio *a) {
    /* Not registered:
     *   debug    debug flag (stderr tracing), set by the machine
     *   wiring   host pointers (clock, read, write, irq, output, opaque) and
     *            clock_hz, the machine's wiring, set by pci_audio_init
     *
     * u is a union of the two controllers' registers, only one of which is
     * live (model says which). Both views are registered member by member
     * rather than as one blob, so no padding is saved: the views overlap,
     * every byte that is data in either view is saved by that view, and on
     * load both write back the same bytes they saved. The ICH view is ~120
     * bytes, so the overlap costs next to nothing. */
    bool ok = true;
    char p[64];

    M(a, prefix, model);
    M(a, prefix, cfg);
    M(a, prefix, codec);
    M(a, prefix, gain);
    M(a, prefix, mark);
    M(a, prefix, fraction);
    M(a, prefix, irq_level);
    M(a, prefix, frames);
    M(a, prefix, nonzero_frames);
    M(a, prefix, dma_bytes);
    M(a, prefix, interrupts);

    /* CS4281 */
    if (!sub_prefix(p, sizeof p, prefix, "cs."))
        return false;
    M(&a->u.cs, p, reg);
    M(&a->u.cs, p, fifo);
    M(&a->u.cs, p, phase);
    M(&a->u.cs, p, last);
    M(&a->u.cs, p, intena);

    /* ICH AC'97 */
    static const GemuStateField dma_fields[] = {
        GEMU_STATE_FIELD(IchAudioDma, bdbar),
        GEMU_STATE_FIELD(IchAudioDma, address),
        GEMU_STATE_FIELD(IchAudioDma, control),
        GEMU_STATE_FIELD(IchAudioDma, status),
        GEMU_STATE_FIELD(IchAudioDma, remaining),
        GEMU_STATE_FIELD(IchAudioDma, civ),
        GEMU_STATE_FIELD(IchAudioDma, lvi),
        GEMU_STATE_FIELD(IchAudioDma, piv),
        GEMU_STATE_FIELD(IchAudioDma, command),
        GEMU_STATE_FIELD(IchAudioDma, valid),
        GEMU_STATE_FIELD(IchAudioDma, phase),
        GEMU_STATE_FIELD(IchAudioDma, last),
    };
    if (!sub_prefix(p, sizeof p, prefix, "ich.dma"))
        return false;
    ok &= GEMU_STATE_TABLE(d, p, a->u.ich.dma, dma_fields);
    if (!sub_prefix(p, sizeof p, prefix, "ich."))
        return false;
    M(&a->u.ich, p, control);
    M(&a->u.ich, p, status);
    M(&a->u.ich, p, semaphore);
    return ok;
}
#undef M
