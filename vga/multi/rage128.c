#include "rage128.h"

#include <string.h>

static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, sizeof(v)); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, sizeof(v)); }

void rage128_init(Rage128 *r, bool enabled, uint32_t fb_base,
                  uint32_t io_base, uint32_t mmio_base, uint8_t irq) {
    memset(r, 0, sizeof(*r));
    r->enabled = enabled;
    if (!enabled)
        return;
    put16(r->cfg + 0x00, RAGE128_VENDOR_ID);
    put16(r->cfg + 0x02, RAGE128_DEVICE_ID);
    put16(r->cfg + 0x04, 0x0003);             /* I/O + memory decode */
    r->cfg[0x0b] = 0x03;                      /* display/VGA class */
    put32(r->cfg + 0x10, fb_base | 0x08u);    /* prefetchable */
    put32(r->cfg + 0x14, io_base | 1u);
    put32(r->cfg + 0x18, mmio_base);
    put16(r->cfg + 0x2c, 0x1af4);
    put16(r->cfg + 0x2e, 0x1100);
    r->cfg[0x3c] = irq;
    r->cfg[0x3d] = 1;
}

uint64_t rage128_pci_read(const Rage128 *r, unsigned reg, unsigned size) {
    uint64_t v = 0;
    if (reg + size > sizeof(r->cfg))
        return UINT64_MAX;
    memcpy(&v, r->cfg + reg, size);
    if (size == 4 && reg == 0x10 && (uint32_t)v == UINT32_MAX)
        return 0xfc000008u;
    if (size == 4 && reg == 0x14 && (uint32_t)v == UINT32_MAX)
        return 0xffffff01u;
    if (size == 4 && reg == 0x18 && (uint32_t)v == UINT32_MAX)
        return 0xffffc000u;
    return v;
}

void rage128_pci_write(Rage128 *r, unsigned reg, uint64_t val,
                       unsigned size) {
    if (reg + size > sizeof(r->cfg))
        return;
    for (unsigned i = 0; i < size; i++) {
        unsigned off = reg + i;
        if (off >= 4 && !(off >= 8 && off < 16) && off < 0x40)
            r->cfg[off] = (uint8_t)(val >> (i * 8));
    }
}

uint32_t rage128_bar(const Rage128 *r, unsigned reg, uint32_t fallback,
                     uint32_t mask) {
    uint32_t bar = 0;
    memcpy(&bar, r->cfg + reg, sizeof(bar));
    if (bar == UINT32_MAX || !(bar & mask))
        return fallback;
    return bar & mask;
}

uint32_t rage128_reg32(const Rage128 *r, unsigned off) {
    uint32_t v = 0;
    if (off + sizeof(v) <= sizeof(r->mmio))
        memcpy(&v, r->mmio + off, sizeof(v));
    return v;
}

uint64_t rage128_mmio_read(const Rage128 *r, unsigned off, unsigned size) {
    uint64_t v = 0;
    if (off == 0x00f8 && size == 4)               /* CONFIG_MEMSIZE */
        return RAGE128_VRAM_SIZE;
    if (off + size <= sizeof(r->mmio))
        memcpy(&v, r->mmio + off, size);
    return v;
}

void rage128_mmio_write(Rage128 *r, unsigned off, uint64_t val,
                        unsigned size) {
    if (off + size <= sizeof(r->mmio))
        memcpy(r->mmio + off, &val, size);
}

uint64_t rage128_vram_read(const Rage128 *r, uint32_t off, unsigned size) {
    uint64_t v = 0;
    off &= RAGE128_VRAM_SIZE - 1;
    if (off + size <= RAGE128_VRAM_SIZE)
        memcpy(&v, r->vram + off, size);
    return v;
}

void rage128_vram_write(Rage128 *r, uint32_t off, uint64_t val,
                        unsigned size) {
    off &= RAGE128_VRAM_SIZE - 1;
    if (off + size <= RAGE128_VRAM_SIZE)
        memcpy(r->vram + off, &val, size);
}

void rage128_program_mode(Rage128 *r, unsigned width, unsigned height,
                          unsigned bpp, bool no_clear) {
    unsigned fmt = bpp == 16 ? 4 : bpp == 24 ? 5 : 6;
    put32(r->mmio + 0x0050, fmt << 8);
    put32(r->mmio + 0x0200, ((width / 8) - 1) << 16);
    put32(r->mmio + 0x0208, (height - 1) << 16);
    put32(r->mmio + 0x0224, 0);
    put32(r->mmio + 0x022c, (width / 8) & 0x7ff);
    if (!no_clear)
        memset(r->vram, 0, sizeof(r->vram));
}

bool rage128_render(const Rage128 *r, uint32_t *argb,
                    unsigned output_width, unsigned output_height) {
    unsigned fmt = (rage128_reg32(r, 0x0050) >> 8) & 7;
    if (!r->enabled || fmt < 3)
        return false;
    unsigned w = (((rage128_reg32(r, 0x0200) >> 16) & 0x7ff) + 1) * 8;
    unsigned h = ((rage128_reg32(r, 0x0208) >> 16) & 0xfff) + 1;
    unsigned bpp = fmt <= 4 ? 2 : fmt == 5 ? 3 : 4;
    unsigned pitch = (rage128_reg32(r, 0x022c) & 0x7ff) * 8;
    unsigned start = rage128_reg32(r, 0x0224) & 0x07ffffff;
    if (!w || w > 2048) w = 640;
    if (!h || h > 1536) h = 480;
    if (!pitch) pitch = w;
    for (unsigned y = 0; y < output_height; y++) {
        unsigned sy = y * h / output_height;
        for (unsigned x = 0; x < output_width; x++) {
            unsigned sx = x * w / output_width;
            size_t off = start + ((size_t)sy * pitch + sx) * bpp;
            uint32_t rgb = 0;
            if (off + bpp <= RAGE128_VRAM_SIZE) {
                const uint8_t *p = r->vram + off;
                if (bpp == 2) {
                    uint16_t q;
                    memcpy(&q, p, sizeof(q));
                    if (fmt == 3)
                        rgb = ((q & 0x7c00) << 9) |
                              ((q & 0x03e0) << 6) | ((q & 0x001f) << 3);
                    else
                        rgb = ((q & 0xf800) << 8) |
                              ((q & 0x07e0) << 5) | ((q & 0x001f) << 3);
                } else {
                    rgb = (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
                }
            }
            argb[y * output_width + x] = 0xff000000u | rgb;
        }
    }
    return true;
}

size_t rage128_nonzero_vram(const Rage128 *r) {
    size_t n = 0;
    for (size_t i = 0; i < sizeof(r->vram); i++)
        n += r->vram[i] != 0;
    return n;
}
