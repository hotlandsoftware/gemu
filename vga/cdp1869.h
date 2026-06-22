#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CDP1869_SCREEN_W 360
#define CDP1869_SCREEN_H 312
#define CDP1869_VISIBLE_W 240
#define CDP1869_VISIBLE_H 216
#define CDP1869_PAGE_RAM_SIZE 0x800
#define CDP1869_CHAR_RAM_SIZE 0x800

/* PAL frame timing (50 Hz, 312 lines × 14 machine cycles) */
#define CDP1869_LINES_TOTAL        312
#define CDP1869_MCYCLES_PER_LINE   14
#define CDP1869_MCYCLES_PER_FRAME  (CDP1869_LINES_TOTAL * CDP1869_MCYCLES_PER_LINE)

/* VBlank from line 0 to CDP1869_DISPLAY_START-1; display for the next VISIBLE_H lines */
#define CDP1869_DISPLAY_START      48
#define CDP1869_DISPLAY_END        (CDP1869_DISPLAY_START + CDP1869_VISIBLE_H)
/* Interrupt fires 2 lines into VBlank, giving the CPU time to update RAM */
#define CDP1869_INT_LINE           2

typedef struct Cdp1869 {
    uint8_t  page_ram[CDP1869_PAGE_RAM_SIZE];
    uint8_t  char_ram[CDP1869_CHAR_RAM_SIZE];
    uint8_t  pcb_ram[CDP1869_CHAR_RAM_SIZE];
    uint8_t  bitmap[CDP1869_VISIBLE_W * CDP1869_VISIBLE_H];

    bool     prd;
    bool     non_display;
    bool     dispoff;
    bool     fresvert;
    bool     freshorz;
    bool     cmem;
    bool     dblpage;
    bool     line16;
    bool     line9;
    bool     cfc;
    uint8_t  col;
    uint8_t  bkg;
    uint16_t pma;
    uint16_t hma;
    uint16_t page_ram_mask;
    uint16_t page_mask;
    uint16_t max_page;
    uint8_t  char_stride;
    bool     block_cpu_access;
    uint8_t  q;
    uint8_t  pcb;
    bool     dirty;

    /* Tone generator (programmed via OUT4 using memory address bus) */
    uint8_t  tone_amp;       /* 0-15 */
    uint8_t  tone_freq_sel;  /* 0-7, prescaler = 512 >> tone_freq_sel */
    bool     tone_off;
    uint8_t  tone_div;       /* 0-127 */

    /* Sync counters (managed by cdp1869_sync) */
    uint16_t lc;   /* current scan line (0..CDP1869_LINES_TOTAL-1) */
    uint8_t  mc;   /* machine-cycle within line (0..CDP1869_MCYCLES_PER_LINE-1) */
} Cdp1869;

void    cdp1869_init(Cdp1869 *vis);
void    cdp1869_reset(Cdp1869 *vis);
void    cdp1869_set_page_ram_mask(Cdp1869 *vis, uint16_t mask);
void    cdp1869_set_char_stride(Cdp1869 *vis, uint8_t stride);
void    cdp1869_set_block_cpu_access(Cdp1869 *vis, bool block);
uint8_t cdp1869_char_read(Cdp1869 *vis, uint16_t offset);
void    cdp1869_char_write(Cdp1869 *vis, uint16_t offset, uint8_t data);
uint8_t cdp1869_page_read(Cdp1869 *vis, uint16_t offset);
void    cdp1869_page_write(Cdp1869 *vis, uint16_t offset, uint8_t data);
void    cdp1869_out(Cdp1869 *vis, uint8_t port, uint16_t ma, uint8_t data);
void    cdp1869_render(Cdp1869 *vis);

/* Called once per CDP1802 machine cycle from cpu->on_sync. */
struct Cdp1802;
void    cdp1869_sync(Cdp1869 *vis, struct Cdp1802 *cpu);
