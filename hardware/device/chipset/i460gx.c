/* Intel 460GX SAC/SDC, expander and IFB register model.
 * Register definitions follow Intel 460GX SSDM 248704-001. Board wiring,
 * attached EEPROMs and processor interrupt delivery are supplied externally.
 */
#include "chipset/i460gx.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    IOSAPIC_REGSEL_OFF = 0x00, IOSAPIC_WINDOW_OFF = 0x10,
    SMB_HSTSTS = 0x00, SMB_HSTCNT = 0x02, SMB_HSTCMD = 0x03,
    SMB_HSTADD = 0x04, SMB_HSTDAT0 = 0x05, SMB_HSTDAT1 = 0x06,
    SMB_REGSPAN = 0x07,
    SMB_HSTSTS_INTER = 1u << 1, SMB_HSTSTS_DEV_ERR = 1u << 2,
    SMB_HSTCNT_START = 1u << 6,
    SMB_PROT_BYTE_DATA = 2, SMB_PROT_QUICK = 0,
};

static uint64_t size_mask(unsigned size) {
    return size == 8 ? UINT64_MAX : (UINT64_C(1) << (size * 8)) - 1;
}

static bool access_size_valid(unsigned size) {
    return size == 1 || size == 2 || size == 4 || size == 8;
}

static uint64_t clock_now(I460GX *s) {
    return s->config.clock ? s->config.clock(s->config.opaque) : 0;
}

/* SSDM Table 2-1. Memory cards (05h/06h) have separate configuration
 * latches and I2C routing; expander windows remain visible when unattached. */
static bool chipset_device_present(unsigned dev) {
    return dev == 0 || dev == 1 || dev == 4 || (dev >= 0x10 && dev <= 0x17);
}

static uint8_t ifb_cfg_read8(I460GX *s, unsigned fun, unsigned reg) {
    uint8_t *cfg = fun == 0 ? s->ifb_cfg :
                   fun == 2 ? s->ifb_usb_cfg : s->ifb_smbus_cfg;
    if (fun == 0 && reg >= 0x0f && reg < 0x40)
        return 0;
    if (fun != 0) {
        if (reg >= 0x20 && reg < 0x24) {
            uint32_t value;
            memcpy(&value, cfg + 0x20, sizeof(value));
            value = (value & (fun == 2 ? 0xffe0u : 0xfff0u)) | 1;
            return (uint8_t)(value >> ((reg & 3) * 8));
        }
        if ((reg >= 0x10 && reg < 0x20) ||
            (reg >= 0x24 && reg < 0x3c))
            return 0;
        if (reg == 4) return cfg[reg] & (fun == 2 ? 5 : 1);
        if (reg == 5) return 0;
    }
    return cfg[reg];
}

static void ifb_cfg_write8(I460GX *s, unsigned fun,
                            unsigned reg, uint8_t value) {
    uint8_t *cfg = fun == 0 ? s->ifb_cfg :
                   fun == 2 ? s->ifb_usb_cfg : s->ifb_smbus_cfg;
    if (reg < 4 || (reg >= 8 && reg <= 0x0c) || reg == 0x0e ||
        (fun == 0 && reg >= 0x0f && reg < 0x40) ||
        (fun != 0 && ((reg >= 0x10 && reg < 0x20) ||
                      (reg >= 0x24 && reg < 0x3c) || reg == 0x3d)) ||
        (fun == 2 && reg == 0x60))
        return;
    cfg[reg] = value;
    cfg[reg] = ifb_cfg_read8(s, fun, reg);
}

/* Fixed identity and topology fields are normalized on reads, including
 * after importing older saved configuration latches. */
