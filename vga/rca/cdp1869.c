#include "cdp1869.h"
#include "cdp1802.h"
#include <string.h>

#define CH_WIDTH 6
#define CDB0 0
#define CDB1 1
#define CDB2 2
#define CDB3 3
#define CDB4 4
#define CDB5 5
#define CCB0 6
#define CCB1 7
void cdp1869_init(Cdp1869 *vis) {
    memset(vis, 0, sizeof(*vis));
    vis->non_display = true;
    vis->page_ram_mask = CDP1869_PAGE_RAM_SIZE - 1u;
    vis->page_mask = 0x3ffu;
    vis->max_page = 480u;
    vis->char_stride = 8u;
    vis->block_cpu_access = true;
    vis->dirty = true;
}

void cdp1869_reset(Cdp1869 *vis) {
    memset(vis->page_ram, 0, sizeof(vis->page_ram));
    memset(vis->char_ram, 0, sizeof(vis->char_ram));
    memset(vis->pcb_ram, 0, sizeof(vis->pcb_ram));
    memset(vis->bitmap, 0, sizeof(vis->bitmap));
    vis->prd = false;
    vis->non_display = true;
    vis->lc = 0;
    vis->mc = 0;
    vis->dispoff = false;
    vis->fresvert = false;
    vis->freshorz = false;
    vis->cmem = false;
    vis->dblpage = false;
    vis->line16 = false;
    vis->line9 = false;
    vis->cfc = false;
    vis->col = 0;
    vis->bkg = 0;
    vis->pma = 0;
    vis->hma = 0;
    if (vis->page_ram_mask == 0)
        vis->page_ram_mask = CDP1869_PAGE_RAM_SIZE - 1u;
    if (vis->char_stride == 0)
        vis->char_stride = 8u;
    vis->block_cpu_access = true;
    vis->page_mask = 0x3ffu;
    vis->max_page = 480u;
    vis->q = 0;
    vis->pcb = 0;
    vis->dirty = true;
}

void cdp1869_set_page_ram_mask(Cdp1869 *vis, uint16_t mask) {
    vis->page_ram_mask = mask & (CDP1869_PAGE_RAM_SIZE - 1u);
}

void cdp1869_set_char_stride(Cdp1869 *vis, uint8_t stride) {
    vis->char_stride = stride ? stride : 8u;
}

void cdp1869_set_block_cpu_access(Cdp1869 *vis, bool block) {
    vis->block_cpu_access = block;
}

static int get_lines(const Cdp1869 *vis) {
    if (vis->line16 && !vis->dblpage) return 16;
    if (!vis->line9) return 9;
    return 8;
}

static int get_rows(const Cdp1869 *vis) {
    return vis->fresvert ? 24 : 12;
}

static int get_cols(const Cdp1869 *vis) {
    return vis->freshorz ? 40 : 20;
}

static void update_page_geometry(Cdp1869 *vis) {
    vis->page_mask = vis->dblpage ? 0x7ffu : 0x3ffu;
    vis->max_page = vis->dblpage ? 1920u :
        (uint16_t)(get_rows(vis) * get_cols(vis));
}

static uint16_t get_pma(const Cdp1869 *vis) {
    return vis->dblpage ? vis->pma : (vis->pma & 0x3ffu);
}

static uint16_t char_addr(const Cdp1869 *vis, uint16_t pma, uint8_t cma, uint8_t pmd) {
    uint8_t column = (pma & 0x400u) ? 0xffu : pmd;
    uint8_t stride = vis->char_stride ? vis->char_stride : 8u;
    return (uint16_t)((column * stride + cma % stride) &
                      (CDP1869_CHAR_RAM_SIZE - 1u));
}

static uint8_t page_read_raw(Cdp1869 *vis, uint16_t offset) {
    uint16_t pma = vis->cmem ? get_pma(vis) : offset;
    return vis->page_ram[pma & vis->page_ram_mask];
}

