#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * UM6578/SH6578/NT6578 PPU — an enhanced NES-clone video chip used in cheap
 * "plug & play TV game" cartridges/consoles. Reference: MAME's
 * src/devices/video/ppu2c0x_sh6578.{h,cpp} (subclasses the standard NES
 * ppu2c0x_device, reusing its register/scanline/vblank timing wholesale, but
 * replacing the background/sprite pixel pipeline).
 *
 * Differences from a plain NES RP2C02 (vga/rp2c02.c), all verified against
 * the MAME source:
 *   - No separate CHR ROM/mapper: the PPU owns a flat, CPU-writable 64 KB
 *     VRAM (populated via PPUDATA writes or the machine's DMA controller —
 *     there is no bank-switched pattern table like standard NES mappers).
 *   - No separate attribute table: each nametable entry is 2 bytes
 *     (tile-number-low, tile-number-high-nibble | color-nibble) instead of
 *     NES's 1-byte tile + a shared 64-byte attribute block per nametable.
 *     This gives a 12-bit tile number (4096 tiles) and a 4-bit per-tile
 *     color/attribute (vs. NES's 8-bit tile + 2-bit-per-2x2-tile-block).
 *   - Optional 4bpp background mode (extended register bit 7): doubles the
 *     tile fetch to 4 bit planes (32 bytes/tile) for 16 colors/tile instead
 *     of the normal 2bpp/4-colour tiles.
 *   - A 64-entry (not 32) direct palette RAM at a CPU-mapped register block
 *     separate from PPU address space ($2040-207F, not $3F00 in PPU space).
 *   - A new "extended" register ($2008 on the CPU side) selecting: bit 0 =
 *     alternate/compact nametable window, bits 2-3 = sprite pattern table
 *     4 KB page, bit 7 = 4bpp background mode.
 *   - Sprite pattern page is selected entirely by the extended register
 *     (PPUCTRL's normal sprite-pattern-table bit is ignored); sprite size
 *     (8x8/8x16) and OAM evaluation otherwise match standard NES exactly.
 *
 * Scanline/vblank timing (262 lines NTSC, VBlank at line 241, NMI on
 * PPUCTRL bit7) is numerically identical to a stock NES PPU, so this device
 * renders one whole scanline at a time (matching the reference driver's own
 * batch-style draw_background()/draw_sprites(), which are not cycle/dot
 * exact either) rather than a per-dot shift-register pipeline.
 */

#define SH6578_WIDTH   256
#define SH6578_HEIGHT  240
#define SH6578_VRAM_SIZE 0x10000
#define SH6578_PALETTE_SIZE 0x40

/* $2000 PPUCTRL / $2001 PPUMASK / $2002 PPUSTATUS bit layouts are numerically
 * identical to standard NES — see vga/rp2c02.h's PPUCTRL_, PPUMASK_ and
 * PPUSTAT_ bit-value macros, which ppu_sh6578.c reuses directly (along with
 * the 64-colour NTSC palette table, since this chip's palette RAM holds
 * plain indices into that very same master palette). */

/* $2008 "extended" register (colsel_pntstart) bits */
#define SH6578_EXT_ALT_NAMETABLE  0x01u
#define SH6578_EXT_SPR_PAGE       0x0Cu  /* bits 2-3: sprite pattern page * 0x1000 */
#define SH6578_EXT_4BPP           0x80u

typedef struct Sh6578Ppu {
    uint8_t  vram[SH6578_VRAM_SIZE];
    uint8_t  oam[256];
    uint8_t  palette[SH6578_PALETTE_SIZE];

    uint16_t v;   /* scroll/rendering position, "loopy v" (m_refresh_data) */
    uint16_t t;   /* scroll/rendering latch, "loopy t" (m_refresh_latch) */
    uint8_t  x;   /* fine X scroll */
    bool     w;   /* write toggle (first/second write latch) */
    /* CPU-visible PPUDATA ($2007) address — a field *separate* from v/t
     * (matches MAME's m_videomem_addr): reading/writing tile or palette
     * data via $2007 does not disturb the active scroll position. Synced
     * from the latch only at the moment of a fresh PPUADDR second write. */
    uint16_t vram_addr;

    uint8_t  ppuctrl;
    uint8_t  ppumask;
    uint8_t  ppustatus;
    uint8_t  oamaddr;
    uint8_t  colsel_pntstart;   /* extended register ($2008) */
    uint8_t  read_buf;          /* buffered PPUDATA read */
    uint8_t  data_latch;        /* last value written (returned by write-only reg reads) */

    double   cycle_acc;
    double   cycles_per_scanline;
    int      scanline;
    int      lines_total;   /* 262 NTSC */
    int      vblank_line;   /* 241 NTSC */
    uint64_t frame;
    bool     dirty;          /* set for one tick when a frame completes */
    bool     nmi_pending;    /* edge-triggered: fires once, caller clears */
    bool     scanline_tick;  /* set for one tick on every scanline advance (not
                               * just frame-complete) — drives the $4034 timer's
                               * "Source=Scanline" mode; caller clears it. */

    uint8_t  pixels[SH6578_WIDTH * SH6578_HEIGHT];
    uint32_t pixels_argb[SH6578_WIDTH * SH6578_HEIGHT];
} Sh6578Ppu;

void    ppu_sh6578_init (Sh6578Ppu *ppu, double cpu_clock_hz, double refresh_hz, int lines_total, int vblank_line);
void    ppu_sh6578_reset(Sh6578Ppu *ppu);

uint8_t ppu_sh6578_read (Sh6578Ppu *ppu, uint8_t reg);
void    ppu_sh6578_write(Sh6578Ppu *ppu, uint8_t reg, uint8_t val);

uint8_t ppu_sh6578_read_ext (Sh6578Ppu *ppu);
void    ppu_sh6578_write_ext(Sh6578Ppu *ppu, uint8_t val);

uint8_t ppu_sh6578_palette_read (Sh6578Ppu *ppu, uint8_t offset);
void    ppu_sh6578_palette_write(Sh6578Ppu *ppu, uint8_t offset, uint8_t val);

void    ppu_sh6578_oam_dma(Sh6578Ppu *ppu, const uint8_t *bytes256);

/* Advance the PPU by one CPU cycle. Sets ppu->dirty when a frame completes
 * and ppu->nmi_pending when VBlank NMI should fire (caller must clear both
 * after acting on them, matching the vga/rp2c02.c convention). */
void    ppu_sh6578_tick(Sh6578Ppu *ppu);
