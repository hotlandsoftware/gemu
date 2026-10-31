/*
 * IBM Fixed Disk Adapter (Xebec S1410): see ibm_hdc.h.
 *
 * The command set is the one the adapter's BIOS (ROM 5000059 and its
 * successor 59X7291) uses:
 *   00 test drive ready      01 recalibrate          03 request sense
 *   04 format drive          05 verify sectors       06 format track
 *   07 format bad track      08 read                 0A write
 *   0B seek                  0C initialize drive characteristics (8 bytes)
 *   0D read ECC burst length 0E read sector buffer   0F write sector buffer
 *   E0 RAM diagnostic        E3 drive diagnostic     E4 controller diagnostic
 *   E5 read long             E6 write long
 * Anything else ends with "invalid command".
 */
#include "ibm_hdc.h"
#include <string.h>

enum { PH_IDLE, PH_CMD, PH_BUSY, PH_PIO_OUT, PH_PIO_IN, PH_DMA, PH_STATUS };

/* What runs when the current wait ends */
enum {
    ST_NONE,
    ST_BEGIN,       /* the command has been decoded */
    ST_SEEKED,      /* the heads are on the cylinder */
    ST_SECTOR,      /* a sector has passed under the heads */
    ST_DMA_DONE,    /* the sector buffer has gone to or come from memory */
    ST_TRACK,       /* a track has been formatted (one turn from the index) */
    ST_FINISH,      /* a command with nothing more to do */
};

/* Status register */
#define S_REQ  0x01
#define S_IO   0x02
#define S_CD   0x04
#define S_BSY  0x08
#define S_DRQ  0x10
#define S_INT  0x20

/* Error codes (request sense byte 0) */
#define E_NONE        0x00
#define E_WRITE_FAULT 0x03
#define E_NOT_READY   0x04
#define E_NOT_FOUND   0x14      /* sector not found */
#define E_INVALID     0x20      /* invalid command */
#define E_ADDRESS     0x21      /* illegal disk address */

#define CMD_US        100       /* decoding a command */
#define DIAG_US       5000      /* the diagnostics */
#define DMA_CLOCKS    5         /* per byte: the 8237's 4-clock cycle and the PC's wait state */
#define FORMAT_FILL   0x00      /* the data byte formatting writes isn't known here */

static uint32_t us(const IbmHdc *h, uint32_t n) {
    return (uint32_t)((uint64_t)n * h->clock_hz / 1000000u);
}

/* 3600 rpm */
static uint32_t rev_clocks(const IbmHdc *h)    { return h->clock_hz / 60u; }
static uint32_t sector_clocks(const IbmHdc *h) { return rev_clocks(h) / IBM_HDC_SPT; }

static void update_irq(IbmHdc *h) {
    if (h->set_irq) h->set_irq(h->ud, h->intr && (h->mask & 0x02));
}

static void after(IbmHdc *h, int stage, uint32_t clocks) {
    h->phase = PH_BUSY;
    h->stage = stage;
    h->wait = clocks ? clocks : 1;
}

static void finish(IbmHdc *h, uint8_t err) {
    h->status_byte = (uint8_t)((h->drv << 5) | (err ? 0x02 : 0));
    h->sense[0] = err ? (uint8_t)(err | 0x80) : 0;
    h->sense[1] = (uint8_t)((h->drv << 5) | h->head);
    h->sense[2] = (uint8_t)(((h->cyl >> 2) & 0xC0) | h->sector);
    h->sense[3] = (uint8_t)h->cyl;
    h->phase = PH_STATUS;
    h->stage = ST_NONE;
    h->wait = 0;
    h->intr = true;
    update_irq(h);
}

static bool present(const IbmHdc *h) {
    return h->drive[h->drv] && gemu_drive_inserted(h->drive[h->drv]);
}

/* The address the command gives is on the drive: inside both what the
 * drive has and what it was initialized to. */
static bool address_ok(const IbmHdc *h) {
    const IbmHdcGeometry *g = &h->geom[h->drv], *p = &h->param[h->drv];
    return h->cyl < g->cyls && h->cyl < p->cyls &&
           h->head < g->heads && h->head < p->heads && h->sector < IBM_HDC_SPT;
}

