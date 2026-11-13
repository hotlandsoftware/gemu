/* ATI Rage 128 (see rage128.h; "RRG" = RAGE 128 VR/GL Register Reference
 * Guide).
 *
 * Modelled:
 *  - PCI configuration (RRG 4.1): IDs, class 0300, BARs MEM_BASE (64 MiB
 *    prefetchable, 31:26), IO_BASE (256 bytes, 31:8), REG_BASE (16 KiB,
 *    31:14), BIOS_ROM (128 KiB, 31:17, enable bit 0); ADAPTER_ID written
 *    through its 4Ch alias ADAPTER_ID_W; the AGP and power-management
 *    capability blocks;
 *  - memory map (RRG 2.2): register aperture (MMR offsets; 0xF00-0xFFF a
 *    read-only copy of configuration space; 0x00-0xFF also in the I/O BAR,
 *    Table 2-1), MM_INDEX/MM_DATA indirect access to registers and (MM_APER,
 *    bit 31) the frame buffer, the linear aperture's two 32 MiB images of the
 *    frame buffer, each with the byte swapping CONFIG_CNTL selects;
 *  - the register file with the RRG reset values; the PLL registers through
 *    CLOCK_CNTL_INDEX/DATA; the palette; the raster (CRTC_STATUS,
 *    CRTC_VLINE_CRNT_VLINE, GEN_INT_STATUS VBLANK/VLINE) from the pixel
 *    clock; the DAC load-sense comparators; GPIO_MONID's pads;
 *  - legacy VGA ports 3B0-3DF and the 0xA0000 window, served by the VGA core;
 *  - native CRTC scan-out with the hardware cursor; the 2D engine
 *    (rage128_gui.c).
 * A register whose behaviour is not modelled is named the first time it is
 * accessed (r128_unimpl), never silently stored. */
#include "rage128_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Register offsets used by name below. */
enum {
    MM_INDEX = 0x000, MM_DATA = 0x004, CLOCK_CNTL_INDEX = 0x008,
    CLOCK_CNTL_DATA = 0x00C, BIOS_0_SCRATCH = 0x010, BIOS_3_SCRATCH = 0x01C,
    BUS_CNTL = 0x030, BUS_CNTL1 = 0x034, GEN_INT_CNTL = 0x040,
    GEN_INT_STATUS = 0x044, CRTC_GEN_CNTL = 0x050, CRTC_EXT_CNTL = 0x054,
    DAC_CNTL = 0x058, CRTC_STATUS = 0x05C, GPIO_MONID = 0x068,
    PALETTE_INDEX = 0x0B0, PALETTE_DATA = 0x0B4, CONFIG_CNTL = 0x0E0,
    GEN_RESET_CNTL = 0x0F0, CONFIG_MEMSIZE = 0x0F8, CONFIG_APER_0_BASE = 0x100,
    CONFIG_APER_1_BASE = 0x104, CONFIG_APER_SIZE = 0x108,
    CONFIG_REG_1_BASE = 0x10C, CONFIG_REG_APER_SIZE = 0x110,
    TEST_DEBUG_MUX = 0x124, HW_DEBUG = 0x128, HOST_PATH_CNTL = 0x130,
    MEM_CNTL = 0x140, MEM_SDRAM_MODE_REG = 0x158, PC_NGUI_MODE = 0x180,
    CRTC_H_TOTAL_DISP = 0x200, CRTC_V_TOTAL_DISP = 0x208,
    CRTC_VLINE_CRNT_VLINE = 0x210, CRTC_OFFSET = 0x224,
    CRTC_OFFSET_CNTL = 0x228, CRTC_PITCH = 0x22C, OVR_CLR = 0x230,
    OVR_WID_LEFT_RIGHT = 0x234, OVR_WID_TOP_BOTTOM = 0x238, CUR_OFFSET = 0x260,
    CUR_HORZ_VERT_POSN = 0x264, CUR_HORZ_VERT_OFF = 0x268, CUR_CLR0 = 0x26C,
    CUR_CLR1 = 0x270, DDA_CONFIG = 0x2E0, DDA_ON_OFF = 0x2E4,
};

/* PLL registers (indices). */
enum {
    PPLL_REF_DIV = 0x03, PLL_DIV_0 = 0x04, PLL_DIV_3 = 0x07, VCLK_ECP_CNTL = 0x08,
    PLL_TEST_CNTL = 0x13,
};

#define REG(off) (r->regs[(off) / 4])

/* ---- diagnostics ---------------------------------------------------------- */

void r128_unimpl(const char *key, const char *fmt, ...) {
    enum { NKEYS = 128 };
    static char seen[NKEYS][40];
    static unsigned nseen;
    for (unsigned i = 0; i < nseen; i++) {
        if (!strcmp(seen[i], key)) {
            return;
        }
    }
    if (nseen >= NKEYS) {
        return;
    }
    snprintf(seen[nseen++], sizeof seen[0], "%s", key);
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "rage128: unimplemented: %s\n", msg);
}

