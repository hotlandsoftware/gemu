#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RAGE128_VENDOR_ID    0x1002u
#define RAGE128_DEVICE_ID    0x5046u
#define RAGE128_FB_APER_SIZE 0x04000000u
#define RAGE128_VRAM_SIZE    0x00800000u
#define RAGE128_MMIO_SIZE    0x4000u

/* Reusable minimal ATI Rage 128 PCI display device. The host machine owns
 * address decoding and supplies its preferred initial BAR locations. */
typedef struct Rage128 {
    bool enabled;
    uint8_t cfg[256];
    uint8_t mmio[RAGE128_MMIO_SIZE];
    uint8_t vram[RAGE128_VRAM_SIZE];
} Rage128;

void rage128_init(Rage128 *r, bool enabled, uint32_t fb_base,
                  uint32_t io_base, uint32_t mmio_base, uint8_t irq);

uint64_t rage128_pci_read(const Rage128 *r, unsigned reg, unsigned size);
void rage128_pci_write(Rage128 *r, unsigned reg, uint64_t val,
                       unsigned size);
uint32_t rage128_bar(const Rage128 *r, unsigned reg, uint32_t fallback,
                     uint32_t mask);

uint64_t rage128_mmio_read(const Rage128 *r, unsigned off, unsigned size);
void rage128_mmio_write(Rage128 *r, unsigned off, uint64_t val,
                        unsigned size);
uint64_t rage128_vram_read(const Rage128 *r, uint32_t off, unsigned size);
void rage128_vram_write(Rage128 *r, uint32_t off, uint64_t val,
                        unsigned size);
uint32_t rage128_reg32(const Rage128 *r, unsigned off);

void rage128_program_mode(Rage128 *r, unsigned width, unsigned height,
                          unsigned bpp, bool no_clear);
bool rage128_render(const Rage128 *r, uint32_t *argb,
                    unsigned output_width, unsigned output_height);
size_t rage128_nonzero_vram(const Rage128 *r);
