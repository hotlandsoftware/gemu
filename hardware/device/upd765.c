#include "upd765.h"
#include <string.h>

/* Main status register */
#define MSR_RQM  0x80   /* data register ready */
#define MSR_DIO  0x40   /* 1 = controller -> CPU (result phase) */
#define MSR_CB   0x10   /* command in progress */

/* ST0 */
#define ST0_ABNORMAL  0x40
#define ST0_INVALID   0x80
#define ST0_READY_CHG 0xC0
#define ST0_SEEK_END  0x20
#define ST0_EQUIP     0x10
/* ST1 */
#define ST1_EN  0x80    /* end of cylinder */
#define ST1_ND  0x04    /* no data */
#define ST1_NW  0x02    /* not writable */
#define ST1_MA  0x01    /* missing address mark */

/* How long operations take, in CPU clocks (4.77 MHz): enough that BIOS
 * code sees the controller busy and waits for the interrupt as it would
 * on hardware, without slowing things to real drive speeds. */
#define DELAY_RESET  2000
#define DELAY_SEEK   4000
#define DELAY_RW     8000

typedef struct { uint64_t size; int cyls, heads, spt; } Geometry;

static const Geometry geometries[] = {
    {  163840, 40, 1,  8 },   /* 160K */
    {  184320, 40, 1,  9 },   /* 180K */
    {  327680, 40, 2,  8 },   /* 320K */
    {  368640, 40, 2,  9 },   /* 360K */
    {  737280, 80, 2,  9 },   /* 720K */
    { 1228800, 80, 2, 15 },   /* 1.2M */
    { 1474560, 80, 2, 18 },   /* 1.44M */
};

static bool geometry(GemuDrive *d, Geometry *g) {
    uint64_t size = gemu_drive_size(d);
    for (size_t i = 0; i < sizeof(geometries) / sizeof(geometries[0]); i++)
        if (geometries[i].size == size) { *g = geometries[i]; return true; }
    return false;
}

static void set_irq(Upd765 *f, bool level) {
    f->irq = level;
    if (f->set_irq) f->set_irq(f->ud, level && (f->dor & 0x08));
}

/* Queue an interrupt status for sense interrupt status.  A seek or
 * recalibrate replaces whatever is left over from a reset (QEMU's FDC does
 * the same; the 19OCT81 BIOS reads just one of the four reset statuses). */
static void seek_int(Upd765 *f, uint8_t st0, uint8_t pcn) {
    f->n_int = 0;
    f->int_st0[0] = st0;
    f->int_pcn[0] = pcn;
    f->n_int = 1;
}

static void push_int(Upd765 *f, uint8_t st0, uint8_t pcn) {
    if (f->n_int < 4) {
        f->int_st0[f->n_int] = st0;
        f->int_pcn[f->n_int] = pcn;
        f->n_int++;
    }
}

void upd765_reset(Upd765 *f) {
    f->cmd_len = f->cmd_need = 0;
    f->res_len = f->res_pos = 0;
    f->busy = 0;
    f->irq_after = false;
    f->n_int = 0;
    set_irq(f, false);
}

static void result(Upd765 *f, const uint8_t *r, int n, uint32_t delay, bool irq) {
    if (n) memcpy(f->res, r, (size_t)n);
    f->res_len = n;
    f->res_pos = 0;
    f->busy = delay;
    f->irq_after = irq;
    if (!delay && irq) set_irq(f, true);
}

/* Command bytes per opcode (low 5 bits), including the opcode byte. */
static int command_length(uint8_t op) {
    switch (op & 0x1F) {
    case 0x02: case 0x05: case 0x06: case 0x09: case 0x0C:
    case 0x11: case 0x19: case 0x1D: return 9;   /* read/write/scan */
    case 0x03: return 3;                          /* specify */
    case 0x04: return 2;                          /* sense drive status */
    case 0x07: return 2;                          /* recalibrate */
    case 0x08: return 1;                          /* sense interrupt status */
    case 0x0A: return 2;                          /* read ID */
    case 0x0D: return 6;                          /* format track */
    case 0x0F: return 3;                          /* seek */
    default:   return 1;                          /* invalid */
    }
}

/* Read/write data: transfer sectors R..EOT (continuing onto head 1 with
 * MT) until DMA terminal count. */
