/* Intel 460GX PCI configuration space.
 *
 * Four address spaces, none of which carries any board policy beyond the
 * bus/device numbers and expander wiring in I460GXConfig:
 *   - the PID (I/O SAPIC) on the compatibility bus;
 *   - the IFB (PCI-to-LPC/FWH bridge, USB, SMBus) on the compatibility bus;
 *   - the SAC's programming function on the compatibility bus, whose only
 *     writable register is the Chipset Bus Number (CBN);
 *   - the chipset devices on the bus CBN selects: SAC, SDC, the two memory
 *     cards and the four expander ports.
 * Register definitions follow Intel 460GX SSDM 248704-001, Table 2-1 for the
 * device map. */
#include "i460gx_internal.h"

#include <string.h>

enum {
    DEV_SAC = 0, DEV_SAC_ALT = 1, DEV_SDC = 4,
    DEV_MEMCARD_A = 5, DEV_MEMCARD_B = 6,
    DEV_EXPANDER_FIRST = 0x10, DEV_EXPANDER_LAST = 0x17,

    CFG_VENDOR_INTEL = 0x8086,
    CFG_REG_CLASS_SUB = 0x0a, CFG_REG_CLASS_BASE = 0x0b,
    CFG_REG_HEADER = 0x0e,
    CFG_HEADER_MULTIFUNCTION = 0x80,

    SAC_REG_CBN = 0x40, SAC_REG_CBUSES = 0x44, SAC_REG_IIADR = 0x68,
    SAC_REG_F16NUM = 0x60, SAC_REG_F16IDX = 0x64,
    SAC_REG_EXPANDER_BUSES = 0x42,
    SAC_REG_PCIS = 0x84, SAC_REG_IOR = 0x8c,
    EXPANDER_REG_BUSES = 0x80,
    MEMCARD_REG_SEGMENT = 0x48,

    IIADR_EEPROM_MASK = 0xf8, IIADR_EEPROM_VALUE = 0xd0, IIADR_SLAVE_MASK = 0x7f,
    MEMCARD_SEGMENT_ENABLE = 1,

    SAC_PCIS_DEFAULT = 0x40, SAC_IOR_DEFAULT = 0x0f,

    CFG_BIOSEN = 0x4e, BIOSEN_WEN = 0x04, BIOSEN_BLE = 0x80,
};

enum {
    DID_SAC = 0x84e0, DID_SDC = 0x84e1, DID_MEMCARD = 0x84e3,
    DID_PXB = 0x84cb, DID_WXB = 0x84e6,
    DID_GXB_AGP = 0x84ea, DID_GXB_BRIDGE = 0x84e2,
    DID_PID = 0x123d,
};

/* Where a configuration cycle lands. */
typedef enum {
    CFG_SPACE_PID,
    CFG_SPACE_IFB,
    CFG_SPACE_SAC_COMPAT,
    CFG_SPACE_CHIPSET,
    CFG_SPACE_MEMCARD,
} CfgSpace;

typedef struct {
    CfgSpace space;
    unsigned dev, fun;
} CfgTarget;

/* Identity of one chipset-bus function. */
typedef struct {
    uint16_t device;
    uint8_t revision, subclass, header;
    uint8_t buses;      /* expander function 1: PCI buses the board wires */
} FunctionId;

static uint32_t reg_le32(const uint8_t *cfg, unsigned reg) {
    uint32_t value;
    memcpy(&value, cfg + reg, sizeof(value));
    return value;
}

static uint8_t byte_of(uint64_t value, unsigned index) {
    return (uint8_t)(value >> (index * 8));
}

/* Chipset-bus devices that exist at all. Memory cards (05h/06h) have their
 * own latches and I2C routing; expander windows are visible even when no
 * bridge is attached. */
static bool chipset_device_present(unsigned dev) {
    return dev == DEV_SAC || dev == DEV_SAC_ALT || dev == DEV_SDC ||
           (dev >= DEV_EXPANDER_FIRST && dev <= DEV_EXPANDER_LAST);
}

/* The SAC answers all eight functions of devices 0 and 1; the SDC and each
 * expander window answer only the functions their bridge implements. A
 * function that is not listed here reads as all-ones. */
