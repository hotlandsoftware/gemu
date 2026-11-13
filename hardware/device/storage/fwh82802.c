/* Intel 82802AC Firmware Hub; see fwh82802.h. Table numbers are the
 * datasheet's (290658-004). */
#include "storage/fwh82802.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

enum {
    BLOCK_SIZE = 0x10000,
    /* Table 15, 3.3 V VPP typical: 17 us a byte, 0.8 s a block. */
    PROGRAM_NS = 17000,
    ERASE_NS = 800000000,

    SR_ESS = 0x40, SR_ES = 0x20, SR_PS = 0x10, SR_PSS = 0x04, SR_DPS = 0x02,
    SR_READY = 0x80,

    LOCK_WRITE = 0x01, LOCK_DOWN = 0x02, LOCK_READ = 0x04, LOCK_MASK = 0x07,
    LOCK_REG_OFFSET = 0x0002,       /* within each block */

    REG_FGPI = 0xc0100, REG_RNG_STATUS = 0xc015f, REG_RNG_DATA_STATUS = 0xc0160,
    REG_RNG_DATA = 0xc0161,

    MAX_REPORTS = 16,
};

enum { MODE_ARRAY, MODE_ID, MODE_STATUS, MODE_LOCK };
enum { OP_NONE, OP_ERASE, OP_PROGRAM, OP_LOCK };