static void do_rw(Upd765 *f, bool write) {
    uint8_t *c = f->cmd;
    int ds = c[1] & 3, head = (c[1] >> 2) & 1;
    bool mt = (c[0] & 0x80) != 0;
    uint8_t C = c[2], H = c[3], R = c[4], N = c[5], eot = c[6];
    GemuDrive *d = f->drive[ds];
    uint8_t st0 = (uint8_t)(ds | (head << 2)), st1 = 0, st2 = 0;
    Geometry g;

    if (!d || !gemu_drive_inserted(d) || !geometry(d, &g)) {
        st0 |= ST0_ABNORMAL;
        st1 = ST1_MA;                              /* no readable track */
    } else if (write && gemu_drive_readonly(d)) {
        st0 |= ST0_ABNORMAL;
        st1 = ST1_NW;
    } else {
        size_t sector_size = (size_t)128 << (N & 7);
        uint8_t buf[16384];
        bool tc = false;
        while (!tc) {
            if (C != f->pcn[ds] || H >= g.heads || head >= g.heads ||
                R < 1 || R > g.spt || f->pcn[ds] >= g.cyls || sector_size != 512) {
                st0 |= ST0_ABNORMAL;
                st1 |= ST1_ND;
                break;
            }
            uint64_t off = (((uint64_t)f->pcn[ds] * g.heads + head) * g.spt + (R - 1)) * 512;
            if (!write && !gemu_drive_read(d, off, buf, 512)) { st0 |= ST0_ABNORMAL; st1 |= ST1_ND; break; }
            size_t i;
            for (i = 0; i < 512 && !tc; i++) {
                bool ok = write ? f->dma_from_mem(f->ud, &buf[i], &tc)
                                : f->dma_to_mem(f->ud, buf[i], &tc);
                if (!ok) break;
            }
            if (i < 512 && !tc) { st0 |= ST0_ABNORMAL; break; }     /* DMA not ready */
            if (write && !gemu_drive_write(d, off, buf, 512)) {
                st0 |= ST0_ABNORMAL; st1 |= ST1_NW; break;
            }
            /* Advance to the next sector, as the result phase reports it. */
            if (R == eot) {
                R = 1;
                if (mt && head == 0 && g.heads > 1) { head = 1; H ^= 1; }
                else { C++; if (mt) { head = 0; H ^= 1; } if (!tc) { st0 |= ST0_ABNORMAL; st1 |= ST1_EN; break; } }
            } else {
                R++;
            }
        }
    }
    st0 = (uint8_t)((st0 & ~0x04) | (head << 2));
    uint8_t r[7] = { st0, st1, st2, C, H, R, N };
    result(f, r, 7, DELAY_RW, true);
}

static void do_format(Upd765 *f) {
    uint8_t *c = f->cmd;
    int ds = c[1] & 3, head = (c[1] >> 2) & 1;
    uint8_t N = c[2], sc = c[3], fill = c[5];
    GemuDrive *d = f->drive[ds];
    uint8_t st0 = (uint8_t)(ds | (head << 2)), st1 = 0;
    Geometry g;
    uint8_t id[4] = {0};
    if (!d || !gemu_drive_inserted(d) || !geometry(d, &g)) {
        st0 |= ST0_ABNORMAL; st1 = ST1_MA;
    } else if (gemu_drive_readonly(d)) {
        st0 |= ST0_ABNORMAL; st1 = ST1_NW;
    } else {
        uint8_t sector[512];
        memset(sector, fill, sizeof(sector));
        for (int s = 0; s < sc; s++) {
            bool tc = false;
            for (int k = 0; k < 4; k++)               /* C, H, R, N from memory */
                if (!f->dma_from_mem(f->ud, &id[k], &tc)) { st0 |= ST0_ABNORMAL; break; }
            if (st0 & ST0_ABNORMAL) break;
            uint8_t R = id[2];
            if (R >= 1 && R <= g.spt && head < g.heads && f->pcn[ds] < g.cyls) {
                uint64_t off = (((uint64_t)f->pcn[ds] * g.heads + head) * g.spt + (R - 1)) * 512;
                gemu_drive_write(d, off, sector, sizeof(sector));
            }
            if (tc) break;
        }
    }
    uint8_t r[7] = { st0, st1, 0, id[0], id[1], id[2], N };
    result(f, r, 7, DELAY_RW, true);
}