static bool chipset_function_id(const I460GX *s, unsigned dev, unsigned fun,
                                FunctionId *id) {
    *id = (FunctionId){ .device = DID_SAC, .revision = 3 };
    if (dev == DEV_SAC || dev == DEV_SAC_ALT) {
        id->header = fun == 0 ? CFG_HEADER_MULTIFUNCTION : 0;
        return true;
    }
    if (dev == DEV_SDC) {
        id->device = DID_SDC;
        return fun == 0;
    }
    if (dev < DEV_EXPANDER_FIRST || dev > DEV_EXPANDER_LAST)
        return false;
    I460GXBridge bridge = s->config.expander[dev - DEV_EXPANDER_FIRST];
    switch (fun) {
    case 0:
        id->header = bridge != I460GX_BRIDGE_NONE ? CFG_HEADER_MULTIFUNCTION : 0;
        return true;
    case 1:
        switch (bridge) {
        case I460GX_BRIDGE_PXB:
            id->device = DID_PXB; id->revision = 5; id->buses = 1;
            return true;
        case I460GX_BRIDGE_WXB:
            id->device = DID_WXB; id->revision = 7; id->buses = 2;
            return true;
        case I460GX_BRIDGE_GXB:
            id->device = DID_GXB_AGP; id->revision = 2; id->buses = 1;
            return true;
        default:
            return false;
        }
    case 2:
        id->device = DID_GXB_BRIDGE; id->revision = 2; id->subclass = 0x80;
        return bridge == I460GX_BRIDGE_GXB;
    default:
        return false;
    }
}

static bool cfg_decode(const I460GX *s, unsigned bus, unsigned dev,
                       unsigned fun, CfgTarget *target) {
    const I460GXConfig *c = &s->config;
    *target = (CfgTarget){ .dev = dev, .fun = fun };
    if (bus == c->pid_bus && dev == c->pid_dev && fun == 0) {
        target->space = CFG_SPACE_PID;
        return true;
    }
    if (bus == c->ifb_bus && dev == c->ifb_dev &&
        (fun == 0 || fun == 2 || fun == 3)) {
        target->space = CFG_SPACE_IFB;
        return true;
    }
    /* This programming function stays reachable however CBN is set. */
    if (bus == c->cbn_bus && dev == c->cbn_dev && fun == 0) {
        target->space = CFG_SPACE_SAC_COMPAT;
        return true;
    }
    if (bus != s->chipset_bus)
        return false;
    if (dev == DEV_MEMCARD_A || dev == DEV_MEMCARD_B) {
        target->space = CFG_SPACE_MEMCARD;
        return true;
    }
    if (!chipset_device_present(dev))
        return false;
    target->space = CFG_SPACE_CHIPSET;
    return true;
}

/* ---- PID ------------------------------------------------------------------
 * The 460GX's I/O SAPIC appears as a programmable interrupt device with a
 * fixed register window and no base address registers. Registers past the
 * header are routing and GPIO latches whose behaviour inside the PID is not
 * documented; they are stored. */

static bool pid_cfg_writable(unsigned reg) {
    return !(reg < 4 || (reg >= 8 && reg <= 0x0b) || reg == CFG_REG_HEADER ||
             (reg >= 0x10 && reg < 0x28) || (reg >= 0x30 && reg < 0x34));
}

/* ---- IFB --------------------------------------------------------------- */

uint8_t i460gx_ifb_cfg_read8(const I460GX *s, unsigned fun, unsigned reg) {
    const uint8_t *cfg = fun == 0 ? s->ifb_cfg :
                         fun == 2 ? s->ifb_usb_cfg : s->ifb_smbus_cfg;
    if (fun == 0 && reg >= 0x0f && reg < 0x40)
        return 0;
    if (fun != 0) {
        if (reg >= 0x20 && reg < 0x24) {
            uint32_t value = reg_le32(cfg, 0x20);
            value = (value & (fun == 2 ? 0xffe0u : 0xfff0u)) | 1;
            return byte_of(value, reg & 3);
        }
        if ((reg >= 0x10 && reg < 0x20) || (reg >= 0x24 && reg < 0x3c))
            return 0;
        if (reg == 4) return cfg[reg] & (fun == 2 ? 5 : 1);
        if (reg == 5) return 0;
    }
    return cfg[reg];
}

static void ifb_cfg_write8(I460GX *s, unsigned fun, unsigned reg,
                           uint8_t value) {
    uint8_t *cfg = fun == 0 ? s->ifb_cfg :
                   fun == 2 ? s->ifb_usb_cfg : s->ifb_smbus_cfg;
    if (reg < 4 || (reg >= 8 && reg <= 0x0c) || reg == CFG_REG_HEADER ||
        (fun == 0 && reg >= 0x0f && reg < 0x40) ||
        (fun != 0 && ((reg >= 0x10 && reg < 0x20) ||
                      (reg >= 0x24 && reg < 0x3c) || reg == 0x3d)) ||
        (fun == 2 && reg == 0x60))
        return;
    cfg[reg] = value;
    cfg[reg] = i460gx_ifb_cfg_read8(s, fun, reg);
}

