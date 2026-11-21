/* CMD PCI-649 IDE controller: see cmd649.h. */
#include "storage/cmd649.h"
#include <string.h>

/* The BARs' fixed bits: I/O space (bit 0) and their sizes - 8 bytes for a
 * task file, 4 for a control block, 16 for the bus-master registers. */
static const uint32_t bar_masks[5] = {
    UINT32_C(0xfffffff8), UINT32_C(0xfffffffc),
    UINT32_C(0xfffffff8), UINT32_C(0xfffffffc),
    UINT32_C(0xfffffff0),
};

static uint8_t read8(const Cmd649 *c, unsigned reg) {
    if (reg >= 0x24 && reg < 0x28)
        return 0;                               /* BAR5 is unimplemented */
    if (reg >= 0x10 && reg < 0x24) {
        unsigned bar = (reg - 0x10) / 4;
        uint32_t value;
        memcpy(&value, &c->cfg[0x10 + bar * 4], sizeof(value));
        value = (value & bar_masks[bar]) | 1;
        return (uint8_t)(value >> ((reg & 3) * 8));
    }
    return c->cfg[reg];
}

uint32_t cmd649_cfg_read(const Cmd649 *c, unsigned reg, unsigned size) {
    uint32_t v = 0;
    for (unsigned i = 0; i < size && reg + i < sizeof(c->cfg); i++)
        v |= (uint32_t)read8(c, reg + i) << (i * 8);
    return v;
}

void cmd649_cfg_write(Cmd649 *c, unsigned reg, unsigned size, uint32_t val) {
    for (unsigned i = 0; i < size && reg + i < sizeof(c->cfg); i++) {
        unsigned off = reg + i;
        if (off < 4 || (off >= 8 && off < 16) || (off >= 0x24 && off < 0x28))
            continue;                           /* read-only */
        c->cfg[off] = (uint8_t)(val >> (i * 8));
        if (off >= 0x10 && off < 0x24)
            c->cfg[off] = read8(c, off);        /* keep the fixed bits */
    }
}

void cmd649_reset(Cmd649 *c) {
    memset(c->cfg, 0, sizeof(c->cfg));
    c->cfg[0x00] = 0x95; c->cfg[0x01] = 0x10;   /* vendor 1095h: CMD */
    c->cfg[0x02] = 0x49; c->cfg[0x03] = 0x06;   /* device 0649h */
    c->cfg[0x08] = 0x02;                        /* revision */
    c->cfg[0x09] = 0x8A;                        /* legacy channels, bus master */
    c->cfg[0x0A] = 0x01;                        /* IDE */
    c->cfg[0x0B] = 0x01;                        /* mass storage */
    c->cfg[0x0E] = 0x00;                        /* normal header */
    for (unsigned bar = 0; bar < 5; bar++)
        c->cfg[0x10 + bar * 4] = 1;             /* I/O BAR, unassigned */
    c->cfg[0x3C] = 15;                          /* interrupt line */
    c->cfg[0x3D] = 0x01;                        /* INTA */
    /* CNTRL: primary and secondary channels enabled.  Drivers (Windows'
     * CmdIde among them) check these before creating channel devices. */
    c->cfg[0x51] = 0x0c;
}

void cmd649_init(Cmd649 *c, IdeChannel *primary, IdeChannel *secondary) {
    c->channel[0] = primary;
    c->channel[1] = secondary;
    cmd649_reset(c);
}

bool cmd649_decode(const Cmd649 *c, uint16_t port, unsigned *channel, unsigned *reg) {
    static const uint16_t base[2] = { 0x1F0, 0x170 };
    for (unsigned ch = 0; ch < 2; ch++) {
        if (!c->channel[ch])
            continue;
        if (port >= base[ch] && port <= base[ch] + 7) {
            *channel = ch;
            *reg = port - base[ch];
            return true;
        }
        if (port == base[ch] + 0x206) {         /* 3F6h, 376h */
            *channel = ch;
            *reg = 8;
            return true;
        }
    }
    return false;
}

bool cmd649_io_read(Cmd649 *c, uint16_t port, unsigned size, uint64_t *val) {
    unsigned ch, reg;
    if (!cmd649_decode(c, port, &ch, &reg))
        return false;
    *val = ide_read(c->channel[ch], reg, size);
    return true;
}

bool cmd649_io_write(Cmd649 *c, uint16_t port, unsigned size, uint64_t val) {
    unsigned ch, reg;
    if (!cmd649_decode(c, port, &ch, &reg))
        return false;
    ide_write(c->channel[ch], reg, val, size);
    return true;
}
