#include "cdp1861.h"
#include <string.h>

void cdp1861_init(Cdp1861 *vdc,
                  void (*dma_out_cb)(uint8_t *, void *), void *dma_ud) {
    memset(vdc, 0, sizeof(*vdc));
    vdc->lines_total = CDP1861_LINES_TOTAL; /* default: NTSC */
    vdc->dma_out_cb  = dma_out_cb;
    vdc->dma_out_ud  = dma_ud;
}

void cdp1861_reset(Cdp1861 *vdc) {
    vdc->line_counter = 0;
    vdc->mcycle       = 0;
    vdc->display_addr = 0;
    vdc->display_on   = false;
    vdc->int_fired    = false;
}

void cdp1861_sync(Cdp1861 *vdc, Cdp1802 *cpu) {
    uint16_t lc = vdc->line_counter;
    uint8_t  mc = vdc->mcycle;

    /*
     * EF1 (EFX pin, active-low): driven LOW (EF=0) during the blanking
     * windows near frame boundaries; HIGH (EF=1) during active display.
     * This is updated every machine cycle.
     */
    {
        bool blanking = (lc >= 60 && lc <= 63) || (lc >= 188 && lc <= 191);
        cpu->EF[0] = !blanking;   /* EF1 = 1 active, 0 blanking */
    }

    /*
     * Interrupt fires at machine cycle 2 of line 62 unconditionally —
     * the real CDP1861 always asserts INT at this point regardless of the
     * DISP (display_on) state.  Gating it on display_on causes a deadlock
     * on machines (e.g. Pecom 32) where the ISR is the only code that
     * calls INP to assert DISP.
     */
    if (lc == 62 && mc == 2) {
        cdp1802_request_irq(cpu);
        vdc->int_fired = true;
    }

    /*
     * DMA-out at machine cycle 4 of each active line, but ONLY after the
     * frame interrupt has fired and display is enabled.  This mirrors
     * Emma02's vidInt_ gate — without it the first DMA fires with an
     * uninitialised R[0].
     */
    if (vdc->display_on && vdc->int_fired &&
        lc >= CDP1861_FIRST_LINE && lc <= CDP1861_LAST_LINE && mc == 4)
        cdp1802_request_dma_out(cpu, 8, vdc->dma_out_cb, vdc->dma_out_ud);

    if (++vdc->mcycle == CDP1861_MCYCLES_PER_LINE) {
        vdc->mcycle = 0;
        if (++vdc->line_counter == vdc->lines_total)
            vdc->line_counter = 0;
    }
}