/* ---- SAC programming function ---------------------------------------------
 * Device 10h on the compatibility bus is how software finds and moves the
 * chipset bus (SSDM 2.2.1). Only CBN is writable; CBUSES reports one
 * chipset bus per 460GX. */

static uint8_t sac_compat_read8(const I460GX *s, unsigned reg) {
    switch (reg) {
    case 0: return byte_of(CFG_VENDOR_INTEL, 0);
    case 1: return byte_of(CFG_VENDOR_INTEL, 1);
    case 2: return byte_of(DID_SAC, 0);
    case 3: return byte_of(DID_SAC, 1);
    case 8: return 3;
    case CFG_REG_CLASS_BASE: return 6;
    case SAC_REG_CBN: return s->chipset_bus;
    case SAC_REG_CBUSES: return 1;
    default: return 0;
    }
}

/* ---- Chipset bus ------------------------------------------------------- */

/* Fixed identity and topology fields are normalised on every read, so a
 * latch restored from an older save cannot disagree with the wiring. */
static uint8_t chipset_cfg_read8(const I460GX *s, unsigned dev, unsigned fun,
                                 unsigned reg) {
    FunctionId id;
    if (!chipset_function_id(s, dev, fun, &id))
        return 0xff;
    uint8_t value = s->chipset_cfg[dev][fun][reg];
    if (reg < 2) return byte_of(CFG_VENDOR_INTEL, reg);
    if (reg < 4) return byte_of(id.device, reg - 2);
    switch (reg) {
    case 8: return id.revision;
    case 9: return 0;
    case CFG_REG_CLASS_SUB: return id.subclass;
    case CFG_REG_CLASS_BASE: return 6;
    case CFG_REG_HEADER: return id.header;
    default: break;
    }
    if (dev == DEV_SAC && fun == 0) {
        if (reg == SAC_REG_F16NUM)
            return (uint8_t)s->config.link_count;
        if (reg == SAC_REG_F16IDX) {
            /* Indexed link probe: bit 7 completes the request and bit 6
             * reports a connected link. An out-of-range probe completes
             * without reporting an extra link; firmware polls this
             * interface before programming the DEVNPRES bitmap. */
            unsigned link = value & 0x3f;
            return (uint8_t)(link | 0x80 |
                             (link < s->config.link_count ? 0x40 : 0));
        }
    }
    /* Each expander window can describe two buses. Its active count
     * reflects the bridge's wiring and mode: firmware uses it to mask unused
     * windows and takes a GXB's second link without making another root
     * bus. The upper bits remain writable. */
    if (fun == 0 && dev >= DEV_EXPANDER_FIRST && reg == SAC_REG_EXPANDER_BUSES)
        return (value & ~3u) | 2;
    if (id.buses && reg == EXPANDER_REG_BUSES)
        return (value & ~3u) | id.buses;
    return value;
}

static void chipset_cfg_write8(I460GX *s, unsigned dev, unsigned fun,
                               unsigned reg, uint8_t value) {
    FunctionId id;
    if (!chipset_function_id(s, dev, fun, &id) ||
        reg < 4 || (reg >= 8 && reg <= 0x0b) || reg == CFG_REG_HEADER)
        return;
    uint8_t *latch = &s->chipset_cfg[dev][fun][reg];
    *latch = value;
    *latch = chipset_cfg_read8(s, dev, fun, reg);
}

/* ---- Memory cards -----------------------------------------------------
 * With the SAC's IIADR (dev 0, reg 68h) selecting an EEPROM address, a
 * card's function 2 turns configuration reads into I2C reads of that device
 * at the configuration offset (SSDM 2.2.1). Functions 4..7 each carry a
 * row-select bit; exactly one must be set. The board decides which rows and
 * DIMMs are wired. */

static bool memcard_spd_selected(const I460GX *s) {
    return (s->chipset_cfg[DEV_SAC][0][SAC_REG_IIADR] & IIADR_EEPROM_MASK) ==
           IIADR_EEPROM_VALUE;
}