static void report(Fwh82802 *fwh, unsigned *count, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static void report(Fwh82802 *fwh, unsigned *count, const char *format, ...) {
    if (!fwh->debug || (*count)++ >= MAX_REPORTS)
        return;
    va_list args;
    va_start(args, format);
    fputs("fwh82802: ", stderr);
    vfprintf(stderr, format, args);
    va_end(args);
}

static bool busy(const Fwh82802 *fwh, uint64_t now_ns) {
    return !fwh->suspended && fwh->op != OP_NONE && now_ns < fwh->busy_until;
}

static uint8_t read_status(Fwh82802 *fwh, uint64_t now_ns) {
    if (fwh->op != OP_NONE && !fwh->suspended && !busy(fwh, now_ns))
        fwh->op = OP_NONE;
    return (uint8_t)((busy(fwh, now_ns) ? 0 : SR_READY) | fwh->status);
}

/* The WSM starts an operation: a write-locked block fails with SR.1 (DPS)
 * set. The array changes as the operation starts; the busy time shows in the
 * status register. */
static void start_operation(Fwh82802 *fwh, uint8_t *array, uint64_t now_ns,
                            int op, uint32_t off, uint8_t data) {
    unsigned block = (off & (FWH82802_SIZE - 1)) / BLOCK_SIZE;
    fwh->mode = MODE_STATUS;
    if (fwh->lock[block] & LOCK_WRITE) {
        fwh->status |= SR_DPS | (op == OP_ERASE ? SR_ES : SR_PS);
        return;
    }
    if (op == OP_ERASE) {
        memset(array + block * BLOCK_SIZE, 0xff, BLOCK_SIZE);
        fwh->changed_off = block * BLOCK_SIZE;
        fwh->changed_len = BLOCK_SIZE;
    } else {
        uint32_t at = off & (FWH82802_SIZE - 1);
        array[at] &= data;                          /* programming clears bits */
        fwh->changed_off = at;
        fwh->changed_len = 1;
    }
    fwh->op = op;
    fwh->busy_until = now_ns + (op == OP_ERASE ? ERASE_NS : PROGRAM_NS);
}

/* Lock-down can only be set; once set the register is frozen until reset. */
static void write_lock(Fwh82802 *fwh, uint32_t off, uint8_t value) {
    uint8_t *lock = &fwh->lock[(off & (FWH82802_SIZE - 1)) / BLOCK_SIZE];
    if (!(*lock & LOCK_DOWN))
        *lock = value & LOCK_MASK;
}

static void command(Fwh82802 *fwh, uint8_t *array, uint64_t now_ns,
                    uint32_t off, uint8_t value) {
    if (fwh->setup == OP_LOCK) {
        /* After 91h the next cycle is the block's lock register at block+2;
         * anything else is the next command. */
        fwh->setup = OP_NONE;
        if ((off & 0xffff) == LOCK_REG_OFFSET) {
            write_lock(fwh, off, value);
            return;
        }
    }
    if (fwh->setup == OP_PROGRAM) {
        fwh->setup = OP_NONE;
        start_operation(fwh, array, now_ns, OP_PROGRAM, off, value);
        return;
    }
    if (fwh->setup == OP_ERASE) {
        fwh->setup = OP_NONE;
        if (value == 0xd0) {
            start_operation(fwh, array, now_ns, OP_ERASE, off, 0);
            return;
        }
        fwh->status |= SR_ES | SR_PS;       /* improper command sequence */
        fwh->mode = MODE_STATUS;
        return;
    }
    if (busy(fwh, now_ns) && value != 0xb0 && value != 0x70) {
        report(fwh, &fwh->reports, "command %#04x ignored while busy\n", value);
        return;
    }
    switch (value) {
    case 0xff: fwh->mode = MODE_ARRAY; break;
    case 0x90: fwh->mode = MODE_ID; break;
    case 0x70: fwh->mode = MODE_STATUS; break;
    case 0x91:
        /* Lock register access as the Intel IA-64 flash driver performs it:
         * 91h at the block, then the block's lock register written or read
         * at block+2, then FFh. The values it writes (00h before an update,
         * 01h after, 03h on the block it locks down) are the bit layout of
         * the lock register. Not in the command table. */
        fwh->mode = MODE_LOCK;
        fwh->setup = OP_LOCK;
        break;
    case 0x50:
        fwh->status &= (uint8_t)~(SR_ES | SR_PS | 0x08 | SR_DPS);
        break;
    case 0x20: fwh->setup = OP_ERASE; break;
    case 0x40: case 0x10:
        if (fwh->suspended && fwh->op == OP_PROGRAM) {
            report(fwh, &fwh->reports, "program while a program is suspended\n");
            break;
        }
        fwh->setup = OP_PROGRAM;
        break;
    case 0xb0:
        /* Suspend: with the operation modelled as one span, it stops at
         * once. */
        if (busy(fwh, now_ns)) {
            fwh->remaining = fwh->busy_until - now_ns;
            fwh->suspended = true;
            fwh->status |= fwh->op == OP_ERASE ? SR_ESS : SR_PSS;
        }
        fwh->mode = MODE_STATUS;
        break;
    case 0xd0:
        if (fwh->suspended) {
            fwh->suspended = false;
            fwh->busy_until = now_ns + fwh->remaining;
            fwh->status &= (uint8_t)~(SR_ESS | SR_PSS);
        }
        fwh->mode = MODE_STATUS;
        break;
    default:
        report(fwh, &fwh->reports, "reserved command %#04x\n", value);
        break;
    }
}

/* The RNG digitises thermal noise; a fixed-seed xorshift stands in so runs
 * are reproducible. */
static uint8_t rng_byte(Fwh82802 *fwh) {
    uint32_t x = fwh->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    fwh->rng = x;
    return (uint8_t)x;
}

static void register_access(Fwh82802 *fwh, bool write, uint32_t off,
                            uint8_t *value) {
    off &= FWH82802_SIZE - 1;
    if ((off & 0xffff) == LOCK_REG_OFFSET) {    /* per-block lock registers */
        if (write)
            write_lock(fwh, off, *value);
        else
            *value = fwh->lock[off >> 16];
        return;
    }
    switch (off) {
    case REG_FGPI:
        if (!write)
            *value = fwh->gpi & 0x1f;
        return;
    case REG_RNG_STATUS:
        if (write)
            fwh->rng_status = (uint8_t)((fwh->rng_status & ~1u) | (*value & 1));
        else
            *value = fwh->rng_status;
        return;
    case REG_RNG_DATA_STATUS:
        if (!write)
            *value = fwh->rng_status & 1;
        return;
    case REG_RNG_DATA:
        if (!write)
            *value = (fwh->rng_status & 1) ? rng_byte(fwh) : 0;
        return;
    default:
        report(fwh, &fwh->register_reports,
               "register space %s at %#07x is not in Table 5\n",
               write ? "write" : "read", off);
        if (!write)
            *value = 0xff;
        return;
    }
}

void fwh82802_reset(Fwh82802 *fwh) {
    fwh->mode = MODE_ARRAY;
    fwh->setup = OP_NONE;
    fwh->status = 0;
    fwh->op = OP_NONE;
    fwh->suspended = false;
    fwh->busy_until = 0;
    memset(fwh->lock, LOCK_WRITE, sizeof(fwh->lock));   /* Table 5 default */
    fwh->rng_status = 0x40;
}

void fwh82802_init(Fwh82802 *fwh, uint8_t gpi, bool debug) {
    memset(fwh, 0, sizeof(*fwh));
    fwh->gpi = gpi;
    fwh->debug = debug;
    fwh->rng = 0x2545f491;
    fwh82802_reset(fwh);
}

void fwh82802_access(Fwh82802 *fwh, uint8_t *array, uint64_t now_ns, bool reg,
                     bool write, uint32_t off, uint8_t *value) {
    if (reg) {
        register_access(fwh, write, off, value);
        return;
    }
    off &= FWH82802_SIZE - 1;
    if (write) {
        command(fwh, array, now_ns, off, *value);
        return;
    }
    if (busy(fwh, now_ns) || fwh->mode == MODE_STATUS) {
        *value = read_status(fwh, now_ns);
        return;
    }
    if (fwh->mode == MODE_LOCK) {
        if ((off & 0xffff) == LOCK_REG_OFFSET) {
            *value = fwh->lock[off / BLOCK_SIZE];
            return;
        }
        report(fwh, &fwh->reports,
               "read at %#07x in lock mode away from block+2 returns the array\n",
               off);
    }
    if (fwh->mode == MODE_ID) {
        /* Table 4: 89h manufacturer, ACh device (8 Mbit). Decoded within the
         * block: the firmware identifies each part at its top block, and the
         * real board boots. */
        if ((off & (BLOCK_SIZE - 1)) <= 1) {
            *value = (off & 1) ? 0xac : 0x89;
            return;
        }
        report(fwh, &fwh->reports,
               "identifier read at %#07x is outside block+0/+1\n", off);
        *value = 0xff;
        return;
    }
    /* An attempted read from a read-locked block returns 00h. */
    *value = (fwh->lock[off / BLOCK_SIZE] & LOCK_READ) ? 0x00 : array[off];
}