static uint8_t chipset_cfg_read8(I460GX *s, unsigned dev,
                                  unsigned fun, unsigned reg) {
    uint8_t value = s->chipset_cfg[dev][fun][reg];
    uint32_t identity = 0;
    unsigned revision = 0, buses = 0;
    if (fun == 0) {
        identity = dev == 4 ? UINT32_C(0x84e18086) : UINT32_C(0x84e08086);
        revision = 3;
    } else if (dev >= 0x10 && dev <= 0x17) {
        I460GXBridge bridge = s->config.expander[dev - 0x10];
        if (fun == 1) {
            switch (bridge) {
            case I460GX_BRIDGE_PXB:
                identity = UINT32_C(0x84cb8086); revision = 5; buses = 1;
                break;
            case I460GX_BRIDGE_WXB:
                identity = UINT32_C(0x84e68086); revision = 7; buses = 2;
                break;
            case I460GX_BRIDGE_GXB:
                identity = UINT32_C(0x84ea8086); revision = 2; buses = 1;
                break;
            default: break;
            }
        } else if (fun == 2 && bridge == I460GX_BRIDGE_GXB) {
            identity = UINT32_C(0x84e28086); revision = 2;
        }
    }
    if (identity) {
        if (reg < 4) return (uint8_t)(identity >> (reg * 8));
        if (reg == 8) return (uint8_t)revision;
        if (reg == 9) return 0;
        if (reg == 0x0a) return fun == 2 ? 0x80 : 0;
        if (reg == 0x0b) return 6;
        if (reg == 0x0e) return fun == 0 && dev >= 0x10 ? 0x80 : 0;
    }
    if (dev == 0 && fun == 0) {
        if (reg == 0x60) return (uint8_t)s->config.link_count; /* F16NUM */
        if (reg == 0x64) {
            /* Indexed link probe: bit 7 completes the request; bit 6
             * reports a connected link. An out-of-range probe completes
             * without reporting an extra link. ROM and EFI both poll this
             * interface before programming the DEVNPRES bitmap. */
            unsigned link = value & 0x3f;
            return (uint8_t)(link | 0x80 |
                             (link < s->config.link_count ? 0x40 : 0));
        }
    }
    /* Each SAC expander window can describe two buses. The bridge's
     * active count reflects its wiring/mode. Firmware uses these fields
     * to mask unused windows and consumes the GXB's second link without
     * creating another root bus. Preserve the writable upper bits. */
    if (fun == 0 && dev >= 0x10 && reg == 0x42)
        return (value & ~3u) | 2;
    if (buses && reg == 0x80)
        return (value & ~3u) | buses;
    return value;
}

static uint64_t smbus_reg_read(I460GX *s, uint64_t off, unsigned size) {
    if (size != 1)
        return size_mask(size);
    switch (off) {
    case SMB_HSTSTS:  return s->smb_hststs;
    case SMB_HSTCNT:  return 0;  /* START always reads back 0 */
    case SMB_HSTCMD:  return s->smb_hstcmd;
    case SMB_HSTADD:  return s->smb_hstadd;
    case SMB_HSTDAT0: return s->smb_hstdat0;
    case SMB_HSTDAT1: return s->smb_hstdat1;
    default: return size_mask(size);
    }
}

static void smbus_reg_write(I460GX *s, uint64_t off, uint64_t val,
                            unsigned size) {
    if (size != 1)
        return;
    switch (off) {
    case SMB_HSTSTS:
        s->smb_hststs &= (uint8_t)~val;  /* write-1-to-clear */
        return;
    case SMB_HSTCMD:  s->smb_hstcmd  = (uint8_t)val; return;
    case SMB_HSTADD:  s->smb_hstadd  = (uint8_t)val; return;
    case SMB_HSTDAT0: s->smb_hstdat0 = (uint8_t)val; return;
    case SMB_HSTDAT1: s->smb_hstdat1 = (uint8_t)val; return;
    case SMB_HSTCNT: {
        if (!(val & SMB_HSTCNT_START))
            return;
        unsigned protocol = (unsigned)((val >> 2) & 7);
        unsigned slave = (unsigned)(s->smb_hstadd >> 1);
        bool is_read = (s->smb_hstadd & 1) != 0;
        uint8_t data = 0;
        bool present = s->config.spd_read &&
            s->config.spd_read(s->config.opaque, s->config.smbus_card,
                               s->config.smbus_row, slave,
                               s->smb_hstcmd, &data);
        if (s->config.smbus_debug)
            fprintf(stderr, "i460gx: SMBUS start slave=%#x rw=%s proto=%u "
                    "cmd=%#x present=%d\n", slave,
                    is_read ? "read" : "write", protocol, s->smb_hstcmd,
                    present);
        if (!present) {
            s->smb_hststs |= SMB_HSTSTS_DEV_ERR | SMB_HSTSTS_INTER;
            return;
        }
        if (protocol == SMB_PROT_QUICK) {
            s->smb_hststs |= SMB_HSTSTS_INTER;
            return;
        }
        if (protocol == SMB_PROT_BYTE_DATA) {
            if (is_read)
                s->smb_hstdat0 = data;
            /* SPD EEPROMs are effectively read-only here (no firmware
             * write-back path needs modeling); accept writes silently. */
            s->smb_hststs |= SMB_HSTSTS_INTER;
            return;
        }
        s->smb_hststs |= SMB_HSTSTS_DEV_ERR | SMB_HSTSTS_INTER;
        return;
    }
    default:
        return;
    }
}

