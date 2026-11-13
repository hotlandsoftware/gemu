#pragma once

/* IBM VGA-compatible core shared with a card's linear frame buffer: sequencer
 * (3C4/3C5), graphics controller (3CE/3CF), CRTC (3D4/3D5 or 3B4/3B5),
 * attribute controller (3C0/3C1), DAC (3C6-3C9), the miscellaneous output
 * register and input status 1, plus the four 64 KiB planes behind the
 * 0xA0000-0xBFFFF window and the screen composer for text and 4-plane
 * graphics modes. The card decides when this core's picture is the one
 * displayed.
 *
 * Vga is plain data so a machine can snapshot it as raw bytes. The only
 * member that points at host memory is `vram`: after loading a raw dump, point
 * it at the frame buffer again with vga_attach_vram(). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VGA_PLANE_SIZE      0x10000u
#define VGA_VRAM_MAX        (32u << 20)
#define VGA_VRAM_PAGE_SHIFT 12
#define VGA_VRAM_DIRTY_WORDS ((VGA_VRAM_MAX >> VGA_VRAM_PAGE_SHIFT) / 64)
#define VGA_PLANE_CHUNK_SHIFT 6
#define VGA_PLANE_DIRTY_WORDS ((VGA_PLANE_SIZE >> VGA_PLANE_CHUNK_SHIFT) / 64)

typedef struct Vga {
    uint8_t seq_idx, seq[8];
    uint8_t gc_idx, gc[16];
    uint8_t crtc_idx, crtc[32];
    uint8_t attr_idx, attr[32];
    bool attr_flipflop;          /* 3C0 alternates index and data */
    uint8_t misc;                /* miscellaneous output (3C2 write, 3CC read) */
    uint8_t dac[256 * 3];        /* 6-bit components */
    uint8_t dac_idx, dac_sub;    /* write address (3C8) and component */
    uint8_t dac_ridx, dac_rsub;  /* read address (3C7) and component */
    uint8_t pixel_mask;          /* 3C6 */
    bool dac_reading;            /* last address written was the read address */

    /* Four planes behind the legacy window, and the read latches. The
     * graphics controller's write mode, bit mask, set/reset and map mask
     * decide how a CPU byte reaches them. */
    uint8_t plane[4][VGA_PLANE_SIZE];
    uint8_t latch[4];
    bool font_uploaded;          /* guest loaded a character generator into plane 2 */

    /* Input status 1 (3DA) as the beam currently shows it: bit 3 vertical
     * retrace, bit 0 display disabled. The card sets this before a read. */
    uint8_t retrace;

    /* Redraw hints for the renderer, not guest state. disp_gen moves whenever
     * a card register that changes the picture is written; vram_dirty has one
     * bit per 4 KiB page of VRAM written; plane_dirty one bit per 64 bytes of
     * plane offset written through the legacy window. */
    uint64_t disp_gen;
    uint64_t vram_dirty[VGA_VRAM_DIRTY_WORDS];
    uint64_t plane_dirty[VGA_PLANE_DIRTY_WORDS];

    size_t vram_size;
    uint8_t *vram;               /* HOST POINTER: the card's frame buffer */
} Vga;

/* Power-on state with `vram` (at most VGA_VRAM_MAX bytes) as the frame buffer.
 * VRAM contents are left as they are. */
void vga_init(Vga *vg, uint8_t *vram, size_t vram_size);
/* Re-attach the frame buffer after loading a raw dump; redraws everything. */
void vga_attach_vram(Vga *vg, uint8_t *vram, size_t vram_size);
/* Reset: registers to power-on; VRAM and planes keep their contents. */
void vga_reset(Vga *vg);

/* Every VRAM store goes through here. */
static inline void vga_vram_written(Vga *vg, uint64_t off, unsigned n) {
    if (!n) {
        return;
    }
    for (uint64_t p = off >> VGA_VRAM_PAGE_SHIFT;
         p <= (off + n - 1u) >> VGA_VRAM_PAGE_SHIFT; p++) {
        vg->vram_dirty[p >> 6] |= UINT64_C(1) << (p & 63u);
    }
}
/* Any VRAM page written since the last call; clears them all. */
bool vga_vram_take_any(Vga *vg);
/* Mark the whole picture changed. */
void vga_touch_all(Vga *vg);

/* Register ports 3B0-3DF. Returns false for a port this core does not decode
 * (the mono or colour CRTC set that Misc Output bit 0 deselects, ports it
 * has no register at). `len` is the access width in bytes and matters: an
 * index port takes `out dx,ax`, writing index and data in one 16-bit access.
 * 3C3 is card-specific and not decoded here. */
bool vga_io(Vga *vg, bool write, uint16_t port, unsigned len, uint64_t *val);

/* The 0xA0000-0xBFFFF window. `off` is relative to 0xA0000. */
uint8_t vga_mem_read(Vga *vg, uint32_t off);
void vga_mem_write(Vga *vg, uint32_t off, uint8_t val);

/* Display state. */
bool vga_text_mode(const Vga *vg);
bool vga_blanked(const Vga *vg);
void vga_text_geometry(const Vga *vg, unsigned *cols, unsigned *rows,
                       unsigned *cell_w, unsigned *cell_h);

/* Compose the screen into an ARGB8888 frame buffer of fb_w x fb_h at its
 * origin. `w`/`h` are the fallback size for an unprogrammed CRTC and `xscale`
 * repeats each pixel horizontally. Returns false (leaving fb alone) when the
 * display is blanked. The picture size is stored through out_w and out_h,
 * which are required. */
bool vga_blit(const Vga *vg, uint32_t *fb, uint32_t fb_w, uint32_t fb_h,
              uint32_t w, uint32_t h, unsigned xscale, uint32_t *out_w,
              uint32_t *out_h);
/* The same, for a frame buffer that still holds the previous picture: unless
 * `full`, a planar graphics mode redraws only the rows over plane chunks
 * written since the last call (text mode redraws whole), and the written
 * chunks are consumed. [*y0, *y1) is the band drawn. */
bool vga_blit_ex(Vga *vg, uint32_t *fb, uint32_t fb_w, uint32_t fb_h,
                 uint32_t w, uint32_t h, unsigned xscale, uint32_t *out_w,
                 uint32_t *out_h, bool full, uint32_t *y0, uint32_t *y1);
/* Any plane chunk written since the last vga_blit_ex. */
bool vga_planes_dirty(const Vga *vg);
/* Changes exactly when a register the renderer reads changes. */
uint64_t vga_render_key(const Vga *vg);
