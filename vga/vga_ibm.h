#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Standard IBM-compatible VGA: sequencer, CRTC, graphics controller,
 * attribute controller and DAC registers, plus the classic 4-plane x 64KiB
 * planar VRAM and its write-mode/read-mode address logic. Register
 * semantics follow the standard VGA programming reference (cross-checked
 * against real QEMU's hw/display/vga.c, which is generic PC hardware
 * behavior, not IA-64-specific - unlike this project's other qemu-ia64
 * reference material, this part is trustworthy). */

typedef struct VgaIbm {
    uint8_t vram[4][0x10000];

    uint8_t seq_index;
    uint8_t seq[8];

    uint8_t crtc_index;
    uint8_t crtc[32];

    uint8_t gc_index;
    uint8_t gc[16];

    uint8_t attr_index;
    uint8_t attr[32];
    bool    attr_flipflop;   /* false = next 0x3C0 write is an index */

    uint8_t dac[256][3];
    uint8_t dac_read_index, dac_write_index;
    unsigned dac_read_sub, dac_write_sub;
    uint8_t dac_mask;

    uint8_t misc_output;

    uint8_t latch[4];

    uint32_t retrace_toggle;
} VgaIbm;

void    vga_ibm_reset(VgaIbm *v);
uint8_t vga_ibm_io_read(VgaIbm *v, uint16_t port);
void    vga_ibm_io_write(VgaIbm *v, uint16_t port, uint8_t val);

/* addr is relative to the currently selected VRAM aperture - see
 * vga_ibm_aperture() below, which tells the caller where that aperture
 * currently sits in CPU physical address space. */
uint8_t vga_ibm_mem_read(VgaIbm *v, uint32_t addr);
void    vga_ibm_mem_write(VgaIbm *v, uint32_t addr, uint8_t val);

/* Graphics Controller register 6 bits 3:2 select which slice of CPU
 * address space the VRAM window currently answers to. Returns the base
 * physical address and window size (both real VGA conventions). */
void vga_ibm_aperture(const VgaIbm *v, uint32_t *base, uint32_t *size);

/* True once the guest has programmed a real mode (sequencer/CRTC touched
 * past their power-on state) - used to decide whether to show real VGA
 * output yet or fall back to a placeholder. */
bool    vga_ibm_mode_set(const VgaIbm *v);
bool    vga_ibm_is_text_mode(const VgaIbm *v);

/* Renders the current mode into an ARGB buffer of the given dimensions,
 * letterboxed/centered as needed. font8x16 is 256 glyphs * 16 bytes
 * (one byte per scanline, MSB = leftmost pixel), the standard VGA ROM font
 * layout. */
void vga_ibm_render(const VgaIbm *v, uint32_t *argb, int fb_w, int fb_h,
                    const uint8_t *font8x16);
