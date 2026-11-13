/* Intel 460GX IFB SMBus controller (PCI function 3, SSDM chapter 14).
 *
 * The host interface runs quick, byte, byte-data, word-data and block
 * transactions on the board's SMBus segment. Transactions complete at once,
 * so HOST_BUSY is never seen set. The slave interface registers are stored
 * but no external master is modelled. The controller decodes a 16-byte I/O
 * block at SMBBA (function 3, reg 20h) while PCI I/O decode is enabled. */
#include "i460gx_internal.h"

#include <stdio.h>
#include <string.h>

enum {
    SMBUS_IO_SIZE = 16,

    /* Offsets within the I/O block. */
    REG_HST_STS = 0x00, REG_SLV_STS = 0x01, REG_HST_CNT = 0x02,
    REG_HST_CMD = 0x03, REG_HST_ADD = 0x04, REG_HST_DAT0 = 0x05,
    REG_HST_DAT1 = 0x06, REG_BLOCK = 0x07, REG_SLV_CNT = 0x08,
    REG_SHDW_CMD = 0x09, REG_SLV_EVT = 0x0a, REG_SLV_DAT = 0x0c,

    /* SMBHSTSTS, 14.3.1. */
    HST_STS_INTER = 0x02,       /* last host command completed */
    HST_STS_DEV_ERR = 0x04,     /* illegal command, unclaimed cycle, time-out */
    HST_STS_BUS_ERR = 0x08,     /* collision */
    HST_STS_FAILED = 0x10,      /* KILL */
    HST_STS_WRITE_CLEAR = HST_STS_INTER | HST_STS_DEV_ERR |
                          HST_STS_BUS_ERR | HST_STS_FAILED,
    /* SMBSLVSTS, 14.3.2: bits 4:2 are write-one-to-clear. */
    SLV_STS_WRITE_CLEAR = 0x1c,
    /* SMBHSTCNT, 14.3.3. */
    HST_CNT_KILL = 0x02,
    HST_CNT_START = 0x40,       /* always reads back 0 */
    HST_CNT_STORED = 0x1f,
    SLV_CNT_STORED = 0x87,

    /* Host controller configuration (function 3, reg 40h). */
    CFG_HOST_CONFIG = 0x40, CFG_HOST_ENABLE = 0x01,
    CFG_COMMAND = 0x04, CFG_COMMAND_IO = 0x01,
    CFG_BASE = 0x20,

    PROTOCOL_QUICK = 0, PROTOCOL_BYTE = 1, PROTOCOL_BYTE_DATA = 2,
    PROTOCOL_WORD_DATA = 3, PROTOCOL_BLOCK = 4,
    BLOCK_MAX = 32,
};

static unsigned protocol_of(uint8_t hst_cnt) {
    return (hst_cnt >> 2) & 7;
}

/* One host transaction. SMBHSTADD bit 0 is the direction; a NACK anywhere is
 * an unclaimed cycle (DEV_ERR). Returns false on failure. */
static bool run_transaction(I460GXSmbus *r, I2cBus *bus) {
    uint8_t addr7 = r->hst_add >> 1;
    bool read = r->hst_add & 1;
    bool ok;

    switch (protocol_of(r->hst_cnt)) {
    case PROTOCOL_QUICK:
        ok = i2c_start(bus, addr7, read);
        break;
    case PROTOCOL_BYTE:
        ok = i2c_start(bus, addr7, read);
        if (ok && read)
            r->hst_dat0 = i2c_read(bus, false);
        else if (ok)
            ok = i2c_write(bus, r->hst_cmd);
        break;
    case PROTOCOL_BYTE_DATA:
    case PROTOCOL_WORD_DATA: {
        bool word = protocol_of(r->hst_cnt) == PROTOCOL_WORD_DATA;
        ok = i2c_start(bus, addr7, false) && i2c_write(bus, r->hst_cmd);
        if (ok && read) {
            ok = i2c_start(bus, addr7, true);
            if (ok) {
                r->hst_dat0 = i2c_read(bus, word);
                if (word)
                    r->hst_dat1 = i2c_read(bus, false);
            }
        } else if (ok) {
            ok = i2c_write(bus, r->hst_dat0);
            if (ok && word)
                ok = i2c_write(bus, r->hst_dat1);
        }
        break;
    }
    case PROTOCOL_BLOCK:
        /* DAT0 holds the byte count; the data is the block array from
         * index 0. A write count outside 1..32 is undefined and refused. */
        ok = i2c_start(bus, addr7, false) && i2c_write(bus, r->hst_cmd);
        if (ok && read) {
            ok = i2c_start(bus, addr7, true);
            if (ok) {
                unsigned count = i2c_read(bus, true);
                if (count > BLOCK_MAX)
                    count = BLOCK_MAX;
                r->hst_dat0 = (uint8_t)count;
                for (unsigned i = 0; i < count; i++)
                    r->block[i] = i2c_read(bus, i + 1 < count);
            }
        } else if (ok) {
            unsigned count = r->hst_dat0;
            ok = count >= 1 && count <= BLOCK_MAX &&
                 i2c_write(bus, (uint8_t)count);
            for (unsigned i = 0; ok && i < count; i++)
                ok = i2c_write(bus, r->block[i]);
        }
        break;
    default:
        return false;       /* 101b-111b: illegal command field */
    }
    i2c_stop(bus);
    return ok;
}

