/* Intel 460GX firmware hub interface (SSDM 11.1.11, 11.1.27).
 *
 * The board carries four 82802AC firmware hubs of 1 MB each for the 4 MB
 * below 4 GB. The IFB splits FFC00000-FFFFFFFF into eight 512 KB granules:
 * FWHDE (function 0, E3h) enables each, FWHS (E8h) names the FWH ID that
 * answers it, and the same granule 4 MB lower (FF800000-FFBFFFFF) is that
 * part's register space. BIOSEN (4Eh) bit 2 gates writes to the memory range
 * only: there is no protection on writes to FWH register space.
 *
 * FWHS defaults to 00112233h (FFF/FFE8 ID 0, FFE ID 1, FFD ID 2, FFC ID 3), so
 * with a top-aligned 4 MB image ID n holds megabyte 3-n. The board owns the
 * image; every part's array is the board's flash. */
#include "i460gx_internal.h"

#include <stdio.h>

enum {
    CFG_FWHDE = 0xe3, CFG_FWHS = 0xe8, CFG_BIOSEN = 0x4e,
    BIOSEN_WEN = 0x04,
    GRANULE_SHIFT = 19,
    REPORTS = 8,
};

static uint64_t now_ns(const I460GX *s) {
    uint64_t ticks = s->config.clock ? s->config.clock(s->config.opaque) : 0;
    uint64_t hz = s->config.clock_hz;
    return (ticks / hz) * UINT64_C(1000000000) +
           (ticks % hz) * UINT64_C(1000000000) / hz;
}

static void report(const I460GX *s, unsigned *count, bool reg, bool write,
                   uint32_t addr, const char *why) {
    if (!s->config.fwh_debug || (*count)++ >= REPORTS)
        return;
    fprintf(stderr, "i460gx: FWH %s %s %#010x %s\n", reg ? "register" : "memory",
            write ? "write" : "read", addr, why);
}

void i460gx_fwh_reset(I460GX *s) {
    for (unsigned id = 0; id < I460GX_FWH_PARTS; id++)
        fwh82802_init(&s->fwh[id], 0x1f, s->config.fwh_debug);
    s->flash_changed_len = 0;
}

bool i460gx_flash_range(uint64_t addr) {
    return addr >= I460GX_FWH_REG_BASE &&
           addr < I460GX_FLASH_BASE + I460GX_FLASH_SIZE;
}

/* One byte of an FWH cycle at 32-bit address `a`. A cycle the IFB does not
 * decode to the FWH bus is master-aborted: reads return all ones. */
static void fwh_byte(I460GX *s, bool reg, bool write, uint32_t a, uint8_t *value) {
    static unsigned reports;
    const uint8_t *lpc = s->ifb_cfg;
    unsigned granule = (a - (uint32_t)I460GX_FLASH_BASE) >> GRANULE_SHIFT;
    uint32_t fwhs = (uint32_t)lpc[CFG_FWHS] | (uint32_t)lpc[CFG_FWHS + 1] << 8 |
                    (uint32_t)lpc[CFG_FWHS + 2] << 16 |
                    (uint32_t)lpc[CFG_FWHS + 3] << 24;
    unsigned id = (fwhs >> (4 * granule)) & 0xf;
    if (!((lpc[CFG_FWHDE] >> granule) & 1) || id >= I460GX_FWH_PARTS ||
        !s->config.flash) {
        report(s, &reports, reg, write, a, "is not decoded: master abort");
        if (!write)
            *value = 0xff;
        return;
    }
    if (write && !reg && !(lpc[CFG_BIOSEN] & BIOSEN_WEN)) {
        report(s, &reports, reg, write, a, "with BIOS_WEN clear: master abort");
        return;
    }
    Fwh82802 *fwh = &s->fwh[id];
    uint8_t *array = s->config.flash + (size_t)(I460GX_FWH_PARTS - 1 - id) *
                                       FWH82802_SIZE;
    fwh->changed_len = 0;
    fwh82802_access(fwh, array, now_ns(s), reg, write,
                    a & (FWH82802_SIZE - 1), value);
    if (fwh->changed_len) {
        /* Remember what the firmware programmed or erased, as image offsets. */
        uint32_t lo = (uint32_t)((I460GX_FWH_PARTS - 1 - id) * FWH82802_SIZE) +
                      fwh->changed_off;
        uint32_t hi = lo + fwh->changed_len;
        if (s->flash_changed_len) {
            uint32_t old_hi = s->flash_changed_off + s->flash_changed_len;
            if (s->flash_changed_off < lo) lo = s->flash_changed_off;
            if (old_hi > hi) hi = old_hi;
        }
        s->flash_changed_off = lo;
        s->flash_changed_len = hi - lo;
    }
}

bool i460gx_flash_access(I460GX *s, bool write, uint64_t addr, unsigned size,
                         uint64_t *value) {
    if (!i460gx_flash_range(addr) || !i460gx_access_size_valid(size))
        return false;
    bool reg = addr < I460GX_FLASH_BASE;
    uint32_t a = (uint32_t)(reg ? addr + I460GX_FLASH_SIZE : addr);
    uint64_t out = 0;
    /* The FWH protocol moves one byte per cycle; a wider access is split. */
    for (unsigned i = 0; i < size && (uint64_t)a + i <= UINT32_MAX; i++) {
        uint8_t byte = write ? (uint8_t)(*value >> (8 * i)) : 0xff;
        fwh_byte(s, reg, write, a + i, &byte);
        out |= (uint64_t)byte << (8 * i);
    }
    if (!write)
        *value = out;
    return true;
}

bool i460gx_flash_take_changes(I460GX *s, uint32_t *offset, uint32_t *length) {
    if (!s->flash_changed_len)
        return false;
    *offset = s->flash_changed_off;
    *length = s->flash_changed_len;
    s->flash_changed_len = 0;
    return true;
}