uint8_t cdp1869_char_read(Cdp1869 *vis, uint16_t offset) {
    if (vis->block_cpu_access && !vis->non_display)
        return 0xff;
    uint8_t cma = (uint8_t)(offset & 0x0f);
    uint16_t pma = vis->cmem ? get_pma(vis) : offset;
    if (vis->dblpage) cma &= 0x07u;
    uint8_t pmd = page_read_raw(vis, pma);
    uint16_t addr = char_addr(vis, pma, cma, pmd);
    vis->pcb = vis->pcb_ram[addr] & 1u;
    return vis->char_ram[addr];
}

void cdp1869_char_write(Cdp1869 *vis, uint16_t offset, uint8_t data) {
    if (vis->block_cpu_access && !vis->non_display)
        return;
    uint8_t cma = (uint8_t)(offset & 0x0f);
    uint16_t pma = vis->cmem ? get_pma(vis) : offset;
    if (vis->dblpage) cma &= 0x07u;
    uint8_t pmd = page_read_raw(vis, pma);
    uint16_t addr = char_addr(vis, pma, cma, pmd);
    vis->char_ram[addr] = data;
    vis->pcb_ram[addr] = vis->q & 1u;
    vis->dirty = true;
}

uint8_t cdp1869_page_read(Cdp1869 *vis, uint16_t offset) {
    if (vis->block_cpu_access && !vis->non_display)
        return 0xff;
    return page_read_raw(vis, offset);
}

void cdp1869_page_write(Cdp1869 *vis, uint16_t offset, uint8_t data) {
    if (vis->block_cpu_access && !vis->non_display)
        return;
    uint16_t pma = vis->cmem ? get_pma(vis) : offset;
    vis->page_ram[pma & vis->page_ram_mask] = data;
    vis->dirty = true;
}

void cdp1869_out(Cdp1869 *vis, uint8_t port, uint16_t ma, uint8_t data) {
    switch (port) {
    case 3:
        vis->bkg = data & 0x07u;
        vis->cfc = (data & 0x08u) != 0;
        vis->dispoff = (data & 0x10u) != 0;
        vis->col = (data >> 5) & 0x03u;
        vis->freshorz = (data & 0x80u) != 0;
        update_page_geometry(vis);
        break;
    case 4:
        /* tone amp[3:0], freq_sel[6:4], tone_off[7], divider[14:8] */
        vis->tone_amp      = (uint8_t)(ma & 0x0fu);
        vis->tone_freq_sel = (uint8_t)((ma >> 4) & 0x07u);
        vis->tone_off      = (ma & 0x80u) != 0;
        vis->tone_div      = (uint8_t)((ma >> 8) & 0x7fu);
        break;
    case 5:
        vis->cmem = (ma & 0x0001u) != 0;
        vis->line9 = (ma & 0x0008u) != 0;
        vis->line16 = (ma & 0x0020u) != 0;
        vis->dblpage = (ma & 0x0040u) != 0;
        vis->fresvert = (ma & 0x0080u) != 0;
        update_page_geometry(vis);
        if (vis->cmem) vis->pma = ma;
        else vis->pma = 0;
        break;
    case 6:
        vis->pma = ma & 0x07ffu;
        break;
    case 7:
        vis->hma = ma & vis->page_mask & 0x07fcu;
        break;
    default:
        break;
    }
    vis->dirty = true;
}

static uint8_t get_pen(const Cdp1869 *vis, int ccb0, int ccb1, int pcb) {
    int r = 0, g = 0, b = 0;
    switch (vis->col) {
    case 0: r = ccb0; b = ccb1; g = pcb; break;
    case 1: r = ccb0; b = pcb;  g = ccb1; break;
    default: r = pcb; b = ccb0; g = ccb1; break;
    }
    uint8_t color = (uint8_t)((r << 2) | (b << 1) | g);
    if (vis->cfc)
        return (uint8_t)(color + ((vis->bkg + 1u) * 8u));
    return color;
}