static void start_host_transaction(I460GX *s) {
    I460GXSmbus *r = &s->smbus;
    bool ok = false;
    if (protocol_of(r->hst_cnt) <= PROTOCOL_BLOCK && s->config.smbus)
        ok = run_transaction(r, s->config.smbus);
    r->hst_sts |= ok ? HST_STS_INTER : HST_STS_DEV_ERR;
}

static uint8_t io_read8(I460GX *s, unsigned off, bool *valid) {
    I460GXSmbus *r = &s->smbus;
    *valid = true;
    switch (off) {
    case REG_HST_STS:  return r->hst_sts;
    case REG_SLV_STS:  return r->slv_sts;
    case REG_HST_CNT:
        r->block_index = 0;     /* reading HST_CNT clears the block index */
        return r->hst_cnt;
    case REG_HST_CMD:  return r->hst_cmd;
    case REG_HST_ADD:  return r->hst_add;
    case REG_HST_DAT0: return r->hst_dat0;
    case REG_HST_DAT1: return r->hst_dat1;
    case REG_BLOCK:    return r->block[r->block_index++ % BLOCK_MAX];
    case REG_SLV_CNT:  return r->slv_cnt;
    case REG_SHDW_CMD: return r->shdw_cmd;
    case REG_SLV_EVT:  return (uint8_t)r->slv_evt;
    case REG_SLV_EVT + 1: return (uint8_t)(r->slv_evt >> 8);
    case REG_SLV_DAT:  return (uint8_t)r->slv_dat;
    case REG_SLV_DAT + 1: return (uint8_t)(r->slv_dat >> 8);
    default:
        *valid = false;
        return 0;
    }
}

static void io_write8(I460GX *s, unsigned off, uint8_t value, bool *valid) {
    I460GXSmbus *r = &s->smbus;
    *valid = true;
    switch (off) {
    case REG_HST_STS:
        r->hst_sts &= (uint8_t)~(value & HST_STS_WRITE_CLEAR);
        break;
    case REG_SLV_STS:
        r->slv_sts &= (uint8_t)~(value & SLV_STS_WRITE_CLEAR);
        break;
    case REG_HST_CNT:
        r->hst_cnt = value & HST_CNT_STORED;
        if (value & HST_CNT_KILL)
            r->hst_sts |= HST_STS_FAILED;
        /* Without the host enable bit a START executes nothing. */
        if ((value & HST_CNT_START) &&
            (s->ifb_smbus_cfg[CFG_HOST_CONFIG] & CFG_HOST_ENABLE))
            start_host_transaction(s);
        break;
    case REG_HST_CMD:  r->hst_cmd = value; break;
    case REG_HST_ADD:  r->hst_add = value; break;
    case REG_HST_DAT0: r->hst_dat0 = value; break;
    case REG_HST_DAT1: r->hst_dat1 = value; break;
    case REG_BLOCK:    r->block[r->block_index++ % BLOCK_MAX] = value; break;
    case REG_SLV_CNT:  r->slv_cnt = value & SLV_CNT_STORED; break;
    case REG_SHDW_CMD: break;               /* read-only */
    case REG_SLV_EVT:
        r->slv_evt = (uint16_t)((r->slv_evt & 0xff00) | value);
        break;
    case REG_SLV_EVT + 1:
        r->slv_evt = (uint16_t)((r->slv_evt & 0x00ff) | (value << 8));
        break;
    case REG_SLV_DAT: case REG_SLV_DAT + 1:
        break;                              /* read-only */
    default:
        *valid = false;
        break;
    }
}

static bool port_offset(const I460GX *s, uint64_t port, unsigned size,
                        unsigned *off) {
    if (!(s->ifb_smbus_cfg[CFG_COMMAND] & CFG_COMMAND_IO))
        return false;
    uint32_t base = (uint32_t)i460gx_ifb_cfg_read8(s, 3, CFG_BASE) |
                    (uint32_t)i460gx_ifb_cfg_read8(s, 3, CFG_BASE + 1) << 8;
    base &= 0xfff0;
    if (!base || port < base || port - base > SMBUS_IO_SIZE - size)
        return false;
    *off = (unsigned)(port - base);
    return true;
}

bool i460gx_smbus_io(I460GX *s, uint64_t port, bool write, unsigned size,
                     uint64_t *value) {
    unsigned off;
    if (!port_offset(s, port, size, &off))
        return false;
    uint64_t result = 0;
    for (unsigned i = 0; i < size; i++) {
        bool valid;
        uint8_t byte = 0;
        if (write)
            io_write8(s, off + i, (uint8_t)(*value >> (i * 8)), &valid);
        else
            byte = io_read8(s, off + i, &valid);
        if (!valid && i2c_debug_enabled())
            fprintf(stderr, "i460gx: SMBus %s of reserved offset %#x\n",
                    write ? "write" : "read", off + i);
        result |= (uint64_t)byte << (i * 8);
    }
    if (!write)
        *value = result;
    return true;
}