static bool memcard_spd_read8(const I460GX *s, unsigned card, unsigned reg,
                              uint8_t *value) {
    unsigned row = 4;
    for (unsigned i = 0; i < 4; i++) {
        if (!(s->memcard_cfg[card][4 + i][MEMCARD_REG_SEGMENT] &
              MEMCARD_SEGMENT_ENABLE))
            continue;
        if (row != 4)
            return false;
        row = i;
    }
    I2cBus *bus = row == 4 ? NULL : s->config.memcard_i2c[card][row];
    if (!bus)
        return false;
    unsigned slave = s->chipset_cfg[DEV_SAC][0][SAC_REG_IIADR] &
                     IIADR_SLAVE_MASK;
    /* Random read: set the word address, then restart for the data byte. */
    bool ok = i2c_start(bus, (uint8_t)slave, false) &&
              i2c_write(bus, (uint8_t)reg) &&
              i2c_start(bus, (uint8_t)slave, true);
    if (ok)
        *value = i2c_read(bus, false);
    i2c_stop(bus);
    return ok;
}

static uint8_t memcard_cfg_read8(const I460GX *s, unsigned card, unsigned fun,
                                 unsigned reg) {
    if (fun == 2 && memcard_spd_selected(s)) {
        uint8_t value;
        return memcard_spd_read8(s, card, reg, &value) ? value : 0xff;
    }
    return s->memcard_cfg[card][fun][reg];
}

static void memcard_cfg_write8(I460GX *s, unsigned card, unsigned fun,
                               unsigned reg, uint8_t value) {
    /* The EEPROM is effectively read-only, and a write to an absent row must
     * not fall through into the ordinary latches. */
    if (fun == 2 && memcard_spd_selected(s))
        return;
    if (fun == 0 && (reg < 4 || reg == CFG_REG_CLASS_BASE))
        return;
    s->memcard_cfg[card][fun][reg] = value;
}

/* ---- Access dispatch --------------------------------------------------- */

static uint8_t cfg_read8(const I460GX *s, const CfgTarget *t, unsigned reg) {
    switch (t->space) {
    case CFG_SPACE_PID:        return s->pid_cfg[reg];
    case CFG_SPACE_IFB:        return i460gx_ifb_cfg_read8(s, t->fun, reg);
    case CFG_SPACE_SAC_COMPAT: return sac_compat_read8(s, reg);
    case CFG_SPACE_CHIPSET:    return chipset_cfg_read8(s, t->dev, t->fun, reg);
    case CFG_SPACE_MEMCARD:
        return memcard_cfg_read8(s, t->dev - DEV_MEMCARD_A, t->fun, reg);
    }
    return 0xff;
}

static void cfg_write8(I460GX *s, const CfgTarget *t, unsigned reg,
                       uint8_t value) {
    switch (t->space) {
    case CFG_SPACE_PID:
        if (pid_cfg_writable(reg))
            s->pid_cfg[reg] = value;
        break;
    case CFG_SPACE_IFB:
        ifb_cfg_write8(s, t->fun, reg, value);
        break;
    case CFG_SPACE_SAC_COMPAT:
        if (reg == SAC_REG_CBN)
            s->chipset_bus = value;
        break;
    case CFG_SPACE_CHIPSET:
        chipset_cfg_write8(s, t->dev, t->fun, reg, value);
        break;
    case CFG_SPACE_MEMCARD:
        memcard_cfg_write8(s, t->dev - DEV_MEMCARD_A, t->fun, reg, value);
        break;
    }
}

static bool cfg_access_valid(unsigned bus, unsigned dev, unsigned fun,
                             unsigned reg, unsigned size) {
    return bus <= 255 && dev <= 31 && fun <= 7 &&
           i460gx_access_size_valid(size) && reg <= 256 - size;
}

bool i460gx_pci_read(I460GX *s, unsigned bus, unsigned dev, unsigned fun,
                     unsigned reg, unsigned size, uint64_t *value) {
    CfgTarget target;
    if (!cfg_access_valid(bus, dev, fun, reg, size) ||
        !cfg_decode(s, bus, dev, fun, &target))
        return false;
    *value = 0;
    for (unsigned i = 0; i < size; i++)
        *value |= (uint64_t)cfg_read8(s, &target, reg + i) << (i * 8);
    return true;
}

bool i460gx_pci_write(I460GX *s, unsigned bus, unsigned dev, unsigned fun,
                      unsigned reg, uint64_t value, unsigned size) {
    CfgTarget target;
    if (!cfg_access_valid(bus, dev, fun, reg, size) ||
        !cfg_decode(s, bus, dev, fun, &target))
        return false;
    bool bios_wen = s->ifb_cfg[CFG_BIOSEN] & BIOSEN_WEN;
    for (unsigned i = 0; i < size; i++)
        cfg_write8(s, &target, reg + i, byte_of(value, i));
    /* Setting BIOS_WEN raises BIOSWEN_STS, judged once the whole access has
     * landed so a 16-bit write sees its own BLE bit. */
    if (target.space == CFG_SPACE_IFB && fun == 0 && !bios_wen &&
        (s->ifb_cfg[CFG_BIOSEN] & BIOSEN_WEN))
        i460gx_pm_bios_wen(s, s->ifb_cfg[CFG_BIOSEN + 1] & BIOSEN_BLE);
    return true;
}