static bool smbus_port_offset(I460GX *s, uint64_t port, uint64_t *off) {
    uint32_t bar = (uint32_t)ifb_cfg_read8(s, 3, 0x20) |
                   (uint32_t)ifb_cfg_read8(s, 3, 0x21) << 8;
    if (!(s->ifb_smbus_cfg[4] & 1))
        return false;  /* PCI I/O decode is disabled. */
    uint32_t base = bar & 0xFFF0u;
    if (base == 0 || port < base || port - base > SMB_REGSPAN)
        return false;
    *off = port - base;
    return true;
}

static bool acpi_port_offset(I460GX *s, uint64_t port,
                             unsigned size, unsigned *off) {
    uint32_t base = (uint32_t)s->ifb_cfg[0x40] |
                    (uint32_t)s->ifb_cfg[0x41] << 8;
    base &= 0xffc0;
    if (!(s->ifb_cfg[0x44] & 1) || !base || port < base ||
        port - base > sizeof(s->acpi_pm) - size)
        return false;
    *off = (unsigned)(port - base);
    return true;
}

static uint64_t acpi_pm_read(I460GX *s, unsigned off, unsigned size) {
    const uint8_t *regs = (const uint8_t *)s->acpi_pm;
    uint64_t itc = clock_now(s);
    uint32_t timer = (uint32_t)((itc / s->config.clock_hz) * 3579545u +
                     (itc % s->config.clock_hz) * 3579545u /
                     s->config.clock_hz) & 0xffffff;
    uint64_t value = 0;
    for (unsigned i = 0; i < size; i++) {
        unsigned p = off + i;
        uint8_t byte = p >= 8 && p < 12 ? timer >> ((p - 8) * 8) : regs[p];
        value |= (uint64_t)byte << (i * 8);
    }
    return value;
}