static uint64_t offset(const IbmHdc *h) {
    const IbmHdcGeometry *g = &h->geom[h->drv];
    return (((uint64_t)h->cyl * g->heads + h->head) * IBM_HDC_SPT + h->sector) * IBM_HDC_SECTOR;
}

/* Move the heads; returns how long it takes. */
static uint32_t seek(IbmHdc *h, unsigned cyl) {
    unsigned from = h->pos[h->drv];
    unsigned d = cyl > from ? cyl - from : from - cyl;
    h->pos[h->drv] = cyl;
    return d ? us(h, 3000 + 660 * (d - 1)) : 0;
}

/* Clocks until the start of sector s comes under the heads.  The
 * interleave decides where on the track each sector was written. */
static uint32_t until_sector(const IbmHdc *h, unsigned s) {
    unsigned il = h->interleave[h->drv] ? h->interleave[h->drv] : 1;
    bool used[IBM_HDC_SPT] = { false };
    unsigned p = 0, slot = 0;
    for (unsigned i = 0; i <= s; i++) {
        while (used[p]) p = (p + 1) % IBM_HDC_SPT;
        used[p] = true;
        slot = p;
        p = (p + il) % IBM_HDC_SPT;
    }
    uint32_t rev = rev_clocks(h);
    uint32_t at = (uint32_t)(h->now % rev), target = slot * sector_clocks(h);
    return (target + rev - at) % rev;
}

static uint32_t until_index(const IbmHdc *h) {
    uint32_t rev = rev_clocks(h);
    return (uint32_t)((rev - h->now % rev) % rev);
}

static bool op_write(uint8_t op) { return op == 0x0A || op == 0xE6; }
static bool op_long(uint8_t op)  { return op == 0xE5 || op == 0xE6; }

static void dma_start(IbmHdc *h, bool to_mem, int len) {
    h->phase = PH_DMA;
    h->stage = ST_NONE;
    h->wait = 0;
    h->to_mem = to_mem;
    h->buf_len = len;
    h->buf_pos = 0;
}

/* Move the buffer through the DMA channel as far as it is ready. */
static void dma_run(IbmHdc *h) {
    while (h->buf_pos < h->buf_len) {
        bool tc, ok = h->to_mem ? h->dma_to_mem(h->ud, h->buf[h->buf_pos], &tc)
                                : h->dma_from_mem(h->ud, &h->buf[h->buf_pos], &tc);
        if (!ok) return;
        h->buf_pos++;
    }
    after(h, ST_DMA_DONE, (uint32_t)h->buf_len * DMA_CLOCKS);
}

/* The next sector of a multi-sector command, across heads and cylinders. */
static void next_sector(IbmHdc *h) {
    if (--h->count == 0) { finish(h, E_NONE); return; }
    if (++h->sector == IBM_HDC_SPT) {
        h->sector = 0;
        if (++h->head >= h->param[h->drv].heads || h->head >= h->geom[h->drv].heads) {
            h->head = 0;
            h->cyl++;
        }
    }
    if (!address_ok(h)) { finish(h, E_NOT_FOUND); return; }
    after(h, ST_SEEKED, seek(h, h->cyl));
}

/* Format the track under the heads: every sector filled. */
static bool format_track(IbmHdc *h) {
    uint8_t fill[IBM_HDC_SECTOR];
    memset(fill, FORMAT_FILL, sizeof(fill));
    unsigned keep = h->sector;
    bool ok = true;
    for (h->sector = 0; h->sector < IBM_HDC_SPT && ok; h->sector++)
        ok = gemu_drive_write(h->drive[h->drv], offset(h), fill, sizeof(fill));
    h->sector = keep;
    return ok;
}

