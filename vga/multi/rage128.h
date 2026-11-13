#pragma once

/* ATI Rage 128 GL PCI (1002:5245), the display card of the Intel IA-64
 * Workstation SDV, modelled well enough for its own x86 video BIOS (ATI
 * 113-53008-100) to POST and for a native-mode driver to set modes.
 *
 * "RRG" in the comments is the RAGE 128 VR/GL Register Reference Guide
 * (RRG-R04100). The card owns its PCI configuration space and its address
 * decode: the host feeds it configuration cycles, memory and I/O accesses
 * and asks which memory ranges the BARs currently cover. The VGA-compatible
 * core (vga.h) serves the legacy ports and the 0xA0000 window, and its VRAM is
 * the card's frame buffer.
 *
 * Rage128 is plain data so a machine can snapshot it as raw bytes. The host
 * pointers inside it are `wiring` (three function pointers and an opaque),
 * `vga` and `rom`; after loading a raw dump call rage128_rebind() and
 * rage128_set_rom() to point them at the live objects again. */

#include "vga.h"

#include <stdbool.h>
#include <stdint.h>

#define RAGE128_VENDOR_ID 0x1002u
#define RAGE128_DEVICE_ID 0x5245u
#define RAGE128_VRAM_SIZE 0x00800000u   /* default frame buffer: 8 MiB */

/* BARs (RRG 4.1): MEM_BASE 64 MiB prefetchable, IO_BASE 256 bytes, REG_BASE
 * 16 KiB, BIOS_ROM 128 KiB. */
#define RAGE128_FB_BAR_SIZE  0x04000000u
#define RAGE128_IO_BAR_SIZE  0x100u
#define RAGE128_REG_BAR_SIZE 0x4000u
#define RAGE128_ROM_BAR_SIZE 0x20000u

/* The REG_BASE BAR holds two identical register apertures of
 * CONFIG_REG_APER_SIZE = 8 KiB (RRG 2.2). */
#define RAGE128_REG_APER_SIZE 0x2000u

/* A draw waiting for its source pixels through HOST_DATA (rage128_gui.c). */
typedef struct R128HostOp {
    bool active;
    int x0, x, y, w, h, xdir, ydir;   /* next pixel (x, y), row start x0 */
    int left;                         /* pixels left in the row */
    bool byte_aligned;                /* DP_SRC_SOURCE 4: rows start on a byte */
    unsigned bits;                    /* mono: bits of the current row consumed */
    unsigned skip;                    /* mono: padding bits still to skip */
} R128HostOp;

/* Both callbacks are required. */
typedef struct {
    uint64_t (*clock)(void *opaque);   /* monotonic ticks at clock_hz, drives vblank/vline timing */
    uint64_t clock_hz;
    void (*irq)(void *opaque, bool level);   /* INTA# level changes */
    void *opaque;
} Rage128Wiring;

typedef struct Rage128 {
    Rage128Wiring wiring;             /* HOST POINTERS: clock, irq, opaque */
    Vga *vga;                         /* HOST POINTER, borrowed */
    const uint8_t *rom;               /* HOST POINTER, borrowed: BIOS image */
    uint32_t rom_len;
    uint16_t device_id;               /* power-on DEVICE_ID; reset applies a change */
    uint8_t revision;                 /* power-on REVISION_ID (RRG default 00) */
    uint8_t cfg[256];                 /* PCI configuration space */
    uint32_t regs[RAGE128_REG_APER_SIZE / 4];
    uint32_t pll[64];
    uint8_t palette[256][3];
    R128HostOp host;
    /* The pixel count (since clock 0) at which CRTC_STATUS CRTC_VBLANK_SAVE
     * and GEN_INT_STATUS CRTC_VBLANK_INT / CRTC_VLINE_INT were last cleared. */
    uint64_t vblank_save_clear, int_vblank_clear, int_vline_clear;
    bool irq_out;                     /* INTA# as last reported */
} Rage128;

/* Power-on. `vga` is borrowed and must already be initialised with
 * vga_init(); its VRAM (vga->vram, vga->vram_size) is the frame buffer. */
void rage128_init(Rage128 *r, Vga *vga, const Rage128Wiring *wiring);
/* Re-point the host pointers after loading a raw dump; changes no state. */
void rage128_rebind(Rage128 *r, Vga *vga, const Rage128Wiring *wiring);
/* PCIRST#: configuration space, registers, PLLs, palette and the VGA core's
 * registers return to power-on, INTA# is released; the frame buffer keeps its
 * contents. */
void rage128_reset(Rage128 *r);
/* The 128 KiB video BIOS image (borrowed, `len` clamped to the BAR). Until
 * one is set the ROM BAR does not exist (reads as zero, ignores writes). */
void rage128_set_rom(Rage128 *r, const uint8_t *rom, uint32_t len);

/* Configuration cycles; `len` is 1, 2 or 4 within one dword. */
uint32_t rage128_cfg_read(const Rage128 *r, unsigned reg, unsigned len);
void rage128_cfg_write(Rage128 *r, unsigned reg, unsigned len, uint32_t val);

/* Address decode. Both return false when the card does not claim the access
 * (COMMAND.MEM / COMMAND.IO, the BARs as currently programmed and the ROM
 * BAR's enable bit decide). `len` is 1, 2, 4 or 8; 8 is split into two
 * dwords. */
/* BAR0 frame buffer, BAR2 registers, the enabled ROM BAR and the legacy
 * 0xA0000-0xBFFFF window. */
bool rage128_mem_access(Rage128 *r, bool write, uint64_t addr, unsigned len, uint64_t *val);
/* BAR1 block I/O and the legacy ports 3B0-3DF. */
bool rage128_io_access(Rage128 *r, bool write, uint16_t port, unsigned len, uint64_t *val);

/* Guest address ranges the memory BARs currently cover, so the host can keep
 * its RAM fast path clear of them. Returns false while memory decode is off
 * (COMMAND.MEM clear); otherwise a size of 0 means that BAR is not placed. */
bool rage128_mem_ranges(const Rage128 *r, uint64_t *fb_base, uint64_t *fb_size,
                        uint64_t *reg_base, uint64_t *reg_size);
/* The same for the expansion ROM window; false unless it is enabled. */
bool rage128_rom_range(const Rage128 *r, uint64_t *base, uint64_t *size);

/* Bring INTA# up to date with the raster (VBLANK/VLINE interrupts). Call
 * periodically; cheap when no interrupt is enabled. */
void rage128_tick(Rage128 *r);

/* Native (extended) CRTC scan-out into an ARGB8888 frame buffer with the
 * hardware cursor; false while the CRTC shows the VGA core's picture (the
 * caller renders that with vga_blit_ex). out_w/out_h/y0/y1 are required;
 * [*y0, *y1) is the band redrawn. */
bool rage128_scanout(Rage128 *r, uint32_t *fb, uint32_t fb_w, uint32_t fb_h,
                     uint32_t *out_w, uint32_t *out_h, bool full,
                     uint32_t *y0, uint32_t *y1);