static void plot(Cdp1869 *vis, int x, int y, uint8_t color) {
    if (x < 0 || x >= CDP1869_VISIBLE_W || y < 0 || y >= CDP1869_VISIBLE_H)
        return;
    vis->bitmap[y * CDP1869_VISIBLE_W + x] = color;
}

static void draw_line(Cdp1869 *vis, int x, int y, uint8_t data, uint8_t color) {
    data <<= 2;
    for (int i = 0; i < CH_WIDTH; i++) {
        if (data & 0x80u) {
            plot(vis, x, y, color);
            if (!vis->fresvert) plot(vis, x, y + 1, color);
            if (!vis->freshorz) {
                plot(vis, x + 1, y, color);
                if (!vis->fresvert) plot(vis, x + 1, y + 1, color);
            }
        }
        if (!vis->freshorz) x++;
        x++;
        data <<= 1;
    }
}

static void draw_char(Cdp1869 *vis, int x, int y, uint16_t pma) {
    uint8_t pmd = page_read_raw(vis, pma);
    for (uint8_t cma = 0; cma < get_lines(vis); cma++) {
        uint16_t addr = char_addr(vis, pma, cma, pmd);
        uint8_t data = vis->char_ram[addr];
        int ccb0 = (data >> CCB0) & 1u;
        int ccb1 = (data >> CCB1) & 1u;
        int pcb = vis->pcb_ram[addr] & 1u;
        draw_line(vis, x, y, data, get_pen(vis, ccb0, ccb1, pcb));
        y++;
        if (!vis->fresvert) y++;
    }
}

void cdp1869_render(Cdp1869 *vis) {
    memset(vis->bitmap, vis->bkg & 0x07u, sizeof(vis->bitmap));
    if (!vis->dispoff) {
        int width = vis->freshorz ? CH_WIDTH : CH_WIDTH * 2;
        int height = get_lines(vis);
        if (!vis->fresvert) height *= 2;
        int cols = get_cols(vis);
        int rows = get_rows(vis);
        uint16_t addr = vis->hma;
        while (addr >= vis->max_page)
            addr = (uint16_t)(addr - vis->max_page);
        for (int sy = 0; sy < rows; sy++) {
            for (int sx = 0; sx < cols; sx++) {
                draw_char(vis, sx * width, sy * height, addr);
                addr++;
                if (vis->page_mask == 0x3ffu) {
                    if (addr >= vis->max_page) addr = 0;
                } else {
                    if (addr >= (uint16_t)(vis->max_page + 0x400u)) addr = 0;
                }
            }
        }
    }
    vis->dirty = false;
}

void cdp1869_sync(Cdp1869 *vis, Cdp1802 *cpu) {
    uint16_t lc = vis->lc;
    uint8_t  mc = vis->mc;

    /*
     * non_display: true during VBlank (lines 0..DISPLAY_START-1 and
     * DISPLAY_END..LINES_TOTAL-1) so the CPU can write to page/char RAM.
     * false during active display so the 1869 owns the data bus.
     */
    vis->non_display = (lc < CDP1869_DISPLAY_START || lc >= CDP1869_DISPLAY_END);

    /*
     * Interrupt at line CDP1869_INT_LINE, machine cycle 2.
     * This fires near the start of VBlank, giving the CPU time to update
     * page RAM before the display period begins at line CDP1869_DISPLAY_START.
     */
    if (lc == CDP1869_INT_LINE && mc == 2)
        cdp1802_request_irq(cpu);

    /* Advance scan-line and machine-cycle counters. */
    if (++vis->mc == CDP1869_MCYCLES_PER_LINE) {
        vis->mc = 0;
        if (++vis->lc == CDP1869_LINES_TOTAL)
            vis->lc = 0;
    }
}
