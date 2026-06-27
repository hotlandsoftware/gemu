#pragma once
#include <stdbool.h>
#include <stdint.h>

#define RP2C02_WIDTH          256
#define RP2C02_HEIGHT         240
#define RP2C02_LINES_TOTAL    262
#define RP2C02_DOTS_PER_LINE  341

/* PPUCTRL (0x2000) bits */
#define PPUCTRL_NT_SELECT  0x03u
#define PPUCTRL_VRAM_INC   0x04u
#define PPUCTRL_SPR_PT     0x08u
#define PPUCTRL_BG_PT      0x10u
#define PPUCTRL_SPR_8x16   0x20u
#define PPUCTRL_NMI_EN     0x80u

/* PPUMASK (0x2001) bits */
#define PPUMASK_GRAY       0x01u
#define PPUMASK_BG_LEFT    0x02u
#define PPUMASK_SPR_LEFT   0x04u
#define PPUMASK_SHOW_BG    0x08u
#define PPUMASK_SHOW_SPR   0x10u

/* PPUSTATUS (0x2002) bits */
#define PPUSTAT_SPR_OVF    0x20u
#define PPUSTAT_SPR0_HIT   0x40u
#define PPUSTAT_VBLANK     0x80u

/* Nametable mirroring modes */
#define RP2C02_MIRROR_HORIZONTAL  0
#define RP2C02_MIRROR_VERTICAL    1
#define RP2C02_MIRROR_SINGLE_A    2
#define RP2C02_MIRROR_SINGLE_B    3
#define RP2C02_MIRROR_4SCREEN     4

typedef struct Rp2c02 {
    uint8_t  vram[0x800];
    uint8_t  oam[256];
    uint8_t  palette[32];

    uint16_t v;
    uint16_t t;
    uint8_t  x;
    bool     w;

    uint8_t  ppuctrl;
    uint8_t  ppumask;
    uint8_t  ppumask_pending;
    int      ppumask_delay;
    uint8_t  ppustatus;
    uint8_t  oamaddr;
    uint8_t  read_buf;
    uint8_t  open_bus;

    int      scanline;
    int      dot;
    int      lines_total;  /* 262 NTSC, 312 PAL/Dendy */
    int      vblank_line;  /* scanline where VBlank/NMI fires: 241 NTSC/PAL, 291 Dendy */
    uint64_t frame;
    bool     odd_frame;

    bool     nmi_pending;

    uint8_t  nt_latch;
    uint8_t  at_latch;
    uint8_t  pt_lo_latch;
    uint8_t  pt_hi_latch;
    uint16_t bg_shift_lo;
    uint16_t bg_shift_hi;
    uint16_t bg_attr_lo;
    uint16_t bg_attr_hi;
    uint8_t  at_latch_lo;
    uint8_t  at_latch_hi;

    uint8_t  oam2[32];
    uint8_t  n_spr;
    bool     spr0_next;
    bool     spr0_active;
    bool     spr_is_zero[8];
    uint8_t  spr_shift_lo[8];
    uint8_t  spr_shift_hi[8];
    uint8_t  spr_attr[8];
    uint8_t  spr_x[8];

    uint8_t  pixels[RP2C02_WIDTH * RP2C02_HEIGHT];
    uint32_t pixels_argb[RP2C02_WIDTH * RP2C02_HEIGHT];

    /* Optional alternate palette (e.g. 2C04-0004 for VS. System).
     * If non-NULL, points to 64 ARGB values used instead of rp2c02_palette_rgb. */
    const uint32_t *alt_palette_rgb;
    bool     dirty;

    uint8_t  mirror;

    uint8_t (*chr_read) (uint16_t addr, void *ud);
    void    (*chr_write)(uint16_t addr, uint8_t val, void *ud);
    void    *chr_ud;

    uint8_t (*nt_read) (uint16_t addr, void *ud);
    void    (*nt_write)(uint16_t addr, uint8_t val, void *ud);
    void    *nt_ud;

    void (*irq_scanline)(void *ud);
    void *irq_ud;

    bool debug;
} Rp2c02;

extern const uint32_t rp2c02_palette_rgb[64];

void    rp2c02_init(Rp2c02 *ppu, bool is_pal);
void    rp2c02_reset(Rp2c02 *ppu);

uint8_t rp2c02_read(Rp2c02 *ppu, uint8_t reg);
void    rp2c02_write(Rp2c02 *ppu, uint8_t reg, uint8_t val);

void    rp2c02_tick(Rp2c02 *ppu);
void    rp2c02_oam_dma(Rp2c02 *ppu, const uint8_t *page);
