#pragma once
#include "cdp1802.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct Cdp1861 {
    uint16_t lines_total;    /* 262 = NTSC, 312 = PAL; set before first sync */
    uint16_t line_counter;
    uint8_t  mcycle;         /* machine-cycle counter within scan line (0-13) */
    bool     display_on;     /* set via IN 1 (enable) / OUT 1 (disable) */
    bool     int_fired;      /* true after interrupt fired this frame; gates DMA */
    uint16_t display_addr;   /* byte position within the framebuffer (0..1023) */

    void   (*dma_out_cb)(uint8_t *byte, void *ud);
    void    *dma_out_ud;
} Cdp1861;

void cdp1861_init(Cdp1861 *vdc,
                  void (*dma_out_cb)(uint8_t *, void *), void *dma_ud);
void cdp1861_reset(Cdp1861 *vdc);

/* Called once per CPU machine cycle (from cpu->on_sync). */
void cdp1861_sync(Cdp1861 *vdc, Cdp1802 *cpu);

/*
 * Enable/disable the display.
 *
 * On the COSMAC VIP: IN 1 enables, OUT 1 disables (SC lines of the 1802 drive
 * the CDP1861 enable pin directly). Q output is NOT the display enable —
 * Q drives the audio tone generator.
 */
static inline void cdp1861_set_display(Cdp1861 *vdc, bool on) {
    if (!on) {
        vdc->display_addr = 0;
        vdc->int_fired    = false;
    }
    vdc->display_on = on;
}
