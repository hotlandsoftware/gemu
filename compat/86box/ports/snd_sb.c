/* 86Box's Sound Blaster (third_party/86box/v6.0, unchanged), the models an
 * 8-bit slot takes: "sb" (1.0: DSP 1.05, the CMS chips), "sb1.5" (CMS
 * optional) and "sb2.0" (DSP 2.01, auto-init and high-speed DMA; the CD
 * version's mixer with mixaddr=).  Each has an OPL2 under it (fm_driver_get).
 *
 * The file holds every model up to the AWE64; the others need what GEMU
 * doesn't have (16-bit DMA, the MPU-401, PnP, the EMU8000, IDE) and aren't
 * offered (compat/86box/absent.c stops GEMU if one is reached).
 *
 * The state is the whole sb_t - the DSP, the CMS chips, the mixer - but for
 * its pointers (the OPL drivers' handles, the DSP's callbacks and links)
 * and its timers, which the layer saves; the OPL chip is its own device's. */
#include "../gemu_86box.h"
#include "gemu/dev/state.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#if defined(__GNUC__) && __GNUC__ >= 14
#pragma GCC diagnostic ignored "-Wcalloc-transposed-args"
#endif
#include "../../../third_party/86box/v6.0/src/sound/snd_sb.c"
#pragma GCC diagnostic pop

static const Gemu86BoxKeep sb_keep[] = {
    GEMU_86BOX_KEEP(sb_t, opl),
    GEMU_86BOX_KEEP(sb_t, opl2),
    GEMU_86BOX_KEEP(sb_t, dsp.parent),
    GEMU_86BOX_KEEP(sb_t, dsp.dma_readb),
    GEMU_86BOX_KEEP(sb_t, dsp.dma_readw),
    GEMU_86BOX_KEEP(sb_t, dsp.dma_writeb),
    GEMU_86BOX_KEEP(sb_t, dsp.dma_writew),
    GEMU_86BOX_KEEP(sb_t, dsp.dma_priv),
    GEMU_86BOX_KEEP(sb_t, dsp.irq_update),
    GEMU_86BOX_KEEP(sb_t, dsp.irq_priv),
    GEMU_86BOX_KEEP(sb_t, dsp.output_timer),
    GEMU_86BOX_KEEP(sb_t, dsp.input_timer),
    GEMU_86BOX_KEEP(sb_t, dsp.wb_timer),
    GEMU_86BOX_KEEP(sb_t, dsp.irq_timer),
    GEMU_86BOX_KEEP(sb_t, dsp.irq16_timer),
    GEMU_86BOX_KEEP(sb_t, dsp.espcm_fifo),
    GEMU_86BOX_KEEP(sb_t, dsp.mpu),
    GEMU_86BOX_KEEP(sb_t, mpu),
    GEMU_86BOX_KEEP(sb_t, emu8k),
    GEMU_86BOX_KEEP(sb_t, gameport),
    GEMU_86BOX_KEEP(sb_t, opl_mixer),
    GEMU_86BOX_KEEP(sb_t, opl_mix),
    { 0, 0 }
};

static bool sb_state(GemuDevice *d, void *priv) {
    sb_t *sb = priv;
    if (sb->gameport)                  /* (gameport_add stops GEMU before this) */
        return false;
    return gemu_86box_state_blob(d, "sb", sb, sizeof *sb, sb_keep);
}

/* sb_dsp_init() allocates the ESS clones' ESPCM FIFO for every card and
 * sb_dsp_close() is empty in v6.0 (86Box leaks it too): free it here. */
static void sb_free(void *priv) {
    sb_t *sb = priv;
    if (sb->dsp.espcm_fifo) {
        fifo_close(sb->dsp.espcm_fifo);
        sb->dsp.espcm_fifo = NULL;
    }
}

GEMU_86BOX_PORT_CLOSE(gemu_86box_sb1_type, sb_1_device, "sb", sb_state, NULL, sb_free, false);
GEMU_86BOX_PORT_CLOSE(gemu_86box_sb15_type, sb_15_device, "sb1.5", sb_state, NULL, sb_free, false);
GEMU_86BOX_PORT_CLOSE(gemu_86box_sb2_type, sb_2_device, "sb2.0", sb_state, NULL, sb_free, false);
