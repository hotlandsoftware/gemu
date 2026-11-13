/* Intel 460GX SAC/SDC, expander and IFB register model.
 * Register definitions follow Intel 460GX SSDM 248704-001. Board wiring,
 * attached EEPROMs and processor interrupt delivery are supplied externally.
 * Configuration space lives in i460gx_pci.c.
 */
#include "i460gx_internal.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { IOSAPIC_REGSEL_OFF = 0x00, IOSAPIC_WINDOW_OFF = 0x10 };

static uint64_t clock_now(I460GX *s) {
    return s->config.clock ? s->config.clock(s->config.opaque) : 0;
}

void i460gx_uhci_reset(I460GX *s) {
    memset(s->uhci_regs, 0, sizeof(s->uhci_regs));
    uint8_t *regs = (uint8_t *)s->uhci_regs;
    regs[12] = 0x40;                /* SOFMOD reset value */
    regs[16] = regs[18] = 0x80;     /* two disconnected root-hub ports */
    s->uhci_frame_itc = 0;
}

static bool uhci_port_offset(I460GX *s, uint64_t port,
                             unsigned size, unsigned *off) {
    uint32_t bar = (uint32_t)i460gx_ifb_cfg_read8(s, 2, 0x20) |
                   (uint32_t)i460gx_ifb_cfg_read8(s, 2, 0x21) << 8;
    unsigned base = bar & 0xffe0;
    if (!(s->ifb_usb_cfg[4] & 1) || !(bar & 1) || !base ||
        port < base || port - base > sizeof(s->uhci_regs) - size)
        return false;
    *off = (unsigned)(port - base);
    return true;
}

static void uhci_update_frames(I460GX *s) {
    uint8_t *regs = (uint8_t *)s->uhci_regs;
    uint64_t now = clock_now(s);
    if (regs[0] & 1) {
        uint64_t frames = (now - s->uhci_frame_itc) / (s->config.clock_hz / 1000);
        unsigned frame = ((unsigned)regs[6] | (unsigned)regs[7] << 8);
        frame = (frame + frames) & 0x7ff;
        regs[6] = (uint8_t)frame;
        regs[7] = (uint8_t)(frame >> 8);
        s->uhci_frame_itc += frames * (s->config.clock_hz / 1000);
    } else {
        s->uhci_frame_itc = now;
    }
}

static uint64_t uhci_read(I460GX *s, unsigned off, unsigned size) {
    uhci_update_frames(s);
    uint8_t *regs = (uint8_t *)s->uhci_regs;
    uint64_t value = 0;
    for (unsigned i = 0; i < size; i++) {
        unsigned p = off + i;
        uint8_t byte = p == 2 ? ((regs[0] & 1) ? 0 : 0x20) : regs[p];
        value |= (uint64_t)byte << (i * 8);
    }
    return value;
}

static void uhci_write(I460GX *s, unsigned off,
                        uint64_t value, unsigned size) {
    uhci_update_frames(s);
    uint8_t *regs = (uint8_t *)s->uhci_regs;
    /* No USB devices are attached. Commands and root-port resets still
     * complete, while status has no pending transfer or connection event. */
    if (off == 0 && size >= 2) {
        if (value & 2) {
            i460gx_uhci_reset(s);          /* HCRESET self-clears on completion */
            s->uhci_frame_itc = clock_now(s);
            return;
        }
        regs[0] = (uint8_t)value & 0xfd;
    }
    for (unsigned i = 0; i < size; i++) {
        unsigned p = off + i;
        uint8_t byte = (uint8_t)(value >> (i * 8));
        if (p == 4) regs[p] = byte & 0xf; /* USBINTR */
        else if ((p == 6 || p == 7) && off == 6 && size >= 2 && !(regs[0] & 1))
            regs[p] = p == 6 ? byte : byte & 7;
        else if (p >= 8 && p < 12)
            regs[p] = p == 8 ? 0 : p == 9 ? byte & 0xf0 : byte;
        else if (p == 12) regs[p] = byte & 0x7f;
        else if (p == 17 || p == 19) regs[p] = byte & 0x12;
    }
}

static uint32_t iosapic_indirect_read(I460GX *s) {
    unsigned reg = s->iosapic_regsel & 0xff;
    if (reg == 0)
        return 1u << 15;             /* SAPIC delivery, ID zero */
    if (reg == 1)
        return 0x003f0021u;          /* 64 entries, version 21h */
    if (reg >= 0x10 && reg <= 0x8f) {
        uint64_t rte = s->iosapic_rte[(reg - 0x10) / 2];
        return reg & 1 ? (uint32_t)(rte >> 32) : (uint32_t)rte;
    }
    return 0;
}

static void iosapic_indirect_write(I460GX *s, uint32_t value) {
    unsigned reg = s->iosapic_regsel & 0xff;
    if (reg < 0x10 || reg > 0x8f)
        return;
    unsigned entry = (reg - 0x10) / 2;
    uint64_t rte = s->iosapic_rte[entry];
    if (reg & 1)
        rte = (rte & UINT64_C(0xffffffff)) | ((uint64_t)value << 32);
    else
        rte = (rte & UINT64_C(0xffffffff00000000)) | value;
    s->iosapic_rte[entry] = rte;
    if (s->config.iosapic_debug)
        fprintf(stderr, "i460gx: IOSAPIC RTE[%u] <- %016" PRIX64
                " (mask=%d vector=%#x dmode=%u) ticks=%" PRIu64 "\n",
                entry, rte, !!(rte & I460GX_IOSAPIC_RTE_MASK),
                (unsigned)(rte & 0xff), (unsigned)((rte >> 8) & 7), clock_now(s));
}