static void begin(IbmHdc *h) {
    uint8_t op = h->cmd[0];
    h->drv    = (h->cmd[1] >> 5) & 1;
    h->head   = h->cmd[1] & 0x1F;
    h->sector = h->cmd[2] & 0x3F;
    h->cyl    = ((unsigned)(h->cmd[2] & 0xC0) << 2) | h->cmd[3];
    h->count  = h->cmd[4] ? h->cmd[4] : 256;

    switch (op) {
    case 0x03:                                  /* request sense */
        memcpy(h->pio, h->sense, 4);
        h->pio_n = 4; h->pio_pos = 0;
        h->phase = PH_PIO_IN;
        return;
    case 0x0C:                                  /* initialize drive characteristics */
        h->pio_n = 8; h->pio_pos = 0;
        h->phase = PH_PIO_OUT;
        return;
    case 0x0D:                                  /* ECC burst length: no ECC errors here */
        h->pio[0] = 0;
        h->pio_n = 1; h->pio_pos = 0;
        h->phase = PH_PIO_IN;
        return;
    case 0x0E:                                  /* read sector buffer */
        dma_start(h, true, IBM_HDC_SECTOR);
        return;
    case 0x0F:                                  /* write sector buffer */
        dma_start(h, false, IBM_HDC_SECTOR);
        return;
    case 0xE0: case 0xE4:                       /* RAM, controller diagnostics */
        after(h, ST_FINISH, us(h, DIAG_US));
        return;
    }

    /* The rest act on a drive */
    switch (op) {
    case 0x00: case 0x01: case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x08: case 0x0A: case 0x0B: case 0xE3: case 0xE5: case 0xE6:
        break;
    default:
        finish(h, E_INVALID);
        return;
    }
    if (!present(h)) { finish(h, E_NOT_READY); return; }

    switch (op) {
    case 0x00:                                  /* test drive ready */
        finish(h, E_NONE);
        return;
    case 0x01:                                  /* recalibrate */
        h->cyl = h->head = h->sector = 0;
        after(h, ST_FINISH, seek(h, 0));
        return;
    case 0xE3:                                  /* drive diagnostic: seeks to 0 */
        h->cyl = h->head = h->sector = 0;
        after(h, ST_FINISH, seek(h, 0) + us(h, DIAG_US));
        return;
    }

    if (op == 0x04 || op == 0x06 || op == 0x07) h->sector = 0;
    if (!address_ok(h)) { finish(h, E_ADDRESS); return; }
    if ((op_write(op) || op == 0x04 || op == 0x06 || op == 0x07) &&
        gemu_drive_readonly(h->drive[h->drv])) {
        finish(h, E_WRITE_FAULT);
        return;
    }
    after(h, op == 0x0B ? ST_FINISH : ST_SEEKED, seek(h, h->cyl));
}

static void run_stage(IbmHdc *h) {
    uint8_t op = h->cmd[0];
    int len = op_long(op) ? IBM_HDC_LONG : IBM_HDC_SECTOR;
    switch (h->stage) {
    case ST_BEGIN:
        begin(h);
        break;
    case ST_FINISH:
        finish(h, E_NONE);
        break;
    case ST_SEEKED:
        if (op == 0x04 || op == 0x06 || op == 0x07)
            after(h, ST_TRACK, until_index(h) + rev_clocks(h));
        else if (op_write(op))
            dma_start(h, false, len);
        else                                    /* read, read long, verify */
            after(h, ST_SECTOR, until_sector(h, h->sector) + sector_clocks(h));
        break;
    case ST_SECTOR:
        if (op_write(op)) {
            if (!gemu_drive_write(h->drive[h->drv], offset(h), h->buf, IBM_HDC_SECTOR)) {
                finish(h, E_WRITE_FAULT);
                return;
            }
            next_sector(h);
        } else if (op == 0x05) {
            next_sector(h);
        } else {
            if (!gemu_drive_read(h->drive[h->drv], offset(h), h->buf, IBM_HDC_SECTOR)) {
                finish(h, E_NOT_FOUND);
                return;
            }
            /* read long: the ECC bytes the image doesn't keep read as 0 */
            memset(h->buf + IBM_HDC_SECTOR, 0, IBM_HDC_LONG - IBM_HDC_SECTOR);
            dma_start(h, true, len);
        }
        break;
    case ST_DMA_DONE:
        if (op == 0x0E || op == 0x0F) finish(h, E_NONE);
        else if (op_write(op)) after(h, ST_SECTOR, until_sector(h, h->sector) + sector_clocks(h));
        else next_sector(h);
        break;
    case ST_TRACK:
        if (!format_track(h)) { finish(h, E_WRITE_FAULT); return; }
        if (h->cmd[4]) h->interleave[h->drv] = h->cmd[4];
        if (op != 0x04) { finish(h, E_NONE); return; }
        /* format drive: on to the end of the drive */
        if (++h->head >= h->param[h->drv].heads || h->head >= h->geom[h->drv].heads) {
            h->head = 0;
            if (++h->cyl >= h->param[h->drv].cyls || h->cyl >= h->geom[h->drv].cyls) {
                h->cyl--;
                finish(h, E_NONE);
                return;
            }
        }
        after(h, ST_SEEKED, seek(h, h->cyl));
        break;
    }
}