static void execute(Upd765 *f) {
    uint8_t *c = f->cmd;
    int ds = c[1] & 3;
    switch (c[0] & 0x1F) {
    case 0x03:                                     /* specify: timing only */
        f->cmd_len = 0;
        return;
    case 0x04: {                                   /* sense drive status -> ST3 */
        GemuDrive *d = f->drive[ds];
        Geometry g;
        bool two_sided = d && gemu_drive_inserted(d) && geometry(d, &g) && g.heads == 2;
        uint8_t st3 = (uint8_t)(c[1] & 7) | 0x20 /* ready */;
        if (f->pcn[ds] == 0) st3 |= 0x10;
        if (two_sided) st3 |= 0x08;
        if (d && gemu_drive_readonly(d)) st3 |= 0x40;
        result(f, &st3, 1, 0, false);
        return;
    }
    case 0x07: {                                   /* recalibrate */
        uint8_t st0 = (uint8_t)(ST0_SEEK_END | ds);
        if (f->drive[ds]) f->pcn[ds] = 0;
        else st0 |= ST0_ABNORMAL | ST0_EQUIP;      /* no track 0 signal */
        seek_int(f, st0, f->pcn[ds]);
        result(f, NULL, 0, DELAY_SEEK, true);
        return;
    }
    case 0x08: {                                   /* sense interrupt status */
        if (f->n_int == 0) {
            uint8_t r = ST0_INVALID;
            result(f, &r, 1, 0, false);
            return;
        }
        uint8_t r[2] = { f->int_st0[0], f->int_pcn[0] };
        f->n_int--;
        memmove(f->int_st0, f->int_st0 + 1, (size_t)f->n_int);
        memmove(f->int_pcn, f->int_pcn + 1, (size_t)f->n_int);
        set_irq(f, false);       /* the next event raises a fresh edge */
        result(f, r, 2, 0, false);
        return;
    }
    case 0x0F: {                                   /* seek */
        uint8_t st0 = (uint8_t)(ST0_SEEK_END | (c[1] & 7));
        if (f->drive[ds]) f->pcn[ds] = c[2];
        else st0 |= ST0_ABNORMAL | ST0_EQUIP;
        seek_int(f, st0, f->pcn[ds]);
        result(f, NULL, 0, DELAY_SEEK, true);
        return;
    }
    case 0x0A: {                                   /* read ID */
        GemuDrive *d = f->drive[ds];
        int head = (c[1] >> 2) & 1;
        Geometry g;
        uint8_t r[7] = { (uint8_t)(c[1] & 7), 0, 0, f->pcn[ds], (uint8_t)head, 1, 2 };
        if (!d || !gemu_drive_inserted(d) || !geometry(d, &g) || head >= g.heads) {
            r[0] |= ST0_ABNORMAL;
            r[1] = ST1_MA;
        }
        result(f, r, 7, DELAY_RW, true);
        return;
    }
    case 0x05: case 0x09: do_rw(f, true);  return;  /* write (deleted) data */
    case 0x06: case 0x0C: do_rw(f, false); return;  /* read (deleted) data */
    case 0x0D: do_format(f); return;
    default: {                                     /* invalid / unsupported */
        uint8_t r = ST0_INVALID;
        result(f, &r, 1, 0, false);
        return;
    }
    }
}

uint8_t upd765_read(Upd765 *f, uint16_t port) {
    switch (port & 7) {
    case 4: {                                      /* main status register */
        if (f->busy) return MSR_CB;
        if (f->res_pos < f->res_len) return MSR_RQM | MSR_DIO | MSR_CB;
        uint8_t msr = MSR_RQM;
        if (f->cmd_len) msr |= MSR_CB;
        return msr;
    }
    case 5: {                                      /* data: result bytes */
        if (f->busy || f->res_pos >= f->res_len) return 0xFF;
        /* Reading the result of a read/write/format clears its interrupt. */
        if (f->res_pos == 0 && f->res_len == 7) set_irq(f, false);
        uint8_t v = f->res[f->res_pos++];
        if (f->res_pos >= f->res_len) f->res_len = f->res_pos = 0;
        return v;
    }
    default:
        return 0xFF;
    }
}

void upd765_write(Upd765 *f, uint16_t port, uint8_t val) {
    switch (port & 7) {
    case 2: {                                      /* digital output register */
        uint8_t old = f->dor;
        f->dor = val;
        if (!(val & 0x04)) {                       /* reset held */
            upd765_reset(f);
        } else if (!(old & 0x04)) {
            /* Leaving reset: the controller reports a ready-line change on
             * all four drives, signalled by one interrupt. */
            upd765_reset(f);
            for (int d = 0; d < 4; d++) push_int(f, (uint8_t)(ST0_READY_CHG | d), f->pcn[d]);
            f->busy = DELAY_RESET;
            f->irq_after = true;
        } else {
            set_irq(f, f->irq);                    /* DMA/IRQ enable may have changed */
        }
        return;
    }
    case 5:                                        /* data: command bytes */
        if (f->busy || f->res_pos < f->res_len) return;
        if (f->cmd_len == 0) f->cmd_need = command_length(val);
        f->cmd[f->cmd_len++] = val;
        if (f->cmd_len >= f->cmd_need) {
            execute(f);
            f->cmd_len = 0;
        }
        return;
    default:
        return;
    }
}

void upd765_tick(Upd765 *f, uint32_t cpu_clocks) {
    if (!f->busy) return;
    if (cpu_clocks < f->busy) { f->busy -= cpu_clocks; return; }
    f->busy = 0;
    if (f->irq_after) {
        f->irq_after = false;
        set_irq(f, true);
    }
}