bool i460gx_mmio_read(I460GX *s, uint64_t addr, unsigned size, uint64_t *value) {
    if (size != 4)
        return false;
    if (addr == I460GX_SAC_CBNR) {
        *value = s->sac_cbnr;
    } else if (addr == I460GX_SAC_CCSR) {
        *value = s->sac_ccsr;
    } else if (addr - I460GX_IOSAPIC_BASE < I460GX_IOSAPIC_SIZE) {
        unsigned off = (unsigned)(addr - I460GX_IOSAPIC_BASE);
        *value = off == IOSAPIC_REGSEL_OFF ? s->iosapic_regsel & 0xff :
                 off == IOSAPIC_WINDOW_OFF ? iosapic_indirect_read(s) : 0;
    } else {
        return false;
    }
    return true;
}

bool i460gx_mmio_write(I460GX *s, uint64_t addr, uint64_t value, unsigned size) {
    if (size != 4)
        return false;
    if (addr == I460GX_SAC_CBNR) {
        s->sac_cbnr = (uint32_t)value;
    } else if (addr == I460GX_SAC_CCSR) {
        s->sac_ccsr = (uint32_t)value;
    } else if (addr - I460GX_IOSAPIC_BASE < I460GX_IOSAPIC_SIZE) {
        unsigned off = (unsigned)(addr - I460GX_IOSAPIC_BASE);
        if (off == IOSAPIC_REGSEL_OFF)
            s->iosapic_regsel = (uint32_t)value & 0xff;
        else if (off == IOSAPIC_WINDOW_OFF)
            iosapic_indirect_write(s, (uint32_t)value);
        /* EOI and reserved registers: level-triggered RIRR is not modeled. */
    } else {
        return false;
    }
    return true;
}

bool i460gx_ifb_io_read(I460GX *s, unsigned fun, uint64_t port,
                        unsigned size, uint64_t *value) {
    if (!i460gx_access_size_valid(size))
        return false;
    unsigned off;
    if (fun == 0) {
        return i460gx_pm_io(s, false, port, size, value);
    } else if (fun == 2 && uhci_port_offset(s, port, size, &off)) {
        *value = uhci_read(s, off, size);
    } else if (fun == 3) {
        return i460gx_smbus_io(s, port, false, size, value);
    } else {
        return false;
    }
    return true;
}

bool i460gx_ifb_io_write(I460GX *s, unsigned fun, uint64_t port,
                         uint64_t value, unsigned size) {
    if (!i460gx_access_size_valid(size))
        return false;
    unsigned off;
    if (fun == 0) {
        return i460gx_pm_io(s, true, port, size, &value);
    } else if (fun == 2 && uhci_port_offset(s, port, size, &off)) {
        uhci_write(s, off, value, size);
    } else if (fun == 3) {
        return i460gx_smbus_io(s, port, true, size, &value);
    } else {
        return false;
    }
    return true;
}

bool i460gx_interrupt(I460GX *s, unsigned gsi, uint8_t *vector, bool *extint) {
    if (gsi >= 64 || (s->iosapic_rte[gsi] & I460GX_IOSAPIC_RTE_MASK))
        return false;
    uint64_t rte = s->iosapic_rte[gsi];
    *vector = (uint8_t)rte;
    *extint = ((rte >> 8) & 7) == 7;
    return true;
}

void i460gx_init(I460GX *s, const I460GXConfig *config) {
    I460GXConfig cfg = *config;
    memset(s, 0, sizeof(*s));
    s->config = cfg;
    /* A board without a clock callback has a stopped clock. The fallback
     * frequency keeps timer arithmetic valid for that configuration. */
    if (!s->config.clock_hz)
        s->config.clock_hz = UINT64_C(1000000000);
    s->config.iosapic_debug = getenv("IOSAPIC_DEBUG") != NULL;
    s->config.pm_debug = getenv("PM_DEBUG") != NULL;
    s->config.fwh_debug = getenv("FWH_DEBUG") != NULL;
    i460gx_reset(s);
    i460gx_pm_reset(s, true);       /* power-on: RSMRST# as well as PCIRST# */
}

void i460gx_reset(I460GX *s) {
    I460GXPm pm = s->pm;            /* the resume and RTC wells survive */
    memset(s, 0, offsetof(I460GX, config));
    s->pm = pm;
    i460gx_pci_reset(s);
    i460gx_fwh_reset(s);
    i460gx_pm_reset(s, false);
    i460gx_uhci_reset(s);
    for (unsigned i = 0; i < 64; i++)
        s->iosapic_rte[i] = I460GX_IOSAPIC_RTE_MASK;
}

void i460gx_warm_reset(I460GX *s) {
    uint32_t cbnr = s->sac_cbnr, ccsr = s->sac_ccsr;
    uint8_t sac_config[2];
    /* SAL programs 44h before CF9 SYS_RST, then checks its sticky bit 7
     * on the second bootstrap pass. Keep these latches while resetting
     * PCI devices and interrupt routing programmed by the previous OS. */
    memcpy(sac_config, &s->chipset_cfg[0][0][0x44], sizeof(sac_config));
    i460gx_reset(s);
    s->sac_cbnr = cbnr;
    s->sac_ccsr = ccsr;
    memcpy(&s->chipset_cfg[0][0][0x44], sac_config, sizeof(sac_config));
}