static uint16_t get_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void put_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static const Rage128RegInfo *reg_info(uint32_t off) {
    unsigned lo = 0, hi = RAGE128_NREGS;
    while (lo < hi) {
        const unsigned mid = (lo + hi) / 2;
        if (rage128_regs[mid].offset == off) {
            return &rage128_regs[mid];
        }
        if (rage128_regs[mid].offset < off) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

static void reset_regs(Rage128 *r) {
    memset(r->regs, 0, sizeof r->regs);
    for (unsigned k = 0; k < RAGE128_NREGS; k++) {
        if (rage128_regs[k].offset < RAGE128_REG_APER_SIZE) {
            r->regs[rage128_regs[k].offset / 4] = rage128_regs[k].reset;
        }
    }
    memset(r->pll, 0, sizeof r->pll);
    for (unsigned k = 0; k < RAGE128_NPLL; k++) {
        r->pll[rage128_pll_regs[k].offset & 63] = rage128_pll_regs[k].reset;
    }
    r128_gui_reset(r);
}

/* ---- registers ------------------------------------------------------------ */

/* Registers whose behaviour is modelled (beyond storage), and registers that
 * are pure storage by their definition. Anything else is reported. */
static bool reg_known(uint32_t off) {
    switch (off) {
    case MM_INDEX: case MM_DATA: case CLOCK_CNTL_INDEX: case CLOCK_CNTL_DATA:
    case BIOS_0_SCRATCH: case BIOS_0_SCRATCH + 4: case BIOS_0_SCRATCH + 8:
    case BIOS_3_SCRATCH:
    case GPIO_MONID: case PALETTE_INDEX: case PALETTE_DATA:
    case CRTC_GEN_CNTL: case CRTC_EXT_CNTL: case DAC_CNTL: case CRTC_STATUS:
    case CRTC_VLINE_CRNT_VLINE: case GEN_INT_STATUS: case GEN_INT_CNTL:
    case CRTC_H_TOTAL_DISP: case CRTC_H_TOTAL_DISP + 4:   /* H_TOTAL_DISP, H_SYNC_STRT_WID */
    case CRTC_V_TOTAL_DISP: case CRTC_V_TOTAL_DISP + 4:   /* V_TOTAL_DISP, V_SYNC_STRT_WID */
    case CRTC_OFFSET: case CRTC_OFFSET_CNTL: case CRTC_PITCH:
    case CUR_OFFSET: case CUR_HORZ_VERT_POSN: case CUR_HORZ_VERT_OFF:
    case CUR_CLR0: case CUR_CLR1:
    /* Settings with no effect a program can observe in this model (bus
     * interface, memory controller timing and sizing, display FIFO
     * arbitration, overscan border, debug), kept as written: */
    case BUS_CNTL: case BUS_CNTL1: case GEN_RESET_CNTL: case CONFIG_MEMSIZE:
    case HW_DEBUG: case HOST_PATH_CNTL:
    case MEM_CNTL: case MEM_CNTL + 4: case MEM_CNTL + 8: case MEM_CNTL + 12:
    case MEM_CNTL + 16: case MEM_CNTL + 20: case MEM_SDRAM_MODE_REG:
    case PC_NGUI_MODE:
    case OVR_CLR: case OVR_WID_LEFT_RIGHT: case OVR_WID_TOP_BOTTOM:
    case DDA_CONFIG: case DDA_ON_OFF:
    case CONFIG_CNTL:
    case CONFIG_APER_0_BASE: case CONFIG_APER_1_BASE: case CONFIG_APER_SIZE:
    case CONFIG_REG_1_BASE: case CONFIG_REG_APER_SIZE:
        return true;
    default:
        return false;
    }
}

/* GPIO_MONID (RRG 3.1): MONID_A 3:0 drive values, MONID_Y 11:8 pad levels,
 * MONID_EN 19:16 (1 = output), MONID_MASK 27:24; "MONID2 pin = CLK, MONID1
 * pin = DATA" for DDC. A pad not driven reads its line, pulled up. No DDC
 * slave is attached to the monitor connector, so nothing else ever pulls a
 * line low. */
static uint32_t gpio_monid_pins(Rage128 *r) {
    const uint32_t g = REG(GPIO_MONID);
    unsigned level = 0;
    for (unsigned k = 0; k < 4; k++) {
        const bool out = g & (1u << (16 + k));
        const bool v = out ? (g >> k) & 1u : true;
        level |= (unsigned)v << k;
    }
    if ((g & (1u << 18)) || (g & (1u << 17))) {
        r128_unimpl("ddc", "DDC on GPIO_MONID: no monitor EDID EEPROM is attached "
                    "(lines float high)");
    }
    return level;
}

/* ---- raster timing --------------------------------------------------------
 * The pixel clock (RRG 3.4, PLL_DIV_[3:0]): PPllClk = N * PPLL_REF / M with
 * N = PPLL_FBn_DIV of the PLL_DIV_n selected by PPLL_DIV_SEL
 * (CLOCK_CNTL_INDEX 9:8), M = PPLL_REF_DIV, PPLL_REF = XTALIN
 * (PPLL_REF_DIV_SRC 0); VCLK = PPllClk / PPLL_POSTn_DIV (0-7: /1, /2, /4, /8,
 * /3, reserved, /6, /12) with VCLK_SRC_SEL = PPllClk (VCLK_ECP_CNTL). XTALIN
 * is the 29.50 MHz crystal of the reference design. The CRTC (RRG 5):
 * H_TOTAL and H_DISP in units of 8 pixels (+1), V_TOTAL and V_DISP in lines
 * (+1). The beam position is the pixel count since clock 0 modulo the
 * frame. */
#define XTALIN_HZ 29500000u

typedef struct {
    uint32_t htot, vtot, hdisp, vdisp;
    uint64_t num, den;           /* pixel clock = num / den pixels per clock tick */
} Raster;

static bool raster_timing(const Rage128 *r, Raster *t) {
    const uint32_t h = REG(CRTC_H_TOTAL_DISP), v = REG(CRTC_V_TOTAL_DISP);
    t->htot = ((h & 0x1FFu) + 1) * 8;
    t->hdisp = (((h >> 16) & 0xFFu) + 1) * 8;
    t->vtot = (v & 0x7FFu) + 1;
    t->vdisp = ((v >> 16) & 0x7FFu) + 1;
    if ((r->pll[VCLK_ECP_CNTL] & 3u) != 3u) {
        /* VCLK_SRC_SEL 0 = CPUCLK: the VGA clock path. Only the PLL-driven
         * extended mode's raster is modelled. */
        return false;
    }
    if ((r->pll[PPLL_REF_DIV] >> 16) & 3u) {
        r128_unimpl("ppll-ref-src", "pixel PLL reference other than XTALIN "
                    "(PPLL_REF_DIV_SRC %u)", (r->pll[PPLL_REF_DIV] >> 16) & 3u);
        return false;
    }
    static const unsigned post[8] = { 1, 2, 4, 8, 3, 0, 6, 12 };
    const uint32_t m = r->pll[PPLL_REF_DIV] & 0x3FFu;
    const uint32_t div = r->pll[PLL_DIV_0 + ((REG(CLOCK_CNTL_INDEX) >> 8) & 3u)];
    const uint32_t n = div & 0x7FFu, p = post[(div >> 16) & 7u];
    if (m < 2 || n < 4 || !p) {
        return false;  /* "divider stops operating" / reserved */
    }
    t->num = (uint64_t)XTALIN_HZ * n;
    t->den = (uint64_t)m * p * r->wiring.clock_hz;
    return t->htot && t->vtot;
}

static uint64_t raster_pixels(const Rage128 *r, const Raster *t) {
    return (uint64_t)(((unsigned __int128)r->wiring.clock(r->wiring.opaque) * t->num) / t->den);
}

/* The first pixel count >= after at which line `line` starts. */
static uint64_t next_line_start(const Raster *t, uint64_t after, uint32_t line) {
    const uint64_t frame = (uint64_t)t->htot * t->vtot, at = (uint64_t)line * t->htot;
    const uint64_t f = after / frame, x = f * frame + at;
    return x >= after ? x : x + frame;
}

/* GEN_INT_STATUS bits 0 (CRTC_VBLANK_INT) and 1 (CRTC_VLINE_INT): latched
 * when the vertical blank / the CRTC_VLINE line begins, until acknowledged. */
static uint32_t int_status(const Rage128 *r, const Raster *t, uint64_t now) {
    const uint32_t vl = REG(CRTC_VLINE_CRNT_VLINE) & 0x7FFu;
    return (next_line_start(t, r->int_vblank_clear, t->vdisp) <= now ? 1u : 0u) |
           (next_line_start(t, r->int_vline_clear, vl) <= now ? 2u : 0u);
}

/* INTA#: GEN_INT_CNTL CRTC_VBLANK_INT_EN / CRTC_VLINE_INT_EN over the status;
 * other enables are reported where they are written. */
static void update_irq(Rage128 *r) {
    bool p = false;
    const uint32_t en = REG(GEN_INT_CNTL);
    Raster t;
    if ((en & 3u) && raster_timing(r, &t)) {
        p = (int_status(r, &t, raster_pixels(r, &t)) & en & 3u) != 0;
    }
    if (p != r->irq_out) {
        r->irq_out = p;
        r->wiring.irq(r->wiring.opaque, p);
    }
}

void rage128_tick(Rage128 *r) {
    if (REG(GEN_INT_CNTL) & 3u) {
        update_irq(r);
    }
}

/* Input status 1 (3DA) of the VGA core's synthetic 60 Hz frame: vertical
 * retrace (bit 3) over the last 6.3% of it and display disabled (bit 0)
 * over the last 9.5%. */
static uint8_t vga_status1(const Rage128 *r) {
    const uint64_t hz = r->wiring.clock_hz;
    const uint64_t frame = hz / 60 ? hz / 60 : 1;
    const uint64_t phase = (r->wiring.clock(r->wiring.opaque) % frame) * 19886u / frame;
    return (uint8_t)((phase >= 18636u ? 0x08 : 0) | (phase >= 18000u ? 0x01 : 0));
}

/* Colour (0xRRGGBB) of the frame buffer pixel (x, y) as scanned out. */
static uint32_t scan_rgb(const Rage128 *r, uint32_t x, uint32_t y) {
    const uint32_t gen = REG(CRTC_GEN_CNTL);
    const unsigned pw = (gen >> 8) & 7u;
    static const unsigned bytes[8] = { 0, 0, 1, 2, 2, 3, 4, 0 };
    if (!bytes[pw]) {
        return 0;
    }
    const uint32_t pitch = (REG(CRTC_PITCH) & 0x3FFu) * 8 * bytes[pw];
    const uint32_t a = (REG(CRTC_OFFSET) & 0x1FFFFFFu) + y * pitch + x * bytes[pw];
    if (a + bytes[pw] > r128_vram_bytes(r)) {
        return 0;
    }
    const uint8_t *s = r->vga->vram + a;
    uint32_t rr, gg, bb;
    switch (pw) {
    case 2:
        return (uint32_t)r->palette[s[0]][0] << 16 | (uint32_t)r->palette[s[0]][1] << 8 |
               r->palette[s[0]][2];
    case 3: {
        const uint16_t p = get_le16(s);
        rr = (p >> 10) & 31u;
        gg = (p >> 5) & 31u;
        bb = p & 31u;
        rr = rr << 3 | rr >> 2;
        gg = gg << 3 | gg >> 2;
        bb = bb << 3 | bb >> 2;
        break;
    }
    case 4: {
        const uint16_t p = get_le16(s);
        rr = (p >> 11) & 31u;
        gg = (p >> 5) & 63u;
        bb = p & 31u;
        rr = rr << 3 | rr >> 2;
        gg = gg << 2 | gg >> 4;
        bb = bb << 3 | bb >> 2;
        break;
    }
    default:
        bb = s[0];
        gg = s[1];
        rr = s[2];
        break;
    }
    return rr << 16 | gg << 8 | bb;
}

/* DAC_CNTL DAC_CMP_OUTPUT (RRG 6.6): with DAC_COMP_EN, "1 = All 3
 * comparators < ~0.373V"; the comparators sense "if the termination on the
 * R,G&B lines is 75 ohms (no monitor) or 37.5 ohms (monitor present)". A
 * monitor is attached: the DAC's full scale is the 0.7 V of a
 * double-terminated line. The level compared is the colour being output:
 * OVR_CLR while the CRTC does not display (BIOSes turn CRTC_EN off, set
 * OVR_CLR and read), the pixel under the beam while it does. */
static bool dac_cmp_output(const Rage128 *r) {
    const uint32_t dac = REG(DAC_CNTL);
    if (!(dac & 0x08u)) {  /* DAC_COMP_EN */
        return false;
    }
    const uint32_t gen = REG(CRTC_GEN_CNTL), ext = REG(CRTC_EXT_CNTL);
    const bool displaying = (gen & (1u << 25)) && (gen & (1u << 24)) && !(ext & (1u << 10));
    uint32_t c = REG(OVR_CLR);  /* R 23:16, G 15:8, B 7:0 */
    if (displaying) {
        Raster t;
        if (!raster_timing(r, &t)) {
            return false;
        }
        const uint64_t pos = raster_pixels(r, &t) % ((uint64_t)t.htot * t.vtot);
        const uint32_t line = (uint32_t)(pos / t.htot), col = (uint32_t)(pos % t.htot);
        c = (line < t.vdisp && col < t.hdisp) ? scan_rgb(r, col, line) : 0;
    }
    for (unsigned k = 0; k < 3; k++) {
        if (((c >> (8 * k)) & 0xFFu) * 700u / 255u >= 373u) {
            return false;
        }
    }
    return true;
}

/* ---- PLL registers (RRG 3.4): CLOCK_CNTL_INDEX PLL_ADDR 4:0, PLL_WR_EN 7,
 * PPLL_DIV_SEL 9:8; CLOCK_CNTL_DATA the data ---------------------------------- */

static const Rage128RegInfo *pll_info(unsigned idx) {
    for (unsigned k = 0; k < RAGE128_NPLL; k++) {
        if (rage128_pll_regs[k].offset == idx) {
            return &rage128_pll_regs[k];
        }
    }
    return NULL;
}

static unsigned pll_index(const Rage128 *r) {
    return REG(CLOCK_CNTL_INDEX) & 0x1Fu;
}

static uint32_t pll_read(const Rage128 *r, unsigned idx) {
    if (!pll_info(idx)) {
        char key[24];
        snprintf(key, sizeof key, "pll-%02x", idx);
        r128_unimpl(key, "PLL register 0x%02X read (not in the RRG)", idx);
    }
    uint32_t v = r->pll[idx & 63u];
    /* PPLL_REF_DIV and PLL_DIV_[3:0] bit 15, PPLL_ATOMIC_UPDATE_R: "Indicates
     * progress of last request for update"; the update completes at once. */
    if (idx >= PPLL_REF_DIV && idx <= PLL_DIV_3) {
        v &= ~0x8000u;
    }
    /* PLL_TEST_CNTL TEST_COUNT (31:24) counts edges of a test clock the RRG
     * leaves undescribed (TEST_DEBUG_MUX selects it). With no source for the
     * clock the counter holds what was written. */
    if (idx == PLL_TEST_CNTL && (REG(TEST_DEBUG_MUX) & 0x100u)) {
        r128_unimpl("pll-test-count", "PLL_TEST_CNTL TEST_COUNT clock (TEST_DEBUG_MUX 0x%08X): "
                    "source undocumented, counter does not advance", REG(TEST_DEBUG_MUX));
    }
    return v;
}

static void pll_write(Rage128 *r, unsigned idx, uint32_t v) {
    if (!pll_info(idx)) {
        char key[24];
        snprintf(key, sizeof key, "pll-%02x", idx);
        r128_unimpl(key, "PLL register 0x%02X written (not in the RRG)", idx);
    }
    r->pll[idx & 63u] = v;
}

/* ---- palette (RRG 6.6): PALETTE_INDEX W 7:0, R 23:16, auto-incrementing;
 * PALETTE_DATA R 23:16 G 15:8 B 7:0; DAC_8BIT_EN = 0: 6-bit, writes shifted
 * left by 2, reads right by 2 ---------------------------------------------- */

static void palette_write(Rage128 *r, uint32_t v) {
    const bool bit8 = REG(DAC_CNTL) & 0x100u;
    const unsigned i = REG(PALETTE_INDEX) & 0xFFu;
    for (unsigned c = 0; c < 3; c++) {
        const uint8_t x = (uint8_t)(v >> (16 - 8 * c));
        r->palette[i][c] = bit8 ? x : (uint8_t)(x << 2);
    }
    REG(PALETTE_INDEX) = (REG(PALETTE_INDEX) & ~0xFFu) | ((i + 1) & 0xFFu);
}

static uint32_t palette_read(Rage128 *r) {
    const bool bit8 = REG(DAC_CNTL) & 0x100u;
    const unsigned i = (REG(PALETTE_INDEX) >> 16) & 0xFFu;
    uint32_t v = 0;
    for (unsigned c = 0; c < 3; c++) {
        v |= (uint32_t)(bit8 ? r->palette[i][c] : r->palette[i][c] >> 2) << (16 - 8 * c);
    }
    REG(PALETTE_INDEX) = (REG(PALETTE_INDEX) & ~0xFF0000u) | (((i + 1) & 0xFFu) << 16);
    return v;
}

static void report(uint32_t off, bool write) {
    const Rage128RegInfo *i = reg_info(off);
    char key[24];
    snprintf(key, sizeof key, "reg-%03x-%c", off, write ? 'w' : 'r');
    r128_unimpl(key, "%s of register 0x%03X (%s): behaviour not modelled, kept as storage",
                write ? "write" : "read", off, i ? i->name : "not in the RRG");
}

/* ---- the frame buffer ------------------------------------------------------ */

static uint32_t fb_access(Rage128 *r, uint32_t off, unsigned size, bool write, uint32_t v) {
    const unsigned aper = off >> 25;  /* 32 MiB images */
    const uint32_t a = off & ((32u << 20) - 1);
    const uint32_t cntl = REG(CONFIG_CNTL);
    unsigned swap = aper ? (cntl >> 2) & 3u : cntl & 3u;
    if (a + size > r128_vram_bytes(r)) {
        r128_unimpl("fb-beyond", "linear aperture access beyond the frame buffer (+0x%X): "
                    "unpopulated memory decode not modelled", off);
        return write ? 0 : 0xFFFFFFFFu;
    }
    if (swap == 3) {
        r128_unimpl("aper-swap3", "aperture swap mode 3 (reserved)");
        swap = 0;
    }
    /* Byte k of the access is frame buffer byte (a + k) xor the swap: within
     * 16-bit units (1) or 32-bit units (2) (APER_n_ENDIAN). */
    const uint32_t x = swap == 1 ? 1 : swap == 2 ? 3 : 0;
    uint8_t *vram = r->vga->vram;
    if (write) {
        for (unsigned k = 0; k < size; k++) {
            vram[(a + k) ^ x] = (uint8_t)(v >> (8 * k));
        }
        vga_vram_written(r->vga, a, size);
        return 0;
    }
    uint32_t out = 0;
    for (unsigned k = 0; k < size; k++) {
        out |= (uint32_t)vram[(a + k) ^ x] << (8 * k);
    }
    return out;
}

/* ---- register file ---------------------------------------------------------- */

static uint32_t cfg_mirror(const Rage128 *r, uint32_t off) {
    return get_le32(r->cfg + (off & 0xFCu));
}

static uint32_t reg_read32(Rage128 *r, uint32_t off) {
    if (off >= 0xF00 && off < 0x1000) {
        return cfg_mirror(r, off);
    }
    uint32_t gv;
    if (r128_gui_read(r, off, &gv)) {
        return gv;
    }
    if (!reg_known(off)) {
        report(off, false);
    }
    switch (off) {
    case MM_DATA: {
        /* The register (MM_APER 0) or frame buffer dword (MM_APER 1) that
         * MM_INDEX MM_ADDR (26:0) selects (RRG 3.1). */
        const uint32_t idx = REG(MM_INDEX);
        if (idx & 0x80000000u) {
            return fb_access(r, idx & 0x07FFFFFCu, 4, false, 0);
        }
        return reg_read32(r, idx & (RAGE128_REG_APER_SIZE - 4));
    }
    case GPIO_MONID:
        return (REG(off) & ~0xF00u) | gpio_monid_pins(r) << 8;
    case CLOCK_CNTL_DATA:
        return pll_read(r, pll_index(r));
    case PALETTE_DATA:
        return palette_read(r);
    case CRTC_OFFSET_CNTL:
        /* CRTC_GUI_TRIG_OFFSET (30, R): "1 = Last CRTC_OFFSET written not yet
         * displayed": the scan-out takes a new offset at once. */
        return REG(off) & ~(1u << 30);
    case CRTC_STATUS: case CRTC_VLINE_CRNT_VLINE: case GEN_INT_STATUS: {
        Raster t;
        if (!raster_timing(r, &t)) {
            return off == CRTC_VLINE_CRNT_VLINE ? (REG(off) & 0x7FFu) : 0;
        }
        const uint64_t now = raster_pixels(r, &t), frame = (uint64_t)t.htot * t.vtot;
        const uint32_t line = (uint32_t)((now % frame) / t.htot);
        const uint32_t vl = REG(CRTC_VLINE_CRNT_VLINE) & 0x7FFu;
        if (off == CRTC_VLINE_CRNT_VLINE) {
            return vl | line << 16;  /* CRTC_CRNT_VLINE 26:16 */
        }
        if (off == CRTC_STATUS) {
            /* CRTC_VBLANK_CUR; CRTC_VBLANK_SAVE: a vertical blank began since
             * the last clear. */
            return (line >= t.vdisp ? 1u : 0u) |
                   (next_line_start(&t, r->vblank_save_clear, t.vdisp) <= now ? 2u : 0u);
        }
        return int_status(r, &t, now);
    }
    /* RRG 2.2 and 4.1: CONFIG_APER_0_BASE mirrors MEM_BASE; APER_1_BASE is
     * the second 32 MiB image (bit 25 hardwired to one); CONFIG_APER_SIZE
     * 32 MiB; CONFIG_REG_1_BASE the second register aperture (bit 13
     * hardwired to one); CONFIG_REG_APER_SIZE 8 KiB. */
    case CONFIG_APER_0_BASE:
        return cfg_mirror(r, 0x10) & 0xFC000000u;
    case CONFIG_APER_1_BASE:
        return (cfg_mirror(r, 0x10) & 0xFC000000u) | 0x02000000u;
    case CONFIG_APER_SIZE:
        return 0x02000000u;
    case CONFIG_REG_1_BASE:
        return (cfg_mirror(r, 0x18) & 0xFFFFC000u) | 0x2000u;
    case CONFIG_REG_APER_SIZE:
        return RAGE128_REG_APER_SIZE;
    case DAC_CNTL:
        return (REG(off) & ~0x80u) | (dac_cmp_output(r) ? 0x80u : 0u);
    default:
        return REG(off);
    }
}

static void reg_write32(Rage128 *r, uint32_t off, uint32_t v, uint32_t mask) {
    if (off >= 0xF00 && off < 0x1000) {
        return;  /* read-only mirror */
    }
    r->vga->disp_gen++;
    if (r128_gui_write(r, off, v, mask)) {
        return;
    }
    if (!reg_known(off)) {
        report(off, true);
    }
    switch (off) {
    case MM_DATA: {
        const uint32_t idx = REG(MM_INDEX);
        if (idx & 0x80000000u) {
            const uint32_t a = idx & 0x07FFFFFCu;
            const uint32_t old = fb_access(r, a, 4, false, 0);
            fb_access(r, a, 4, true, (old & ~mask) | (v & mask));
            return;
        }
        reg_write32(r, idx & (RAGE128_REG_APER_SIZE - 4), v, mask);
        return;
    }
    case GPIO_MONID:
        REG(off) = (REG(off) & ~mask) | (v & mask & ~0xF00u);
        gpio_monid_pins(r);
        return;
    case CLOCK_CNTL_DATA:
        /* PLL_WR_EN (CLOCK_CNTL_INDEX bit 7) gates writes. */
        if (REG(CLOCK_CNTL_INDEX) & 0x80u) {
            const unsigned idx = pll_index(r);
            pll_write(r, idx, (r->pll[idx & 63u] & ~mask) | (v & mask));
        }
        return;
    case PALETTE_DATA:
        if ((mask & 0x00FFFFFFu) != 0x00FFFFFFu) {
            r128_unimpl("palette-partial", "PALETTE_DATA written with a 0x%08X mask", mask);
        }
        palette_write(r, v);
        return;
    case CONFIG_APER_0_BASE: case CONFIG_APER_SIZE: case CONFIG_REG_1_BASE:
    case CONFIG_REG_APER_SIZE:
        return;  /* read-only (RRG) */
    case CRTC_STATUS: case GEN_INT_STATUS: {
        /* CRTC_VBLANK_SAVE_CLEAR (CRTC_STATUS bit 1); GEN_INT_STATUS
         * acknowledge bits (write 1 to clear). */
        Raster t;
        const uint64_t now = raster_timing(r, &t) ? raster_pixels(r, &t) + 1 : 0;
        const uint32_t w = v & mask;
        if (off == CRTC_STATUS) {
            if (w & 2u) {
                r->vblank_save_clear = now;
            }
            if (w & ~2u) {
                r128_unimpl("crtc-status-w", "CRTC_STATUS write of bits 0x%08X", w & ~2u);
            }
        } else {
            if (w & 1u) {
                r->int_vblank_clear = now;
            }
            if (w & 2u) {
                r->int_vline_clear = now;
            }
            if (w & ~3u) {
                r128_unimpl("gen-int-status-w", "GEN_INT_STATUS acknowledge of sources 0x%08X",
                            w & ~3u);
            }
        }
        update_irq(r);
        return;
    }
    case CRTC_OFFSET_CNTL:
        REG(off) = (REG(off) & ~mask) | (v & mask);
        if (REG(off) & (1u << 15)) {
            r128_unimpl("crtc-tile", "tiled scan-out (CRTC_TILE_EN)");
        }
        if (REG(off) & (1u << 31)) {
            r128_unimpl("crtc-offset-lock", "CRTC_OFFSET_LOCK");
        }
        return;
    case GEN_INT_CNTL:
        REG(off) = (REG(off) & ~mask) | (v & mask);
        if (REG(off) & ~3u) {
            r128_unimpl("gen-int-cntl", "GEN_INT_CNTL enables 0x%08X other than CRTC VBLANK/VLINE",
                        REG(off) & ~3u);
        }
        update_irq(r);
        return;
    case CRTC_VLINE_CRNT_VLINE:
        REG(off) = (REG(off) & ~mask) | (v & mask & 0x7FFu);  /* CRTC_VLINE */
        return;
    case CONFIG_APER_1_BASE:
        r128_unimpl("aper1-write", "CONFIG_APER_1_BASE written (aperture relocation not modelled)");
        return;
    case CONFIG_CNTL:
        mask &= ~0x000F0000u;  /* CFG_ATI_REV_ID (R) */
        REG(off) = (REG(off) & ~mask) | (v & mask);
        return;
    case PALETTE_INDEX:
        /* Byte writes set the read or the write index alone (RRG: "Recommend
         * using byte writes to set either read mode or write mode"). */
        REG(off) = (REG(off) & ~mask) | (v & mask & 0x00FF00FFu);
        return;
    default:
        REG(off) = (REG(off) & ~mask) | (v & mask);
        return;
    }
}

/* Register access of 1, 2 or 4 bytes (little-endian lanes). */
static uint32_t reg_access(Rage128 *r, uint32_t off, unsigned size, bool write, uint32_t v) {
    const uint32_t dw = off & ~3u, sh = 8 * (off & 3u);
    if ((off & 3u) + size > 4) {
        r128_unimpl("reg-span", "register access of %u bytes spanning a dword at 0x%X", size, off);
        return 0;
    }
    const uint32_t mask = (size == 4 ? 0xFFFFFFFFu : (1u << (8 * size)) - 1) << sh;
    if (write) {
        reg_write32(r, dw, v << sh, mask);
        return 0;
    }
    return (reg_read32(r, dw) & mask) >> sh;
}

/* ---- bus interface ---------------------------------------------------------- */

static bool valid_len(unsigned len) {
    return len == 1 || len == 2 || len == 4 || len == 8;
}

/* The frame buffer window (MEM_BASE). */
static void fb_bar(Rage128 *r, bool write, uint32_t off, unsigned len, uint64_t *val) {
    if (len == 8) {
        if (write) {
            fb_access(r, off, 4, true, (uint32_t)*val);
            fb_access(r, off + 4, 4, true, (uint32_t)(*val >> 32));
        } else {
            *val = fb_access(r, off, 4, false, 0) |
                   ((uint64_t)fb_access(r, off + 4, 4, false, 0) << 32);
        }
    } else if (write) {
        fb_access(r, off, len, true, (uint32_t)*val);
    } else {
        *val = fb_access(r, off, len, false, 0);
    }
}

/* The register window: IO_BASE ("Non-GUI registers, memory mapped as well as
 * directly accessible in IOR space: 0000h - 00FFh", RRG Table 2-1) or one of
 * REG_BASE's two identical 8 KiB apertures, where CONFIG_CNTL APER_REG_ENDIAN
 * (bit 4, "1 = Big endian: 32 bpp swapping") applies to dword accesses. */
static void reg_bar(Rage128 *r, bool write, uint32_t off, unsigned len, bool memory,
                    uint64_t *val) {
    const uint32_t o = memory ? off & (RAGE128_REG_APER_SIZE - 1) : off & 0xFFu;
    if (len == 8) {
        if (write) {
            reg_access(r, o, 4, true, (uint32_t)*val);
            reg_access(r, o + 4, 4, true, (uint32_t)(*val >> 32));
        } else {
            *val = reg_access(r, o, 4, false, 0) |
                   ((uint64_t)reg_access(r, o + 4, 4, false, 0) << 32);
        }
        return;
    }
    if (memory && (REG(CONFIG_CNTL) & 0x10u)) {
        if (len != 4 || (o & 3u)) {
            r128_unimpl("reg-swap-size", "byte-swapped register access of %u bytes", len);
            if (!write) {
                *val = 0;
            }
        } else if (write) {
            reg_access(r, o, 4, true, __builtin_bswap32((uint32_t)*val));
        } else {
            *val = __builtin_bswap32(reg_access(r, o, 4, false, 0));
        }
        return;
    }
    if (write) {
        reg_access(r, o, len, true, (uint32_t)*val);
    } else {
        *val = reg_access(r, o, len, false, 0);
    }
}

/* Legacy VGA ports 3B0-3DF. */
static void legacy_io(Rage128 *r, bool write, uint16_t port, unsigned len, uint64_t *val,
                      bool *claimed) {
    *claimed = true;
    if (port == 0x3C3) {
        /* GENENB (RRG 5.1, [R]): BLK_IO_BASE, "Readback of block I/O aperture
         * base offset. Mirror of the PCI configuration space register": IO_BASE
         * 15:8, the 256-byte BAR's base. */
        if (write) {
            r128_unimpl("genenb-w", "write to GENENB (3C3h, read-only): 0x%02X dropped",
                        (unsigned)(*val & 0xFFu));
        } else {
            *val = r->cfg[0x15];
        }
        return;
    }
    if (port == 0x3C2 && !write) {
        /* GENS0 (RRG 5.1): SENSE_SWITCH (4) "Mirror of DAC_CMP_OUTPUT@
         * DAC_CNTL"; CRT_INTR (7), the VGA vertical retrace interrupt, is not
         * modelled (it reads clear). */
        *val = dac_cmp_output(r) ? 0x10u : 0x00u;
        return;
    }
    if ((port == 0x3BA || port == 0x3DA) && !write) {
        r->vga->retrace = vga_status1(r);
    }
    *claimed = vga_io(r->vga, write, port, len, val);
}

/* The 0xA0000-0xBFFFF window, a byte at a time through the VGA core. */
static void legacy_mem(Rage128 *r, bool write, uint32_t off, unsigned len, uint64_t *val) {
    if (write) {
        for (unsigned i = 0; i < len; i++) {
            vga_mem_write(r->vga, off + i, (uint8_t)(*val >> (8 * i)));
        }
        return;
    }
    uint64_t out = 0;
    for (unsigned i = 0; i < len; i++) {
        out |= (uint64_t)vga_mem_read(r->vga, off + i) << (8 * i);
    }
    *val = out;
}

enum { CMD_IO = 0x1u, CMD_MEM = 0x2u };
enum { LEGACY_MEM_BASE = 0xA0000u, LEGACY_MEM_SIZE = 0x20000u };

static uint32_t command(const Rage128 *r) {
    return get_le16(r->cfg + 0x04);
}

/* Address of memory BAR `idx` (0 = FB, 2 = registers), 0 when not placed. */
static uint64_t mem_bar_base(const Rage128 *r, unsigned idx) {
    return get_le32(r->cfg + 0x10 + 4 * idx) & ~(uint32_t)0xF;
}

static uint32_t rom_bar_raw(const Rage128 *r) {
    return get_le32(r->cfg + 0x30);
}

static bool rom_present(const Rage128 *r) {
    return r->rom && r->rom_len;
}

bool rage128_rom_range(const Rage128 *r, uint64_t *base, uint64_t *size) {
    const uint32_t raw = rom_bar_raw(r);
    if (!(command(r) & CMD_MEM) || !rom_present(r) || !(raw & 1u) || !(raw & 0xFFFFF800u)) {
        return false;
    }
    *base = raw & 0xFFFFF800u;
    *size = RAGE128_ROM_BAR_SIZE;
    return true;
}

bool rage128_mem_ranges(const Rage128 *r, uint64_t *fb_base, uint64_t *fb_size,
                        uint64_t *reg_base, uint64_t *reg_size) {
    if (!(command(r) & CMD_MEM)) {
        return false;
    }
    *fb_base = mem_bar_base(r, 0);
    *fb_size = *fb_base ? RAGE128_FB_BAR_SIZE : 0;
    *reg_base = mem_bar_base(r, 2);
    *reg_size = *reg_base ? RAGE128_REG_BAR_SIZE : 0;
    return true;
}

bool rage128_mem_access(Rage128 *r, bool write, uint64_t addr, unsigned len, uint64_t *val) {
    if (!valid_len(len) || !(command(r) & CMD_MEM)) {
        return false;
    }
    uint64_t rom_base, rom_size;
    if (rage128_rom_range(r, &rom_base, &rom_size) && addr >= rom_base &&
        addr < rom_base + rom_size) {
        /* Served straight out of the image: reads past it are 0xFF, writes
         * are dropped. */
        if (!write) {
            uint64_t out = 0;
            for (unsigned k = 0; k < len; k++) {
                const uint64_t o = addr - rom_base + k;
                const uint8_t byte = o < r->rom_len ? r->rom[o] : 0xFFu;
                out |= (uint64_t)byte << (8 * k);
            }
            *val = out;
        }
        return true;
    }
    const uint64_t fb = mem_bar_base(r, 0), reg = mem_bar_base(r, 2);
    if (fb && addr >= fb && addr < fb + RAGE128_FB_BAR_SIZE) {
        fb_bar(r, write, (uint32_t)(addr - fb), len, val);
        return true;
    }
    if (reg && addr >= reg && addr < reg + RAGE128_REG_BAR_SIZE) {
        reg_bar(r, write, (uint32_t)(addr - reg), len, true, val);
        return true;
    }
    if (addr >= LEGACY_MEM_BASE && addr < LEGACY_MEM_BASE + LEGACY_MEM_SIZE) {
        legacy_mem(r, write, (uint32_t)(addr - LEGACY_MEM_BASE), len, val);
        return true;
    }
    return false;
}

bool rage128_io_access(Rage128 *r, bool write, uint16_t port, unsigned len, uint64_t *val) {
    if (!valid_len(len) || !(command(r) & CMD_IO)) {
        return false;
    }
    /* An I/O BAR's low two bits are type, not address (PCI 2.3 6.2.5.1). */
    const uint32_t io = get_le32(r->cfg + 0x14) & ~3u;
    if (io && port >= io && port < io + RAGE128_IO_BAR_SIZE) {
        reg_bar(r, write, port - io, len, false, val);
        return true;
    }
    if (port >= 0x3B0u && port <= 0x3DFu) {
        bool claimed;
        legacy_io(r, write, port, len, val, &claimed);
        return claimed;
    }
    return false;
}

/* ---- PCI configuration space (RRG 4.1) -------------------------------------- */

enum { CFG_ROM = 0x30 };

/* Power-on configuration space: the RRG defaults. Class 03/00/00
 * VGA-compatible (a card wired as the VGA device; the RRG's SUB_CLASS default
 * 80h depends on the VGA strap). */
static void cfg_poweron(Rage128 *r) {
    uint8_t *c = r->cfg;
    memset(c, 0, sizeof r->cfg);
    put_le16(c + 0x00, RAGE128_VENDOR_ID);
    put_le16(c + 0x02, r->device_id);
    c[0x04] = 0x80;  /* COMMAND */
    put_le16(c + 0x06, 0x02B0);  /* STATUS */
    c[0x08] = r->revision;
    c[0x0B] = 0x03;  /* class: display */
    c[0x10] = 0x08;  /* MEM_BASE: prefetchable memory */
    c[0x14] = 0x01;  /* IO_BASE: I/O space */
    c[0x3C] = 0xFF;  /* INTERRUPT_LINE */
    c[0x3D] = 0x01;  /* INTA# */
    c[0x3E] = 0x08;  /* MIN_GRANT */
    /* CAPABILITIES_ID 50h (AGP, next 5Ch), AGP_STATUS 54h, AGP_COMMAND 58h,
     * PMI_REGISTER 5Ch: RRG reset values, unlinked while CAPABILITIES_PTR (34h)
     * is zero. */
    static const uint32_t caps[4] = { 0x00105C02u, 0x1F000203u, 0x00000200u, 0x06010001u };
    for (unsigned i = 0; i < 4; i++) {
        put_le32(c + 0x50 + 4 * i, caps[i]);
    }
}

static unsigned bar_size(unsigned idx) {
    switch (idx) {
    case 0:
        return RAGE128_FB_BAR_SIZE;
    case 1:
        return RAGE128_IO_BAR_SIZE;
    case 2:
        return RAGE128_REG_BAR_SIZE;
    default:
        return 0;  /* unimplemented BAR reads as zero */
    }
}

/* BAR write: the size is found by writing all ones and reading back the mask
 * with the type bits (I/O flag, or memory type and prefetchable) restored;
 * any other value is an address with the bits below the size not settable. */
static void bar_write(Rage128 *r, unsigned idx, uint32_t val) {
    const uint32_t size = bar_size(idx);
    if (!size) {
        return;
    }
    uint8_t *p = &r->cfg[0x10 + idx * 4];
    const uint32_t type = get_le32(p) & (idx == 1 ? 0x3u : 0xFu);
    put_le32(p, (val & ~(size - 1u)) | type);
}

/* Expansion ROM base: bit 0 enables the decoder, bits 10:1 are reserved, the
 * rest is the base address, settable only above the 128 KiB window. With no
 * ROM the register is read-only zero. */
static void rom_write(Rage128 *r, uint32_t val) {
    if (!rom_present(r)) {
        return;
    }
    put_le32(r->cfg + CFG_ROM,
             (val & ~(RAGE128_ROM_BAR_SIZE - 1u) & 0xFFFFF800u) | (val & 1u));
}

/* Header bytes (00h-3Fh) a configuration write may change: Command, Cache Line
 * Size, Latency Timer, Interrupt Line. Status 15:11 and 8 are write-1-to-clear
 * (PCI 2.2 6.2.3). */
static void hdr_write(Rage128 *r, unsigned reg, unsigned len, uint32_t val) {
    for (unsigned i = 0; i < len; i++) {
        const unsigned b = reg + i;
        const uint8_t v = (uint8_t)(val >> (8 * i));
        if (b == 0x07) {
            r->cfg[b] = (uint8_t)(r->cfg[b] & ~(v & 0xF9));
        } else if (b == 0x04 || b == 0x05 || b == 0x0C || b == 0x0D || b == 0x3C) {
            r->cfg[b] = v;
        }
    }
}

/* Device-specific registers 40h-FFh: ADAPTER_ID_W (4Ch, write-only) loads
 * ADAPTER_ID (2Ch, read-only: subsystem vendor and device), which is how the
 * card's BIOS sets its board identity. The AGP and power-management
 * capability blocks (50h, 5Ch) read their reset values; the AGP_COMMAND and
 * PMCSR words are writable. Others read as stored and report once. */
static void cfg_device_write(Rage128 *r, unsigned reg, unsigned len, uint32_t val) {
    const unsigned dw = reg & ~3u, sh = 8 * (reg & 3u);
    const uint32_t m = (len >= 4 ? 0xFFFFFFFFu : ((1u << (8 * len)) - 1)) << sh;
    const uint32_t nv = (get_le32(r->cfg + dw) & ~m) | ((val << sh) & m);
    switch (dw) {
    case 0x4C:  /* ADAPTER_ID_W */
        put_le32(r->cfg + 0x2C, nv);
        break;
    case 0x58:  /* AGP_COMMAND */
    case 0x60:  /* PMCSR */
        put_le32(r->cfg + dw, nv);
        break;
    default: {
        char key[24];
        snprintf(key, sizeof key, "cfg-%02x", dw);
        r128_unimpl(key, "config register 0x%02X written (0x%08X): not modelled, dropped", dw, nv);
        break;
    }
    }
}

uint32_t rage128_cfg_read(const Rage128 *r, unsigned reg, unsigned len) {
    const uint32_t all_ones = len >= 4 ? 0xFFFFFFFFu : (1u << (8 * len)) - 1;
    if (!len || len > 4 || reg + len > sizeof r->cfg) {
        return all_ones;
    }
    uint32_t v = 0;
    for (unsigned i = 0; i < len; i++) {
        v |= (uint32_t)r->cfg[reg + i] << (8 * i);
    }
    return v;
}

/* BARs and the ROM base take byte enables like any configuration register
 * (PCI 2.3 3.2.2.3.2): the written bytes are merged into the register, then its
 * writable-bit rules apply to the whole dword. */
void rage128_cfg_write(Rage128 *r, unsigned reg, unsigned len, uint32_t val) {
    if (!len || len > 4 || reg + len > sizeof r->cfg) {
        return;
    }
    if (reg >= 0x40) {
        if ((reg & 3u) + len <= 4) {
            cfg_device_write(r, reg, len, val);
        }
        return;
    }
    if ((reg >= 0x10 && reg < 0x28) || (reg >= CFG_ROM && reg < CFG_ROM + 4)) {
        const unsigned base = reg & ~3u;
        if (reg + len > base + 4) {
            return;  /* straddles two registers */
        }
        uint32_t cur = get_le32(r->cfg + base);
        for (unsigned i = 0; i < len; i++) {
            const unsigned sh = 8u * (reg - base + i);
            cur = (cur & ~(0xFFu << sh)) | (((val >> (8 * i)) & 0xFFu) << sh);
        }
        if (base == CFG_ROM) {
            rom_write(r, cur);
        } else {
            bar_write(r, (base - 0x10) / 4u, cur);
        }
        return;
    }
    hdr_write(r, reg, len, val);
}

/* ---- lifecycle -------------------------------------------------------------- */

void rage128_init(Rage128 *r, Vga *vga, const Rage128Wiring *wiring) {
    memset(r, 0, sizeof *r);
    r->device_id = RAGE128_DEVICE_ID;
    rage128_rebind(r, vga, wiring);
    reset_regs(r);
    cfg_poweron(r);
}

void rage128_rebind(Rage128 *r, Vga *vga, const Rage128Wiring *wiring) {
    r->vga = vga;
    r->wiring = *wiring;
}

void rage128_set_rom(Rage128 *r, const uint8_t *rom, uint32_t len) {
    r->rom = rom;
    r->rom_len = len < RAGE128_ROM_BAR_SIZE ? len : RAGE128_ROM_BAR_SIZE;
}

void rage128_reset(Rage128 *r) {
    cfg_poweron(r);
    if (r->irq_out) {
        r->irq_out = false;
        r->wiring.irq(r->wiring.opaque, false);
    }
    reset_regs(r);
    memset(r->palette, 0, sizeof r->palette);
    r->vblank_save_clear = r->int_vblank_clear = r->int_vline_clear = 0;
    vga_reset(r->vga);
}

/* ---- scan-out ----------------------------------------------------------------
 * RRG 6.1 CRTC registers: width = (CRTC_H_DISP + 1) * 8, height =
 * CRTC_V_DISP + 1, pitch = CRTC_PITCH * 8 pixels, start = CRTC_OFFSET bytes,
 * depth = CRTC_PIX_WIDTH (2 8 bpp, 3 15, 4 16, 5 24, 6 32). Displayed when
 * CRTC_EN and CRTC_EXT_DISP_EN are set and CRTC_DISPLAY_DIS (CRTC_EXT_CNTL bit
 * 10) is clear; CRTC_EXT_DISP_EN clear is the VGA core's picture. */
bool rage128_scanout(Rage128 *r, uint32_t *fb, uint32_t fb_w, uint32_t fb_h,
                     uint32_t *out_w, uint32_t *out_h, bool full, uint32_t *y0, uint32_t *y1) {
    *y0 = *y1 = 0;
    const uint32_t gen = REG(CRTC_GEN_CNTL), ext = REG(CRTC_EXT_CNTL);
    if (!(gen & (1u << 24))) {
        return false;  /* VGA display */
    }
    const uint32_t w = (((REG(CRTC_H_TOTAL_DISP) >> 16) & 0xFFu) + 1) * 8;
    const uint32_t h = ((REG(CRTC_V_TOTAL_DISP) >> 16) & 0x7FFu) + 1;
    const uint32_t pitch_px = (REG(CRTC_PITCH) & 0x3FFu) * 8;
    const uint32_t off = REG(CRTC_OFFSET) & 0x1FFFFFFu;
    const unsigned pw = (gen >> 8) & 7u;
    static const unsigned bytes[8] = { 0, 0, 1, 2, 2, 3, 4, 0 };
    const uint32_t dw = w < fb_w ? w : fb_w, dh = h < fb_h ? h : fb_h;
    *out_w = dw;
    *out_h = dh;
    if (!(gen & (1u << 25)) || (ext & (1u << 10)) || !bytes[pw]) {
        if (bytes[pw] == 0 && (gen & (1u << 25))) {
            r128_unimpl("pix-width", "scan-out pixel width %u", pw);
        }
        if (full) {
            for (uint32_t i = 0; i < fb_w * fb_h; i++) {
                fb[i] = 0xFF000000u;
            }
            *y1 = dh;
        }
        (void)vga_vram_take_any(r->vga);
        return true;
    }
    /* The whole picture is redrawn: cheap. */
    const uint32_t rowbytes = pitch_px * bytes[pw];
    for (uint32_t y = 0; y < dh; y++) {
        for (uint32_t x = 0; x < dw; x++) {
            const uint64_t a = (uint64_t)off + (uint64_t)y * rowbytes + (uint64_t)x * bytes[pw];
            uint32_t rgb = 0;
            if (a + bytes[pw] <= r128_vram_bytes(r)) {
                rgb = scan_rgb(r, x, y);
            }
            fb[y * fb_w + x] = 0xFF000000u | rgb;
        }
    }
    /* Hardware cursor: 64x64, 2 bits per pixel (RRG CRTC_CUR_MODE 0), 16 bytes
     * a line from CUR_OFFSET: the AND plane's 8 bytes, then the XOR plane's,
     * leftmost pixel in bit 7. AND 0: CUR_CLR0 / CUR_CLR1 by XOR; AND 1:
     * transparent / inverted. */
    if (gen & (1u << 16)) {
        const uint32_t cofs = REG(CUR_OFFSET) & 0x1FFFFF0u;
        const uint32_t pos = REG(CUR_HORZ_VERT_POSN), curoff = REG(CUR_HORZ_VERT_OFF);
        const uint32_t vpos = pos & 0x7FFu, hpos = (pos >> 16) & 0x7FFu;
        const uint32_t voff = curoff & 0x3Fu, hoff = (curoff >> 16) & 0x3Fu;
        const uint32_t clr[2] = { REG(CUR_CLR0) & 0xFFFFFFu, REG(CUR_CLR1) & 0xFFFFFFu };
        const uint8_t *vram = r->vga->vram;
        for (uint32_t row = 0; row < 64 - voff; row++) {
            const uint32_t y = vpos + row;
            if (y >= dh) {
                break;
            }
            /* No row skip for CUR_VERT_OFF here: drivers move CUR_OFFSET by the
             * offset's 16-byte lines themselves. */
            const uint32_t line = cofs + row * 16;
            if (line + 16 > r128_vram_bytes(r)) {
                break;
            }
            for (uint32_t c = hoff; c < 64; c++) {
                const uint32_t x = hpos + (c - hoff);
                if (x >= dw) {
                    break;
                }
                const bool a = (vram[line + c / 8] >> (7 - c % 8)) & 1u;
                const bool xr = (vram[line + 8 + c / 8] >> (7 - c % 8)) & 1u;
                uint32_t *o = &fb[y * fb_w + x];
                if (!a) {
                    *o = 0xFF000000u | clr[xr];
                } else if (xr) {
                    *o ^= 0x00FFFFFFu;
                }
            }
        }
    }
    (void)vga_vram_take_any(r->vga);
    *y1 = dh;
    return true;
}