/* ---- Reset ------------------------------------------------------------- */

static void set_identity(uint8_t *cfg, uint16_t device) {
    cfg[0] = byte_of(CFG_VENDOR_INTEL, 0);
    cfg[1] = byte_of(CFG_VENDOR_INTEL, 1);
    cfg[2] = byte_of(device, 0);
    cfg[3] = byte_of(device, 1);
}

static void ifb_reset(I460GX *s) {
    /* PCI-to-LPC/FWH bridge; SSDM chapter 11 reset values. Every IFB
     * function reports revision 01 on shipping boards. */
    uint8_t *lpc = s->ifb_cfg;
    set_identity(lpc, 0x7600);
    lpc[0x04] = 0x07;
    lpc[0x08] = 0x01;
    lpc[0x06] = 0x80; lpc[0x07] = 0x02;
    lpc[CFG_REG_CLASS_SUB] = 0x01; lpc[CFG_REG_CLASS_BASE] = 0x06;
    lpc[CFG_REG_HEADER] = CFG_HEADER_MULTIFUNCTION;
    lpc[0x4e] = 0xc1; lpc[0x4f] = 0x07;
    lpc[0x60] = lpc[0x61] = lpc[0x62] = lpc[0x63] = 0x80;
    lpc[0x64] = 0x10;
    lpc[0x69] = 0x02;
    lpc[0x84] = 0x00; lpc[0x85] = 0x05;
    /* FWHDE: every granule decodes (LFRAME# was low at PWROK, so the firmware
     * exists). FWHS: IDs 0-3 on the four granule pairs, top megabyte first. */
    lpc[0xe3] = 0xff;
    lpc[0xe8] = 0x33; lpc[0xe9] = 0x22; lpc[0xea] = 0x11; lpc[0xeb] = 0x00;

    uint8_t *usb = s->ifb_usb_cfg;
    set_identity(usb, 0x7602);
    usb[0x06] = 0x80; usb[0x07] = 0x02; usb[0x08] = 0x01;
    usb[CFG_REG_CLASS_SUB] = 0x03; usb[CFG_REG_CLASS_BASE] = 0x0c;
    usb[0x20] = 0x01;
    usb[0x3d] = 0x04; usb[0x60] = 0x10;

    uint8_t *smbus = s->ifb_smbus_cfg;
    set_identity(smbus, 0x7603);
    smbus[0x06] = 0x80; smbus[0x07] = 0x02; smbus[0x08] = 0x01;
    smbus[CFG_REG_CLASS_SUB] = 0x05; smbus[CFG_REG_CLASS_BASE] = 0x0c;
    smbus[0x20] = 0x01; smbus[0x3d] = 0x02;
}

void i460gx_pci_reset(I460GX *s) {
    s->chipset_bus = 0xff;

    set_identity(s->pid_cfg, DID_PID);
    s->pid_cfg[8] = 1;
    s->pid_cfg[CFG_REG_CLASS_BASE] = 8;     /* base class: system peripheral */

    ifb_reset(s);

    for (unsigned dev = DEV_EXPANDER_FIRST; dev <= DEV_EXPANDER_LAST; dev++) {
        s->chipset_cfg[dev][0][SAC_REG_PCIS] = SAC_PCIS_DEFAULT;
        s->chipset_cfg[dev][0][SAC_REG_IOR] = SAC_IOR_DEFAULT;
    }
    for (unsigned dev = 0; dev < 32; dev++) {
        if (!chipset_device_present(dev))
            continue;
        for (unsigned fun = 0; fun < 8; fun++) {
            FunctionId id;
            if (!chipset_function_id(s, dev, fun, &id))
                continue;
            for (unsigned reg = 0; reg < 256; reg++)
                s->chipset_cfg[dev][fun][reg] =
                    chipset_cfg_read8(s, dev, fun, reg);
        }
    }
    for (unsigned card = 0; card < 2; card++) {
        uint8_t *cfg = s->memcard_cfg[card][0];
        set_identity(cfg, DID_MEMCARD);
        cfg[CFG_REG_CLASS_BASE] = 6;
    }
}