static void acpi_pm_write(I460GX *s, unsigned off,
                           uint64_t value, unsigned size) {
    uint8_t *regs = (uint8_t *)s->acpi_pm;
    for (unsigned i = 0; i < size; i++) {
        unsigned p = off + i;
        uint8_t byte = (uint8_t)(value >> (i * 8));
        if (p >= 8 && p < 12) continue; /* free-running, read-only timer */
        /* PM1_STS and the GPE status halves are write-one-to-clear. */
        if (p < 2 || (p >= 12 && p < 14) || (p >= 22 && p < 24))
            regs[p] &= (uint8_t)~byte;
        else
            regs[p] = byte;
    }
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
    uint32_t bar = (uint32_t)ifb_cfg_read8(s, 2, 0x20) |
                   (uint32_t)ifb_cfg_read8(s, 2, 0x21) << 8;
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

/* SAC IIADR routes function-2 configuration cycles to a selected EEPROM.
 * Functions 4..7 each have a row-select bit; exactly one must be selected.
 * The callback determines which rows and DIMMs are wired on the board. */
static bool memcard_spd_read(I460GX *s, unsigned card, unsigned slave,
                              unsigned reg, unsigned size, uint64_t *value) {
    unsigned row = 4;
    for (unsigned i = 0; i < 4; i++) {
        if (!(s->memcard_cfg[card][4 + i][0x48] & 1))
            continue;
        if (row != 4)
            return false;
        row = i;
    }
    if (row == 4 || !s->config.spd_read)
        return false;
    uint64_t result = 0;
    for (unsigned i = 0; i < size; i++) {
        uint8_t byte;
        if (!s->config.spd_read(s->config.opaque, card, row, slave,
                                reg + i, &byte))
            return false;
        result |= (uint64_t)byte << (i * 8);
    }
    *value = result;
    return true;
}

bool i460gx_pci_read(I460GX *s, unsigned bus, unsigned dev, unsigned fun,
                     unsigned reg, unsigned size, uint64_t *value) {
    if (bus > 255 || dev > 31 || fun > 7 || !access_size_valid(size) ||
        reg > 256 - size)
        return false;
    if (bus == s->config.ifb_bus && dev == s->config.ifb_dev &&
        (fun == 0 || fun == 2 || fun == 3)) {
        *value = 0;
        for (unsigned i = 0; i < size; i++)
            *value |= (uint64_t)ifb_cfg_read8(s, fun, reg + i) << (i * 8);
        return true;
    }
    /* This fixed programming function remains accessible when CBN moves. */
    if (bus == s->config.cbn_bus && dev == s->config.cbn_dev && fun == 0) {
        *value = 0;
        for (unsigned i = 0; i < size; i++) {
            unsigned off = reg + i;
            uint8_t byte = off == 0x40 ? s->chipset_bus :
                           off < 0x10 ? chipset_cfg_read8(s, 0, 0, off) : 0;
            *value |= (uint64_t)byte << (i * 8);
        }
        return true;
    }
    if (bus != s->chipset_bus)
        return false;
    if (chipset_device_present(dev)) {
        *value = 0;
        for (unsigned i = 0; i < size; i++)
            *value |= (uint64_t)chipset_cfg_read8(s, dev, fun, reg + i) << (i * 8);
        return true;
    }
    if (dev == 5 || dev == 6) {
        unsigned iiadr = s->chipset_cfg[0][0][0x68];
        if (fun == 2 && (iiadr & 0xf8) == 0xd0) {
            if (!memcard_spd_read(s, dev - 5, iiadr & 0x7f, reg, size, value))
                *value = size_mask(size);
        } else {
            *value = 0;
            memcpy(value, &s->memcard_cfg[dev - 5][fun][reg], size);
        }
        return true;
    }
    return false;
}

bool i460gx_pci_write(I460GX *s, unsigned bus, unsigned dev, unsigned fun,
                      unsigned reg, uint64_t value, unsigned size) {
    if (bus > 255 || dev > 31 || fun > 7 || !access_size_valid(size) ||
        reg > 256 - size)
        return false;
    if (bus == s->config.ifb_bus && dev == s->config.ifb_dev &&
        (fun == 0 || fun == 2 || fun == 3)) {
        for (unsigned i = 0; i < size; i++)
            ifb_cfg_write8(s, fun, reg + i, (uint8_t)(value >> (i * 8)));
        return true;
    }
    if (bus == s->config.cbn_bus && dev == s->config.cbn_dev && fun == 0) {
        if (reg <= 0x40 && reg + size > 0x40)
            s->chipset_bus = (uint8_t)(value >> ((0x40 - reg) * 8));
        return true;
    }
    if (bus != s->chipset_bus)
        return false;
    if (chipset_device_present(dev)) {
        for (unsigned i = 0; i < size; i++) {
            unsigned off = reg + i;
            if (fun != 0 || (off >= 4 && !(off >= 8 && off < 12))) {
                s->chipset_cfg[dev][fun][off] = (uint8_t)(value >> (i * 8));
                s->chipset_cfg[dev][fun][off] = chipset_cfg_read8(s, dev, fun, off);
            }
        }
        return true;
    }
    if (dev == 5 || dev == 6) {
        /* EEPROM writes, including writes to an absent row, must not
         * fall through into the ordinary configuration latches. */
        unsigned iiadr = s->chipset_cfg[0][0][0x68];
        if (!(fun == 2 && (iiadr & 0xf8) == 0xd0))
            memcpy(&s->memcard_cfg[dev - 5][fun][reg], &value, size);
        return true;
    }
    return false;
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
    if (!access_size_valid(size))
        return false;
    unsigned off;
    if (fun == 0) {
        if (acpi_port_offset(s, port, size, &off)) {
            *value = acpi_pm_read(s, off, size);
        } else if (size == 1 && (port == 0xb2 || port == 0xb3)) {
            *value = port == 0xb2 ? s->apm_control : s->apm_status;
        } else {
            return false;
        }
    } else if (fun == 2 && uhci_port_offset(s, port, size, &off)) {
        *value = uhci_read(s, off, size);
    } else if (fun == 3) {
        uint64_t smb_off;
        if (!smbus_port_offset(s, port, &smb_off))
            return false;
        *value = smbus_reg_read(s, smb_off, size);
    } else {
        return false;
    }
    return true;
}

bool i460gx_ifb_io_write(I460GX *s, unsigned fun, uint64_t port,
                         uint64_t value, unsigned size) {
    if (!access_size_valid(size))
        return false;
    unsigned off;
    if (fun == 0) {
        if (acpi_port_offset(s, port, size, &off)) {
            acpi_pm_write(s, off, value, size);
        } else if (size == 1 && port == 0xb3) {
            s->apm_status = (uint8_t)value;
        } else if (size == 1 && port == 0xb2) {
            s->apm_control = (uint8_t)value;
            if (s->config.apm_write)
                s->config.apm_write(s->config.opaque, (uint8_t)value);
        } else {
            return false;
        }
    } else if (fun == 2 && uhci_port_offset(s, port, size, &off)) {
        uhci_write(s, off, value, size);
    } else if (fun == 3) {
        uint64_t smb_off;
        if (!smbus_port_offset(s, port, &smb_off))
            return false;
        smbus_reg_write(s, smb_off, value, size);
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
    s->config.smbus_debug = getenv("SMBUS_DEBUG") != NULL;
    s->config.iosapic_debug = getenv("IOSAPIC_DEBUG") != NULL;
    i460gx_reset(s);
}

void i460gx_reset(I460GX *s) {
    memset(s, 0, offsetof(I460GX, config));
    s->chipset_bus = 0xff;
    i460gx_uhci_reset(s);

    /* IFB PCI-to-LPC/FWH bridge; SSDM chapter 11 reset values. */
    s->ifb_cfg[0x00] = 0x86; s->ifb_cfg[0x01] = 0x80;
    s->ifb_cfg[0x02] = 0x00; s->ifb_cfg[0x03] = 0x76;
    s->ifb_cfg[0x04] = 0x07; s->ifb_cfg[0x05] = 0x00;
    s->ifb_cfg[0x06] = 0x80; s->ifb_cfg[0x07] = 0x02;
    s->ifb_cfg[0x09] = 0x00; s->ifb_cfg[0x0a] = 0x01;
    s->ifb_cfg[0x0b] = 0x06; s->ifb_cfg[0x0e] = 0x80;
    s->ifb_cfg[0x4e] = 0xc1; s->ifb_cfg[0x4f] = 0x07;
    s->ifb_cfg[0x60] = s->ifb_cfg[0x61] = 0x80;
    s->ifb_cfg[0x62] = s->ifb_cfg[0x63] = 0x80;
    s->ifb_cfg[0x64] = 0x10;
    s->ifb_cfg[0x69] = 0x02;
    s->ifb_cfg[0x84] = 0x00; s->ifb_cfg[0x85] = 0x05;

    /* IFB UHCI and SMBus PCI functions. */
    s->ifb_usb_cfg[0x00] = 0x86; s->ifb_usb_cfg[0x01] = 0x80;
    s->ifb_usb_cfg[0x02] = 0x02; s->ifb_usb_cfg[0x03] = 0x76;
    s->ifb_usb_cfg[0x06] = 0x80; s->ifb_usb_cfg[0x07] = 0x02;
    s->ifb_usb_cfg[0x0a] = 0x03; s->ifb_usb_cfg[0x0b] = 0x0c;
    s->ifb_usb_cfg[0x20] = 0x01;
    s->ifb_usb_cfg[0x3d] = 0x04; s->ifb_usb_cfg[0x60] = 0x10;

    s->ifb_smbus_cfg[0x00] = 0x86; s->ifb_smbus_cfg[0x01] = 0x80;
    s->ifb_smbus_cfg[0x02] = 0x03; s->ifb_smbus_cfg[0x03] = 0x76;
    s->ifb_smbus_cfg[0x06] = 0x80; s->ifb_smbus_cfg[0x07] = 0x02;
    s->ifb_smbus_cfg[0x0a] = 0x05; s->ifb_smbus_cfg[0x0b] = 0x0c;
    s->ifb_smbus_cfg[0x20] = 0x01; s->ifb_smbus_cfg[0x3d] = 0x02;

    for (unsigned dev = 0; dev < 32; dev++) {
        if (!chipset_device_present(dev))
            continue;
        for (unsigned fun = 0; fun < 8; fun++)
            for (unsigned reg = 0; reg < 256; reg++)
                s->chipset_cfg[dev][fun][reg] = chipset_cfg_read8(s, dev, fun, reg);
    }
    for (unsigned card = 0; card < 2; card++) {
        uint8_t *cfg = s->memcard_cfg[card][0];
        cfg[0] = 0x86; cfg[1] = 0x80;
        cfg[2] = 0xe3; cfg[3] = 0x84;
        cfg[0x0b] = 0x06;
    }
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