void ibm_hdc_reset(IbmHdc *h) {
    h->phase = PH_IDLE;
    h->stage = ST_NONE;
    h->wait = 0;
    h->mask = 0;
    h->intr = false;
    h->cmd_n = 0;
    h->status_byte = 0;
    memset(h->sense, 0, sizeof(h->sense));
    for (int d = 0; d < 2; d++) {
        h->param[d] = h->geom[d];
        if (!h->interleave[d]) h->interleave[d] = 1;
    }
    update_irq(h);
}

static uint8_t status_reg(const IbmHdc *h) {
    uint8_t st = 0;
    switch (h->phase) {
    case PH_CMD:     st = S_BSY | S_CD | S_REQ; break;
    case PH_BUSY:    st = S_BSY; break;
    case PH_PIO_OUT: st = S_BSY | S_REQ; break;
    case PH_PIO_IN:  st = S_BSY | S_IO | S_REQ; break;
    case PH_DMA:     st = S_BSY | S_DRQ | (h->to_mem ? S_IO : 0); break;
    case PH_STATUS:  st = S_BSY | S_CD | S_IO | S_REQ; break;
    }
    return (uint8_t)(st | (h->intr ? S_INT : 0));
}

uint8_t ibm_hdc_read(IbmHdc *h, unsigned reg) {
    switch (reg & 3) {
    case 0:
        if (h->phase == PH_PIO_IN) {
            uint8_t v = h->pio[h->pio_pos++];
            if (h->pio_pos == h->pio_n) finish(h, E_NONE);
            return v;
        }
        if (h->phase == PH_STATUS) {
            h->phase = PH_IDLE;
            h->intr = false;
            update_irq(h);
            return h->status_byte;
        }
        return 0xFF;
    case 1:
        return status_reg(h);
    case 2:
        return h->jumpers;
    default:
        return 0xFF;
    }
}

void ibm_hdc_write(IbmHdc *h, unsigned reg, uint8_t val) {
    switch (reg & 3) {
    case 0:
        if (h->phase == PH_CMD) {
            h->cmd[h->cmd_n++] = val;
            if (h->cmd_n == 6) after(h, ST_BEGIN, us(h, CMD_US));
        } else if (h->phase == PH_PIO_OUT) {
            h->pio[h->pio_pos++] = val;
            if (h->pio_pos == h->pio_n) {       /* initialize drive characteristics */
                h->param[h->drv].cyls  = (uint16_t)((h->pio[0] << 8) | h->pio[1]);
                h->param[h->drv].heads = h->pio[2] & 0x1F;
                finish(h, E_NONE);
            }
        }
        break;
    case 1:                                     /* controller reset */
        h->phase = PH_IDLE;
        h->stage = ST_NONE;
        h->wait = 0;
        h->intr = false;
        update_irq(h);
        break;
    case 2:                                     /* select */
        h->phase = PH_CMD;
        h->stage = ST_NONE;
        h->wait = 0;
        h->cmd_n = 0;
        h->intr = false;
        update_irq(h);
        break;
    case 3:
        h->mask = val & 0x03;
        update_irq(h);
        break;
    }
}

void ibm_hdc_tick(IbmHdc *h, uint32_t clocks) {
    h->now += clocks;
    if (h->phase == PH_DMA) dma_run(h);
    if (!h->wait) return;
    if (clocks < h->wait) { h->wait -= clocks; return; }
    h->wait = 0;
    run_stage(h);
}
