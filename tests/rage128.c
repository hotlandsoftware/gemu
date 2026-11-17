/* Standalone Rage 128 + VGA core model: no CPU, machine, ROM file or host UI
 * is linked. Run: python3 tools/ia64/run_rage128_test.py */
#include "rage128.h"
#include "vga.h"
#include "vgafont16.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

enum {
    FB_BASE = 0xE0000000u, REG_BASE = 0xE8000000u, ROM_BASE = 0xF0000000u,
    IO_BASE = 0x2000u,
    PIXEL_HZ = 29500000u,   /* with the PLL below the fake clock counts pixels */
};

/* Registers (offsets in the register aperture). */
enum {
    MM_INDEX = 0x000, MM_DATA = 0x004, CLOCK_CNTL_INDEX = 0x008,
    CLOCK_CNTL_DATA = 0x00C, BIOS_0_SCRATCH = 0x010, GEN_INT_CNTL = 0x040,
    GEN_INT_STATUS = 0x044, CRTC_GEN_CNTL = 0x050, DAC_CNTL = 0x058,
    CRTC_STATUS = 0x05C, PALETTE_INDEX = 0x0B0, PALETTE_DATA = 0x0B4,
    CONFIG_CNTL = 0x0E0, CRTC_H_TOTAL_DISP = 0x200, CRTC_V_TOTAL_DISP = 0x208,
    CRTC_VLINE_CRNT_VLINE = 0x210, CRTC_OFFSET = 0x224, CRTC_PITCH = 0x22C,
    OVR_CLR = 0x230, CUR_OFFSET = 0x260, CUR_HORZ_VERT_POSN = 0x264,
    CUR_CLR0 = 0x26C, CUR_CLR1 = 0x270,
    DST_PITCH_OFFSET = 0x142C, DST_Y_X = 0x1438, DP_GUI_MASTER_CNTL = 0x146C,
    DP_BRUSH_FRGD_CLR = 0x147C, DST_WIDTH_HEIGHT = 0x1598,
    DP_WRITE_MSK = 0x16CC, DEFAULT_OFFSET = 0x16E0, DEFAULT_PITCH = 0x16E4,
    DEFAULT_SC_BOTTOM_RIGHT = 0x16E8, GUI_STAT = 0x1740,
};

typedef struct {
    uint64_t ticks;
    bool irq;
    unsigned irq_edges;
} Env;

static Env env;
static Vga vga;
static Rage128 card;
static uint8_t vram[RAGE128_VRAM_SIZE];
static uint8_t rom[0x10000];
static uint32_t frame[640 * 480];

static uint64_t clock_read(void *opaque) {
    return ((Env *)opaque)->ticks;
}

static void irq_set(void *opaque, bool level) {
    Env *e = opaque;
    e->irq = level;
    e->irq_edges++;
}

static const Rage128Wiring wiring = {
    .clock = clock_read, .clock_hz = PIXEL_HZ, .irq = irq_set, .opaque = &env,
};

static uint32_t cfg_r(unsigned reg, unsigned len) {
    return rage128_cfg_read(&card, reg, len);
}

static void cfg_w(unsigned reg, unsigned len, uint32_t val) {
    rage128_cfg_write(&card, reg, len, val);
}

static uint64_t mem_r(uint64_t addr, unsigned len) {
    uint64_t v = 0xDEADDEADDEADDEADull;
    CHECK(rage128_mem_access(&card, false, addr, len, &v));
    return v;
}

static void mem_w(uint64_t addr, unsigned len, uint64_t v) {
    CHECK(rage128_mem_access(&card, true, addr, len, &v));
}

static uint32_t reg_r(uint32_t off) {
    return (uint32_t)mem_r(REG_BASE + off, 4);
}

static void reg_w(uint32_t off, uint32_t v) {
    mem_w(REG_BASE + off, 4, v);
}

static uint64_t io_r(uint16_t port, unsigned len) {
    uint64_t v = 0xDEADDEADDEADDEADull;
    CHECK(rage128_io_access(&card, false, port, len, &v));
    return v;
}

static void io_w(uint16_t port, unsigned len, uint64_t v) {
    CHECK(rage128_io_access(&card, true, port, len, &v));
}

static bool io_claimed(uint16_t port) {
    uint64_t v = 0;
    return rage128_io_access(&card, false, port, 1, &v);
}

static void seq_w(unsigned idx, unsigned v) {
    io_w(0x3C4, 2, v << 8 | idx);
}

static void gc_w(unsigned idx, unsigned v) {
    io_w(0x3CE, 2, v << 8 | idx);
}

static void crtc_w(unsigned idx, unsigned v) {
    io_w(0x3D4, 2, v << 8 | idx);
}

static void pll_w(unsigned idx, uint32_t v) {
    reg_w(CLOCK_CNTL_INDEX, 0x80u | idx);
    reg_w(CLOCK_CNTL_DATA, v);
}

static uint32_t pll_r(unsigned idx) {
    reg_w(CLOCK_CNTL_INDEX, idx);
    return reg_r(CLOCK_CNTL_DATA);
}

static void power_on(void) {
    memset(&env, 0, sizeof env);
    vga_init(&vga, vram, sizeof vram);
    rage128_init(&card, &vga, &wiring);
}

/* Place the BARs and enable decode. */
static void map_card(void) {
    cfg_w(0x10, 4, FB_BASE);
    cfg_w(0x14, 4, IO_BASE);
    cfg_w(0x18, 4, REG_BASE);
    cfg_w(0x04, 2, 0x0003);
}

static void test_config(void) {
    power_on();
    CHECK(cfg_r(0x00, 4) == 0x52451002u);
    CHECK(cfg_r(0x08, 4) == 0x03000000u);          /* class 03/00/00, revision 0 */
    CHECK(cfg_r(0x04, 2) == 0x0080u);
    CHECK(cfg_r(0x06, 2) == 0x02B0u);
    CHECK(cfg_r(0x0E, 1) == 0x00u);
    CHECK(cfg_r(0x10, 4) == 0x00000008u);
    CHECK(cfg_r(0x14, 4) == 0x00000001u);
    CHECK(cfg_r(0x18, 4) == 0x00000000u);
    CHECK(cfg_r(0x30, 4) == 0x00000000u);
    CHECK(cfg_r(0x34, 1) == 0x00u);
    CHECK(cfg_r(0x3C, 4) == 0x000801FFu);          /* line FF, INTA#, min grant 8 */
    CHECK(cfg_r(0x50, 4) == 0x00105C02u);
    CHECK(cfg_r(0x54, 4) == 0x1F000203u);
    CHECK(cfg_r(0x58, 4) == 0x00000200u);
    CHECK(cfg_r(0x5C, 4) == 0x06010001u);
    CHECK(cfg_r(0xFE, 4) == 0xFFFFFFFFu);          /* beyond configuration space */
    CHECK(cfg_r(0x02, 2) == 0x5245u);
    CHECK(cfg_r(0x03, 1) == 0x52u);

    /* Hardwired identity, writable Command / Interrupt Line, write-1-to-clear
     * Status. */
    cfg_w(0x00, 4, 0);
    cfg_w(0x08, 4, 0xFFFFFFFFu);
    CHECK(cfg_r(0x00, 4) == 0x52451002u);
    CHECK(cfg_r(0x08, 4) == 0x03000000u);
    cfg_w(0x04, 2, 0x0147);
    CHECK(cfg_r(0x04, 2) == 0x0147u);
    cfg_w(0x3C, 1, 0x0B);
    CHECK(cfg_r(0x3C, 1) == 0x0Bu);
    cfg_w(0x06, 2, 0xFFFF);
    CHECK(cfg_r(0x06, 2) == 0x02B0u);
    cfg_w(0x04, 2, 0x0000);

    /* ADAPTER_ID_W loads the read-only ADAPTER_ID; AGP_COMMAND is writable. */
    cfg_w(0x4C, 4, 0x12345678u);
    CHECK(cfg_r(0x2C, 4) == 0x12345678u);
    CHECK(cfg_r(0x4C, 4) == 0);
    cfg_w(0x2C, 4, 0);
    CHECK(cfg_r(0x2C, 4) == 0x12345678u);
    cfg_w(0x58, 4, 0x00000105u);
    CHECK(cfg_r(0x58, 4) == 0x00000105u);
    CHECK(cfg_r(0x59, 1) == 0x01u);
}

static void test_bars(void) {
    power_on();
    /* Sizing: write all ones, read the mask with the type bits. */
    cfg_w(0x10, 4, 0xFFFFFFFFu);
    CHECK(cfg_r(0x10, 4) == 0xFC000008u);           /* 64 MiB prefetchable */
    cfg_w(0x14, 4, 0xFFFFFFFFu);
    CHECK(cfg_r(0x14, 4) == 0xFFFFFF01u);           /* 256 bytes of I/O */
    cfg_w(0x18, 4, 0xFFFFFFFFu);
    CHECK(cfg_r(0x18, 4) == 0xFFFFC000u);           /* 16 KiB */
    cfg_w(0x1C, 4, 0xFFFFFFFFu);
    CHECK(cfg_r(0x1C, 4) == 0);                     /* BAR3 unimplemented */
    cfg_w(0x24, 4, 0xFFFFFFFFu);
    CHECK(cfg_r(0x24, 4) == 0);
    cfg_w(0x30, 4, 0xFFFFFFFFu);
    CHECK(cfg_r(0x30, 4) == 0);                     /* no ROM: read-only zero */

    /* Addresses lose the bits below the size. */
    cfg_w(0x10, 4, 0xE0001234u);
    CHECK(cfg_r(0x10, 4) == 0xE0000008u);
    cfg_w(0x14, 4, 0x00002345u);
    CHECK(cfg_r(0x14, 4) == 0x00002301u);
    cfg_w(0x18, 4, 0xE4003FFFu);
    CHECK(cfg_r(0x18, 4) == 0xE4000000u);
    cfg_w(0x1B, 1, 0xD0);                           /* byte enables merge */
    CHECK(cfg_r(0x18, 4) == 0xD0000000u);

    /* Nothing decodes until COMMAND enables it. */
    uint64_t v = 0, fb, fbs, rg, rgs;
    CHECK(!rage128_mem_access(&card, false, 0xE0000000u, 4, &v));
    CHECK(!rage128_io_access(&card, false, 0x2300, 4, &v));
    CHECK(!rage128_mem_ranges(&card, &fb, &fbs, &rg, &rgs));
    map_card();
    CHECK(rage128_mem_ranges(&card, &fb, &fbs, &rg, &rgs));
    CHECK(fb == FB_BASE && fbs == 0x4000000u);
    CHECK(rg == REG_BASE && rgs == 0x4000u);
    CHECK(rage128_mem_access(&card, false, FB_BASE + 0x7FFFFC, 4, &v));
    CHECK(!rage128_mem_access(&card, false, FB_BASE + 0x4000000, 4, &v));
    CHECK(!rage128_mem_access(&card, false, REG_BASE + 0x4000, 4, &v));
    CHECK(!rage128_mem_access(&card, false, 0x1000, 4, &v));
    CHECK(!rage128_mem_access(&card, false, FB_BASE, 3, &v));

    /* Memory decode off leaves I/O; I/O decode off leaves memory. */
    cfg_w(0x04, 2, 0x0001);
    CHECK(!rage128_mem_access(&card, false, REG_BASE, 4, &v));
    CHECK(!rage128_mem_ranges(&card, &fb, &fbs, &rg, &rgs));
    CHECK(rage128_io_access(&card, false, 0x2010, 4, &v));
    cfg_w(0x04, 2, 0x0002);
    CHECK(!rage128_io_access(&card, false, 0x2010, 4, &v));
    CHECK(!rage128_io_access(&card, false, 0x3DA, 1, &v));
    CHECK(rage128_mem_access(&card, false, REG_BASE, 4, &v));

    /* A BAR left at zero is unplaced. */
    cfg_w(0x10, 4, 0);
    CHECK(rage128_mem_ranges(&card, &fb, &fbs, &rg, &rgs));
    CHECK(fbs == 0 && rgs == 0x4000u);
    CHECK(!rage128_mem_access(&card, false, 0, 4, &v));
}

static void test_rom(void) {
    power_on();
    for (unsigned i = 0; i < sizeof rom; i++) {
        rom[i] = (uint8_t)(i * 7 + 1);
    }
    rage128_set_rom(&card, rom, sizeof rom);
    cfg_w(0x30, 4, 0xFFFFFFFFu);
    CHECK(cfg_r(0x30, 4) == 0xFFFE0001u);           /* 128 KiB window, enable bit */
    cfg_w(0x30, 4, ROM_BASE);
    CHECK(cfg_r(0x30, 4) == ROM_BASE);
    cfg_w(0x04, 2, 0x0002);

    uint64_t base, size, v;
    CHECK(!rage128_rom_range(&card, &base, &size)); /* base set but not enabled */
    CHECK(!rage128_mem_access(&card, false, ROM_BASE, 4, &v));
    cfg_w(0x30, 1, 0x01);                           /* a one-byte enable, as firmware does */
    CHECK(cfg_r(0x30, 4) == (ROM_BASE | 1u));
    CHECK(rage128_rom_range(&card, &base, &size));
    CHECK(base == ROM_BASE && size == 0x20000u);
    CHECK(mem_r(ROM_BASE, 1) == rom[0]);
    CHECK(mem_r(ROM_BASE + 2, 2) == (uint64_t)(rom[2] | rom[3] << 8));
    CHECK(mem_r(ROM_BASE + 0x100, 4) == (uint64_t)(rom[0x100] | rom[0x101] << 8 |
                                                  rom[0x102] << 16 | (uint32_t)rom[0x103] << 24));
    CHECK(mem_r(ROM_BASE + 0xFFFC, 8) == (uint64_t)(rom[0xFFFC] | rom[0xFFFD] << 8 |
                                                   rom[0xFFFE] << 16 | (uint32_t)rom[0xFFFF] << 24 |
                                                   0xFFFFFFFF00000000ull));
    CHECK(mem_r(ROM_BASE + 0x10000, 4) == 0xFFFFFFFFu);   /* past the image */
    CHECK(mem_r(ROM_BASE + 0x1FFFF, 1) == 0xFF);
    mem_w(ROM_BASE, 4, 0);                           /* ROM is not writable */
    CHECK(mem_r(ROM_BASE, 1) == rom[0]);
    CHECK(!rage128_mem_access(&card, false, ROM_BASE + 0x20000, 4, &v));

    cfg_w(0x04, 2, 0x0000);                          /* memory decode off */
    CHECK(!rage128_rom_range(&card, &base, &size));
    CHECK(!rage128_mem_access(&card, false, ROM_BASE, 4, &v));
    cfg_w(0x04, 2, 0x0002);
    cfg_w(0x30, 4, ROM_BASE);                        /* disabled again */
    CHECK(!rage128_mem_access(&card, false, ROM_BASE, 4, &v));
}

static void test_registers(void) {
    power_on();
    map_card();
    uint64_t v;

    /* MM_INDEX / MM_DATA reach registers; the second aperture and the I/O BAR
     * show the same file. */
    reg_w(MM_INDEX, BIOS_0_SCRATCH);
    reg_w(MM_DATA, 0xCAFEBABEu);
    CHECK(reg_r(BIOS_0_SCRATCH) == 0xCAFEBABEu);
    CHECK(reg_r(MM_DATA) == 0xCAFEBABEu);
    CHECK(reg_r(0x2000 + BIOS_0_SCRATCH) == 0xCAFEBABEu);
    CHECK(io_r(IO_BASE + BIOS_0_SCRATCH, 4) == 0xCAFEBABEu);
    io_w(IO_BASE + BIOS_0_SCRATCH, 4, 0x01020304u);
    CHECK(reg_r(BIOS_0_SCRATCH) == 0x01020304u);
    CHECK(mem_r(REG_BASE + BIOS_0_SCRATCH + 1, 1) == 0x03);
    CHECK(mem_r(REG_BASE + BIOS_0_SCRATCH + 2, 2) == 0x0102);
    mem_w(REG_BASE + BIOS_0_SCRATCH + 3, 1, 0xAA);
    CHECK(reg_r(BIOS_0_SCRATCH) == 0xAA020304u);
    mem_w(REG_BASE + BIOS_0_SCRATCH, 8, 0x1111111122222222ull);
    CHECK(reg_r(BIOS_0_SCRATCH) == 0x22222222u);
    CHECK(reg_r(BIOS_0_SCRATCH + 4) == 0x11111111u);

    /* RRG reset values and the read-only copy of configuration space. */
    CHECK(reg_r(0x030) == 0x880F4F41u);             /* BUS_CNTL */
    CHECK(reg_r(CRTC_GEN_CNTL) == 0x04000000u);
    CHECK(reg_r(0xF00) == 0x52451002u);
    reg_w(0xF00, 0);
    CHECK(reg_r(0xF00) == 0x52451002u);
    CHECK(reg_r(0x100) == FB_BASE);                 /* CONFIG_APER_0_BASE */
    CHECK(reg_r(0x104) == (FB_BASE | 0x02000000u));
    CHECK(reg_r(0x108) == 0x02000000u);
    CHECK(reg_r(0x10C) == (REG_BASE | 0x2000u));
    CHECK(reg_r(0x110) == 0x2000u);

    /* MM_INDEX bit 31 reaches the frame buffer. */
    reg_w(MM_INDEX, 0x80000100u);
    reg_w(MM_DATA, 0x11223344u);
    CHECK(vram[0x100] == 0x44 && vram[0x103] == 0x11);
    CHECK(reg_r(MM_DATA) == 0x11223344u);

    /* Block I/O is only the first 256 bytes of the register file. */
    CHECK(rage128_io_access(&card, false, IO_BASE + 0xF0, 4, &v));
    CHECK(!rage128_io_access(&card, false, IO_BASE + 0x100, 4, &v));

    /* PLL registers through CLOCK_CNTL_INDEX/DATA, writes gated by PLL_WR_EN. */
    CHECK(pll_r(0x02) == 0x00004C03u);              /* PPLL_CNTL reset */
    pll_w(0x04, 0x00020123u);
    CHECK(pll_r(0x04) == 0x00020123u);
    pll_w(0x04, 0x0003FFFFu | 0x8000u);
    CHECK(pll_r(0x04) == 0x00037FFFu);              /* update-in-progress reads clear */
    reg_w(CLOCK_CNTL_INDEX, 0x04);                  /* PLL_WR_EN clear */
    reg_w(CLOCK_CNTL_DATA, 0);
    CHECK(reg_r(CLOCK_CNTL_DATA) == 0x00037FFFu);
    pll_w(0x0F, 0xFFFFFFFFu);
    CHECK(pll_r(0x0F) == 0xFFFFFFFFu);
    CHECK(pll_r(0x04) == 0x00037FFFu);
}

/* Identity attribute palette, video enabled. */
static void attr_palette_identity(void) {
    (void)io_r(0x3DA, 1);                           /* reset the attribute flip-flop */
    for (unsigned i = 0; i < 16; i++) {
        io_w(0x3C0, 1, i);
        io_w(0x3C0, 1, i);
    }
    io_w(0x3C0, 1, 0x20);                           /* Palette Address Source: video on */
}

/* A VGA text mode, as a BIOS programs it: 80x25 of 8x16 cells at B8000. */
static void program_text_mode(void) {
    io_w(0x3C2, 1, 0x67);
    seq_w(1, 0x01);                                 /* 8-dot cells */
    seq_w(2, 0x03);                                 /* map mask: planes 0 and 1 */
    seq_w(4, 0x03);
    gc_w(5, 0x10);                                  /* odd/even */
    gc_w(6, 0x0E);                                  /* text, B8000 */
    crtc_w(0x01, 79);
    crtc_w(0x09, 15);
    crtc_w(0x12, 0x8F);                             /* 400 lines: 0x18F */
    crtc_w(0x07, 0x02);                             /* its bit 8 */
    crtc_w(0x0C, 0x00);
    crtc_w(0x0D, 0x00);
    crtc_w(0x0A, 0x20);                             /* cursor off */
    attr_palette_identity();
    io_w(0x3C8, 1, 0);
    for (unsigned i = 0; i < 16; i++) {
        io_w(0x3C9, 1, i == 15 ? 0x3F : 0);
        io_w(0x3C9, 1, i == 15 ? 0x3F : 0);
        io_w(0x3C9, 1, i == 1 ? 0x2A : i == 15 ? 0x3F : 0);
    }
}

static void test_vga_ports(void) {
    power_on();
    map_card();

    /* Colour I/O range is selected at power-on; Misc Output reads back at 3CC. */
    CHECK(io_r(0x3CC, 1) == 0x01);
    CHECK(io_claimed(0x3D4) && !io_claimed(0x3B4));
    io_w(0x3C2, 1, 0x67);
    CHECK(io_r(0x3CC, 1) == 0x67);
    io_w(0x3C2, 1, 0x66);                           /* mono */
    CHECK(io_claimed(0x3B4) && !io_claimed(0x3D4));
    CHECK(!io_claimed(0x3DA) && io_claimed(0x3BA));
    io_w(0x3B4, 2, (0x5A << 8) | 0x0E);             /* the 3B4 pair is the CRTC */
    io_w(0x3C2, 1, 0x67);
    io_w(0x3D4, 1, 0x0E);
    CHECK(io_r(0x3D5, 1) == 0x5A);
    CHECK(io_r(0x3D4, 1) == 0x0E);
    CHECK(!io_claimed(0x3B5));
    CHECK(!io_claimed(0x3B0));                      /* no register there */

    /* A word write to an index port programs the data register too. */
    io_w(0x3C4, 2, 0x0502);
    CHECK(io_r(0x3C4, 1) == 0x02);
    CHECK(io_r(0x3C5, 1) == 0x05);
    io_w(0x3C5, 1, 0x0A);
    CHECK(vga.seq[2] == 0x0A);
    io_w(0x3CE, 2, 0x0D06);
    io_w(0x3CE, 1, 0x06);
    CHECK(io_r(0x3CF, 1) == 0x0D);
    io_w(0x3CF, 1, 0x0E);
    CHECK(vga.gc[6] == 0x0E && io_r(0x3CE, 1) == 0x06);
    io_w(0x3D4, 2, 0x4B0C);
    io_w(0x3D4, 1, 0x0C);
    CHECK(io_r(0x3D5, 1) == 0x4B);
    io_w(0x3D5, 1, 0x4C);
    CHECK(vga.crtc[0x0C] == 0x4C);

    /* Attribute controller: 3C0 alternates index and data; 3DA resets it. */
    (void)io_r(0x3DA, 1);
    io_w(0x3C0, 1, 0x03);
    io_w(0x3C0, 1, 0x2B);
    CHECK(vga.attr[3] == 0x2B);
    io_w(0x3C0, 1, 0x05);                           /* index again */
    CHECK(io_r(0x3C0, 1) == 0x05);
    CHECK(io_r(0x3C1, 1) == vga.attr[5]);
    io_w(0x3C0, 1, 0x06);                           /* now data for index 5 */
    CHECK(vga.attr[5] == 0x06);
    io_w(0x3C0, 1, 0x07);                           /* an index left pending... */
    (void)io_r(0x3DA, 1);                           /* ...is abandoned by a 3DA access */
    io_w(0x3C0, 1, 0x09);
    CHECK(io_r(0x3C0, 1) == 0x09);

    /* DAC: separate read and write addresses, 6-bit components, read state. */
    CHECK(io_r(0x3C6, 1) == 0xFF);
    io_w(0x3C8, 1, 0x10);
    CHECK(io_r(0x3C7, 1) == 0x00);
    io_w(0x3C9, 1, 0x3F);
    io_w(0x3C9, 1, 0x20);
    CHECK(io_r(0x3C8, 1) == 0x10);
    io_w(0x3C9, 1, 0x41);
    CHECK(io_r(0x3C8, 1) == 0x11);
    io_w(0x3C7, 1, 0x10);
    CHECK(io_r(0x3C7, 1) == 0x03);
    CHECK(io_r(0x3C9, 1) == 0x3F);
    CHECK(io_r(0x3C9, 1) == 0x20);
    CHECK(io_r(0x3C9, 1) == 0x01);                  /* bit 6 is not stored */
    io_w(0x3C7, 1, 0x11);
    CHECK(io_r(0x3C9, 1) == 0x00 && io_r(0x3C9, 1) == 0x00 && io_r(0x3C9, 1) == 0x00);
    io_w(0x3C8, 1, 0x00);
    CHECK(io_r(0x3C7, 1) == 0x00);
    io_w(0x3C6, 1, 0x0F);
    CHECK(io_r(0x3C6, 1) == 0x0F);

    /* 3C3 is GENENB: BLK_IO_BASE, the I/O BAR's bits 15:8; read-only. */
    CHECK(io_r(0x3C3, 1) == 0x20);
    io_w(0x3C3, 1, 0x55);
    CHECK(io_r(0x3C3, 1) == 0x20);

    /* 3C2 reads GENS0: SENSE_SWITCH mirrors DAC_CMP_OUTPUT, which needs
     * DAC_COMP_EN and a dark colour on every channel. */
    CHECK(io_r(0x3C2, 1) == 0x00);
    reg_w(DAC_CNTL, reg_r(DAC_CNTL) | 0x08);
    reg_w(OVR_CLR, 0);
    CHECK(io_r(0x3C2, 1) == 0x10);
    reg_w(OVR_CLR, 0x000000FF);
    CHECK(io_r(0x3C2, 1) == 0x00);
    reg_w(OVR_CLR, 0x00101010);
    CHECK(io_r(0x3C2, 1) == 0x10);
    CHECK(io_r(0x3CC, 1) == 0x67);                  /* misc output unaffected */

    /* Input status 1 follows the fake clock over a 60 Hz frame. */
    env.ticks = 0;
    CHECK(io_r(0x3DA, 1) == 0x00);
    env.ticks = PIXEL_HZ / 60 * 93 / 100;
    CHECK(io_r(0x3DA, 1) == 0x01);                  /* display disabled */
    env.ticks = PIXEL_HZ / 60 * 99 / 100;
    CHECK(io_r(0x3DA, 1) == 0x09);                  /* and retrace */
    env.ticks = PIXEL_HZ / 60 + 1000;
    CHECK(io_r(0x3DA, 1) == 0x00);
}

/* VGA CRTC display-end writes reach the extended CRTC: the video BIOS's
 * set-mode path writes CR12 = V_DISP - 1 and only leaves the extended display
 * (CRTC_EXT_DISP_EN) when CRTC_V_TOTAL_DISP follows. */
static void test_crtc_shadow(void) {
    power_on();
    map_card();
    reg_w(CRTC_H_TOTAL_DISP, (99u << 16) | 131u);   /* 800 of 1056 */
    reg_w(CRTC_V_TOTAL_DISP, (599u << 16) | 627u);  /* 600 of 628 */

    /* The ROM's probe, through MM_INDEX/MM_DATA at the I/O BAR. */
    io_w(IO_BASE + 0x00, 4, CRTC_V_TOTAL_DISP + 2);
    const unsigned vdisp = (unsigned)io_r(IO_BASE + 0x06, 1);
    CHECK(vdisp == (599u & 0xFFu));
    crtc_w(0x12, vdisp - 1);
    CHECK(io_r(IO_BASE + 0x06, 1) == vdisp - 1);

    /* Mode 12h's display end: 640 x 480 (CR07 bit 1 = V_DISP bit 8). */
    crtc_w(0x07, 0x3E);
    crtc_w(0x12, 0xDF);
    crtc_w(0x01, 0x4F);
    CHECK(reg_r(CRTC_V_TOTAL_DISP) == ((479u << 16) | 627u));
    CHECK(reg_r(CRTC_H_TOTAL_DISP) == ((79u << 16) | 131u));
    crtc_w(0x07, 0x40);                             /* bit 6 = V_DISP bit 9, bit 1 clear */
    CHECK(reg_r(CRTC_V_TOTAL_DISP) == ((0x2DFu << 16) | 627u));

    /* Index-only writes and the other CRTC registers leave it alone. */
    io_w(0x3D4, 1, 0x12);
    crtc_w(0x00, 0x5F);
    crtc_w(0x06, 0x0B);
    CHECK(reg_r(CRTC_V_TOTAL_DISP) == ((0x2DFu << 16) | 627u));
    CHECK(reg_r(CRTC_H_TOTAL_DISP) == ((79u << 16) | 131u));
    io_w(0x3D4, 1, 0x12);
    io_w(0x3D5, 1, 0x57);                           /* byte write at the data port */
    CHECK(((reg_r(CRTC_V_TOTAL_DISP) >> 16) & 0xFFu) == 0x57u);
}

static void test_legacy_window(void) {
    power_on();
    map_card();
    program_text_mode();

    /* Odd/even text addressing: characters in plane 0, attributes in plane 1. */
    mem_w(0xB8000, 1, 'A');
    mem_w(0xB8001, 1, 0x1F);
    mem_w(0xB8002, 2, 0x1E42);                      /* 'B', attribute 1E */
    CHECK(vga.plane[0][0] == 'A' && vga.plane[1][0] == 0x1F);
    CHECK(vga.plane[0][1] == 'B' && vga.plane[1][1] == 0x1E);
    CHECK(mem_r(0xB8000, 1) == 'A');
    CHECK(mem_r(0xB8000, 4) == 0x1E421F41u);
    CHECK(mem_r(0xB8000, 8) == 0x1E421F41u);
    CHECK(mem_r(0xA0000, 1) == 0xFF);               /* outside the selected window */
    CHECK(mem_r(0xBFFFF, 1) == 0x00);
    uint64_t v;
    CHECK(!rage128_mem_access(&card, false, 0xC0000, 1, &v));
    CHECK(!rage128_mem_access(&card, false, 0x9FFFF, 1, &v));
    cfg_w(0x04, 2, 0x0001);                         /* memory decode off */
    CHECK(!rage128_mem_access(&card, false, 0xB8000, 1, &v));
    cfg_w(0x04, 2, 0x0003);

    /* The compositor draws the page with the ROM font and the DAC. */
    uint32_t w = 0, h = 0, y0, y1;
    CHECK(!vga_blanked(&vga) && vga_text_mode(&vga));
    unsigned cols, rows, cw, ch;
    vga_text_geometry(&vga, &cols, &rows, &cw, &ch);
    CHECK(cols == 80 && rows == 25 && cw == 8 && ch == 16);
    CHECK(vga_blit(&vga, frame, 640, 480, 640, 480, 1, &w, &h));
    CHECK(w == 640 && h == 400);
    for (unsigned y = 0; y < 16; y++) {
        for (unsigned x = 0; x < 8; x++) {
            const bool set = (vgafont16['A' * FONT_HEIGHT + y] >> (7 - x)) & 1;
            CHECK(frame[y * 640 + x] == (set ? 0xFFFFFFFFu : 0xFF0000AAu));
        }
    }
    for (unsigned y = 0; y < 16; y++) {             /* 'B', attribute 1E: black on blue */
        for (unsigned x = 0; x < 8; x++) {
            const bool set = (vgafont16['B' * FONT_HEIGHT + y] >> (7 - x)) & 1;
            CHECK(frame[y * 640 + 8 + x] == (set ? 0xFF000000u : 0xFF0000AAu));
        }
    }
    CHECK(frame[479 * 640] == 0xFF000000u);
    CHECK(vga_blit_ex(&vga, frame, 640, 480, 640, 480, 1, &w, &h, true, &y0, &y1));
    CHECK(y0 == 0 && y1 == 480);

    /* Clearing the Palette Address Source blanks the screen. */
    (void)io_r(0x3DA, 1);
    io_w(0x3C0, 1, 0x00);                           /* index 0, Palette Address Source clear */
    io_w(0x3C0, 1, 0x00);
    CHECK(vga.attr_idx == 0x00);
    gc_w(6, 0x00);
    CHECK(vga_blanked(&vga));
    CHECK(!vga_blit(&vga, frame, 640, 480, 640, 480, 1, &w, &h));
}

static void test_vga_planar(void) {
    power_on();
    map_card();
    seq_w(2, 0x0F);                                 /* map mask: all planes */
    gc_w(5, 0x00);                                  /* write mode 0 */
    gc_w(1, 0x0F);                                  /* set/reset enabled on all planes */
    gc_w(0, 0x0A);                                  /* set/reset colour 0xA */
    gc_w(8, 0xFF);                                  /* bit mask */
    gc_w(6, 0x05);                                  /* graphics, A0000 64 KiB */
    crtc_w(0x01, 79);
    crtc_w(0x12, 0xDF);                             /* 480 lines: 0x1DF */
    crtc_w(0x07, 0x02);
    attr_palette_identity();
    io_w(0x3C8, 1, 0x0A);
    io_w(0x3C9, 1, 0x10);
    io_w(0x3C9, 1, 0x20);
    io_w(0x3C9, 1, 0x30);

    uint32_t w, h, y0, y1;
    CHECK(vga_blit_ex(&vga, frame, 640, 480, 640, 480, 1, &w, &h, true, &y0, &y1));
    CHECK(!vga_planes_dirty(&vga));                 /* consumed */
    mem_w(0xA0000 + 80, 1, 0x00);                   /* row 1: 8 pixels of colour 0xA */
    CHECK(vga_planes_dirty(&vga));
    CHECK(vga.plane[1][80] == 0xFF && vga.plane[3][80] == 0xFF);
    CHECK(vga.plane[0][80] == 0x00 && vga.plane[2][80] == 0x00);
    CHECK(mem_r(0xA0000 + 80, 1) == 0x00);          /* plane 0 read */
    CHECK(vga_blit_ex(&vga, frame, 640, 480, 640, 480, 1, &w, &h, false, &y0, &y1));
    CHECK(w == 640 && h == 480);
    CHECK(y0 == 0 && y1 == 2);                      /* only the rows over the written chunk */
    for (unsigned x = 0; x < 8; x++) {
        CHECK(frame[640 + x] == 0xFF4182C3u);
    }
    CHECK(frame[640 + 8] == 0xFF000000u);

    /* Write mode 2 takes the byte as a colour under the bit mask. */
    gc_w(5, 0x02);
    gc_w(1, 0x00);
    gc_w(8, 0x0F);
    (void)mem_r(0xA0000 + 160, 1);                  /* load the latches */
    mem_w(0xA0000 + 160, 1, 0x05);
    CHECK(vga.plane[0][160] == 0x0F && vga.plane[1][160] == 0x00);
    CHECK(vga.plane[2][160] == 0x0F && vga.plane[3][160] == 0x00);
}

static void test_aperture(void) {
    power_on();
    map_card();
    memset(vram, 0, 0x10000);

    mem_w(FB_BASE + 0x1000, 4, 0x44332211u);
    CHECK(vram[0x1000] == 0x11 && vram[0x1001] == 0x22 &&
          vram[0x1002] == 0x33 && vram[0x1003] == 0x44);
    CHECK(mem_r(FB_BASE + 0x1000, 4) == 0x44332211u);
    CHECK(mem_r(FB_BASE + 0x1002, 2) == 0x4433);
    CHECK(mem_r(FB_BASE + 0x1003, 1) == 0x44);
    mem_w(FB_BASE + 0x4000, 8, 0x8877665544332211ull);
    CHECK(vram[0x4000] == 0x11 && vram[0x4007] == 0x88);
    CHECK(mem_r(FB_BASE + 0x4000, 8) == 0x8877665544332211ull);

    /* CONFIG_CNTL APER_0_ENDIAN (1:0) swaps within 16-bit (1) or 32-bit (2)
     * units; APER_1_ENDIAN (3:2) does the same for the second 32 MiB image,
     * which shows the same memory. */
    reg_w(CONFIG_CNTL, 0x2);
    mem_w(FB_BASE + 0x2000, 4, 0x44332211u);
    CHECK(vram[0x2000] == 0x44 && vram[0x2001] == 0x33 &&
          vram[0x2002] == 0x22 && vram[0x2003] == 0x11);
    CHECK(mem_r(FB_BASE + 0x2000, 4) == 0x44332211u);
    CHECK(mem_r(FB_BASE + 0x2000, 1) == 0x11);       /* byte 0 is memory byte 3 */
    CHECK(mem_r(FB_BASE + 0x2002, 2) == 0x4433);
    reg_w(CONFIG_CNTL, 0x1);
    mem_w(FB_BASE + 0x3000, 4, 0x44332211u);
    CHECK(vram[0x3000] == 0x22 && vram[0x3001] == 0x11 &&
          vram[0x3002] == 0x44 && vram[0x3003] == 0x33);
    CHECK(mem_r(FB_BASE + 0x3000, 4) == 0x44332211u);
    CHECK(mem_r(FB_BASE + 0x3001, 1) == 0x22);
    CHECK(mem_r(FB_BASE + 0x2000 + (32u << 20), 4) == 0x11223344u);   /* image 1: no swap */
    reg_w(CONFIG_CNTL, 0x8);                        /* APER_1_ENDIAN 32-bit */
    CHECK(mem_r(FB_BASE + 0x2000 + (32u << 20), 4) == 0x44332211u);
    mem_w(FB_BASE + 0x5000 + (32u << 20), 4, 0xA1B2C3D4u);
    CHECK(vram[0x5000] == 0xA1 && vram[0x5003] == 0xD4);
    reg_w(CONFIG_CNTL, 0);

    /* APER_REG_ENDIAN (bit 4) swaps dword register accesses in REG_BASE. */
    reg_w(BIOS_0_SCRATCH, 0x01020304u);
    reg_w(CONFIG_CNTL, 0x10);
    CHECK(mem_r(REG_BASE + BIOS_0_SCRATCH, 4) == 0x04030201u);
    mem_w(REG_BASE + BIOS_0_SCRATCH, 4, 0x0A0B0C0Du);
    reg_w(CONFIG_CNTL, 0);
    CHECK(reg_r(BIOS_0_SCRATCH) == 0x0D0C0B0Au);
    CHECK(vga_vram_take_any(&vga));
    CHECK(!vga_vram_take_any(&vga));
    mem_w(FB_BASE + 0x1000, 1, 1);
    CHECK(vga_vram_take_any(&vga));
}

/* 640x480 timings in units where the fake clock counts pixels: XTALIN * N /
 * (M * post) / PIXEL_HZ = 1 with M = N = 4 and no post divider. */
static void program_raster(void) {
    reg_w(CRTC_GEN_CNTL, (1u << 24) | (1u << 25)); /* extended CRTC */
    pll_w(0x03, 4);                                 /* PPLL_REF_DIV */
    pll_w(0x04, 4);                                 /* PLL_DIV_0: N = 4, post /1 */
    pll_w(0x08, 3);                                 /* VCLK_ECP_CNTL: VCLK from PPLL */
    reg_w(CRTC_H_TOTAL_DISP, (79u << 16) | 99u);    /* 640 of 800 */
    reg_w(CRTC_V_TOTAL_DISP, (479u << 16) | 524u);  /* 480 of 525 */
}

static void test_vga_raster(void) {
    power_on();
    map_card();
    /* XP 2002's ATI miniport probes the monitor in VGA text mode and
     * waits for scanline 402. Extended totals remain zero at this point. */
    reg_w(CRTC_GEN_CNTL, 1u << 25);
    io_w(0x3C2, 1, 0x67);                           /* VGA clock select 1 */
    seq_w(1, 0);                                   /* nine dots per character */
    crtc_w(0x00, 0x5F);                            /* 100 characters per line */
    crtc_w(0x01, 0x4F);                            /* 80 visible */
    crtc_w(0x06, 0xBF);
    crtc_w(0x07, 0x1F);                            /* 449 total, 400 visible */
    crtc_w(0x12, 0x8F);
    pll_w(0x03, 4);
    pll_w(0x05, 4);                                /* VGA PLL1: one pixel/tick */
    pll_w(0x07, 8);                                /* extended PLL3: two */
    pll_w(0x08, 3);
    reg_w(CLOCK_CNTL_INDEX, 3u << 8);
    reg_w(CRTC_VLINE_CRNT_VLINE, 37);
    const uint64_t line = 900, frame_ticks = line * 449;
    env.ticks = line * 402 + 10;
    CHECK(reg_r(CRTC_VLINE_CRNT_VLINE) == ((402u << 16) | 37u));
    CHECK((reg_r(CRTC_STATUS) & 1) == 1);
    env.ticks = frame_ticks + line * 12;
    CHECK(reg_r(CRTC_VLINE_CRNT_VLINE) == ((12u << 16) | 37u));
    CHECK((reg_r(CRTC_STATUS) & 1) == 0);
    reg_w(GEN_INT_STATUS, 3);
    reg_w(GEN_INT_CNTL, 1);
    env.ticks = frame_ticks + line * 400;
    rage128_tick(&card);
    CHECK(env.irq && (reg_r(GEN_INT_STATUS) & 1));
    reg_w(GEN_INT_STATUS, 1);
    CHECK(!env.irq);

    /* Eight-dot characters and the sequencer's divide-by-two clock. */
    seq_w(1, 1);
    env.ticks = 800u * 402u;
    CHECK((reg_r(CRTC_VLINE_CRNT_VLINE) >> 16) == 402);
    seq_w(1, 9);
    env.ticks *= 2;
    CHECK((reg_r(CRTC_VLINE_CRNT_VLINE) >> 16) == 402);

    /* Both overflow bits of the VGA vertical total are significant. */
    crtc_w(0x06, 0x58);
    crtc_w(0x07, 0x21);                            /* 858 scanlines */
    env.ticks = 1600u * 857u;
    CHECK((reg_r(CRTC_VLINE_CRNT_VLINE) >> 16) == 857);
    env.ticks += 1600;
    CHECK((reg_r(CRTC_VLINE_CRNT_VLINE) >> 16) == 0);
}

static void test_raster(void) {
    power_on();
    map_card();

    /* Without the PLL-driven raster there is no beam to read. */
    CHECK(reg_r(CRTC_STATUS) == 0);
    reg_w(CRTC_VLINE_CRNT_VLINE, 0x00000123u);
    CHECK(reg_r(CRTC_VLINE_CRNT_VLINE) == 0x123u);

    program_raster();
    const uint64_t frame_px = 800u * 525u, vblank = 800u * 480u;
    env.ticks = 0;
    CHECK(reg_r(CRTC_STATUS) == 0);
    env.ticks = 800u * 100u + 5;
    CHECK(((reg_r(CRTC_VLINE_CRNT_VLINE) >> 16) & 0x7FF) == 100);
    env.ticks = vblank - 1;
    CHECK((reg_r(CRTC_STATUS) & 3) == 0);
    env.ticks = vblank;
    CHECK((reg_r(CRTC_STATUS) & 3) == 3);           /* CRTC_VBLANK_CUR and _SAVE */
    reg_w(CRTC_STATUS, 2);                          /* clear the saved bit */
    CHECK((reg_r(CRTC_STATUS) & 3) == 1);
    env.ticks = frame_px + 10;
    CHECK((reg_r(CRTC_STATUS) & 3) == 0);
    env.ticks = frame_px + vblank + 10;
    CHECK((reg_r(CRTC_STATUS) & 3) == 3);

    /* VBLANK interrupt: latched at the start of blank, until acknowledged. */
    power_on();
    map_card();
    program_raster();
    reg_w(GEN_INT_CNTL, 1);
    rage128_tick(&card);
    CHECK(!env.irq && (reg_r(GEN_INT_STATUS) & 1) == 0);
    env.ticks = vblank - 1;
    rage128_tick(&card);
    CHECK(!env.irq);
    env.ticks = vblank + 1;
    rage128_tick(&card);
    CHECK(env.irq && env.irq_edges == 1);
    CHECK((reg_r(GEN_INT_STATUS) & 1) == 1);
    env.ticks = vblank + 800;
    rage128_tick(&card);
    CHECK(env.irq && env.irq_edges == 1);           /* still latched */
    reg_w(GEN_INT_STATUS, 1);
    CHECK(!env.irq && (reg_r(GEN_INT_STATUS) & 1) == 0);
    env.ticks = frame_px + vblank;
    rage128_tick(&card);
    CHECK(env.irq && env.irq_edges == 3);

    /* VLINE interrupt at the programmed line. */
    reg_w(GEN_INT_STATUS, 1);
    reg_w(GEN_INT_CNTL, 2);
    reg_w(CRTC_VLINE_CRNT_VLINE, 40);
    reg_w(GEN_INT_STATUS, 2);
    CHECK(!env.irq);
    env.ticks = 2 * frame_px + 40 * 800 - 1;
    rage128_tick(&card);
    CHECK(!env.irq);
    env.ticks = 2 * frame_px + 40 * 800;
    rage128_tick(&card);
    CHECK(env.irq);
    CHECK((reg_r(GEN_INT_STATUS) & 3) == 2);
    reg_w(GEN_INT_CNTL, 0);
    rage128_tick(&card);
    CHECK(!env.irq);

    /* PCIRST# releases INTA#. */
    reg_w(GEN_INT_CNTL, 2);
    CHECK(env.irq);
    rage128_reset(&card);
    CHECK(!env.irq);
}

static void program_mode_640x480x8(void) {
    program_raster();
    reg_w(CRTC_GEN_CNTL, (1u << 25) | (1u << 24) | (2u << 8));
    reg_w(CRTC_PITCH, 80);
    reg_w(CRTC_OFFSET, 0);
}

static void test_scanout(void) {
    power_on();
    map_card();
    uint32_t w = 0, h = 0, y0, y1;

    /* CRTC_EXT_DISP_EN clear is the VGA core's picture. */
    CHECK(!rage128_scanout(&card, frame, 640, 480, &w, &h, true, &y0, &y1));

    program_mode_640x480x8();
    reg_w(DAC_CNTL, reg_r(DAC_CNTL) | 0x100);       /* DAC_8BIT_EN */
    reg_w(PALETTE_INDEX, 0);
    reg_w(PALETTE_DATA, 0x00000000u);
    reg_w(PALETTE_DATA, 0x00112233u);
    reg_w(PALETTE_DATA, 0x00FF8040u);
    CHECK((reg_r(PALETTE_INDEX) & 0xFF) == 3);
    memset(vram, 0, 640 * 480);
    vram[1] = 1;
    vram[479 * 640 + 639] = 2;
    vram[3 * 640 + 7] = 1;
    memset(frame, 0x55, sizeof frame);
    CHECK(rage128_scanout(&card, frame, 640, 480, &w, &h, true, &y0, &y1));
    CHECK(w == 640 && h == 480 && y0 == 0 && y1 == 480);
    CHECK(frame[0] == 0xFF000000u);
    CHECK(frame[1] == 0xFF112233u);
    CHECK(frame[3 * 640 + 7] == 0xFF112233u);
    CHECK(frame[479 * 640 + 639] == 0xFFFF8040u);

    /* Palette read-back uses its own index (23:16). */
    reg_w(PALETTE_INDEX, 1u << 16);
    CHECK(reg_r(PALETTE_DATA) == 0x00112233u);
    CHECK(reg_r(PALETTE_DATA) == 0x00FF8040u);
    CHECK(((reg_r(PALETTE_INDEX) >> 16) & 0xFF) == 3);

    /* DAC_8BIT_EN clear: 6-bit components shifted into the high bits. */
    reg_w(DAC_CNTL, reg_r(DAC_CNTL) & ~0x100u);
    reg_w(PALETTE_INDEX, 4);
    reg_w(PALETTE_DATA, 0x003F2010u);
    CHECK(reg_r(PALETTE_INDEX) == 5);
    reg_w(PALETTE_INDEX, 4u << 16);
    CHECK(reg_r(PALETTE_DATA) == 0x003F2010u);
    vram[10] = 4;
    CHECK(rage128_scanout(&card, frame, 640, 480, &w, &h, true, &y0, &y1));
    CHECK(frame[10] == 0xFFFC8040u);

    /* 16 bpp (565) and 32 bpp from CRTC_OFFSET with the pitch in 8-pixel units. */
    reg_w(CRTC_GEN_CNTL, (1u << 25) | (1u << 24) | (4u << 8));
    reg_w(CRTC_PITCH, 80);
    reg_w(CRTC_OFFSET, 0x2000);
    vram[0x2000] = 0x1F;  vram[0x2001] = 0xF8;      /* 0xF81F: red and blue */
    CHECK(rage128_scanout(&card, frame, 640, 480, &w, &h, true, &y0, &y1));
    CHECK(frame[0] == 0xFFFF00FFu);
    reg_w(CRTC_GEN_CNTL, (1u << 25) | (1u << 24) | (6u << 8));
    vram[0x2000] = 0x33;  vram[0x2001] = 0x22;  vram[0x2002] = 0x11;
    CHECK(rage128_scanout(&card, frame, 640, 480, &w, &h, true, &y0, &y1));
    CHECK(frame[0] == 0xFF112233u);

    /* 8 bpp again, with the hardware cursor: 64x64, AND plane then XOR plane
     * per 16-byte line. */
    reg_w(CRTC_GEN_CNTL, (1u << 25) | (1u << 24) | (2u << 8) | (1u << 16));
    reg_w(CRTC_OFFSET, 0);
    reg_w(DAC_CNTL, reg_r(DAC_CNTL) | 0x100);
    memset(vram + 0x100000, 0, 64 * 16);
    vram[0x100000] = 0x40 | 0x10;                   /* AND: pixels 1 and 3 */
    vram[0x100008] = 0x80 | 0x10;                   /* XOR: pixels 0 and 3 */
    reg_w(CUR_OFFSET, 0x100000);
    reg_w(CUR_CLR0, 0x0000FF00);
    reg_w(CUR_CLR1, 0x00FF0000);
    reg_w(CUR_HORZ_VERT_POSN, (100u << 16) | 50u);
    CHECK(rage128_scanout(&card, frame, 640, 480, &w, &h, true, &y0, &y1));
    CHECK(frame[50 * 640 + 100] == 0xFFFF0000u);    /* AND 0, XOR 1: CUR_CLR1 */
    CHECK(frame[50 * 640 + 101] == frame[50 * 640 + 99]);   /* AND 1, XOR 0: transparent */
    CHECK(frame[50 * 640 + 102] == 0xFF00FF00u);    /* AND 0, XOR 0: CUR_CLR0 */
    CHECK(frame[50 * 640 + 103] == 0xFFFFFFFFu);    /* AND 1, XOR 1: inverts black */
    CHECK(frame[51 * 640 + 100] == 0xFF00FF00u);
    CHECK(frame[49 * 640 + 100] == 0xFF000000u);
    CHECK(frame[50 * 640 + 99] == 0xFF000000u);

    /* Display disabled (CRTC_DISPLAY_DIS) blanks it. */
    reg_w(0x054, reg_r(0x054) | (1u << 10));
    CHECK(rage128_scanout(&card, frame, 640, 480, &w, &h, true, &y0, &y1));
    CHECK(frame[0] == 0xFF000000u && frame[50 * 640 + 100] == 0xFF000000u);
}

static void test_engine(void) {
    power_on();
    map_card();
    memset(vram, 0, 640 * 480);
    CHECK(reg_r(GUI_STAT) == 0x40);

    /* Solid fill, GMC defaults: PATCOPY into an 8 bpp destination at the
     * default offset and pitch, clipped to the default scissor. */
    reg_w(DEFAULT_OFFSET, 0);
    reg_w(DEFAULT_PITCH, 80);
    reg_w(DEFAULT_SC_BOTTOM_RIGHT, (479u << 16) | 639u);
    reg_w(DP_GUI_MASTER_CNTL, (0xF0u << 16) | (2u << 8) | (13u << 4) | (1u << 30));
    reg_w(DP_BRUSH_FRGD_CLR, 0x5A);
    reg_w(DST_Y_X, (10u << 16) | 20u);
    (void)vga_vram_take_any(&vga);
    reg_w(DST_WIDTH_HEIGHT, (30u << 16) | 5u);
    CHECK(vga_vram_take_any(&vga));
    for (unsigned y = 8; y < 17; y++) {
        for (unsigned x = 18; x < 52; x++) {
            const bool inside = y >= 10 && y < 15 && x >= 20 && x < 50;
            CHECK(vram[y * 640 + x] == (inside ? 0x5A : 0x00));
        }
    }

    /* A write mask keeps the other bits of the destination. */
    reg_w(DP_WRITE_MSK, 0x0F);
    reg_w(DP_BRUSH_FRGD_CLR, 0xC3);
    reg_w(DST_Y_X, (10u << 16) | 20u);
    reg_w(DST_WIDTH_HEIGHT, (2u << 16) | 1u);
    CHECK(vram[10 * 640 + 20] == 0x53 && vram[10 * 640 + 21] == 0x53);
    CHECK(vram[10 * 640 + 22] == 0x5A);

    /* DST_PITCH_OFFSET: offset in 32-byte units, pitch in 8-pixel units; the
     * scissor clips the draw. */
    reg_w(DP_GUI_MASTER_CNTL, (0xF0u << 16) | (2u << 8) | (13u << 4) | (1u << 30) | 2u);
    reg_w(DST_PITCH_OFFSET, (80u << 21) | (0x4000u >> 5));
    reg_w(DP_BRUSH_FRGD_CLR, 0x77);
    reg_w(DST_Y_X, (1u << 16) | 636u);
    reg_w(DST_WIDTH_HEIGHT, (8u << 16) | 2u);
    CHECK(vram[0x4000 + 640 + 636] == 0x77 && vram[0x4000 + 640 + 639] == 0x77);
    CHECK(vram[0x4000 + 2 * 640 + 639] == 0x77);
    CHECK(vram[0x4000 + 2 * 640] == 0x00);          /* clipped at x = 639, not wrapped */
    CHECK(vram[0x4000 + 3 * 640] == 0x00);
    CHECK(vram[0x4000 + 640] == 0x00);
}

static void test_reset(void) {
    power_on();
    card.device_id = 0x5246;
    map_card();
    program_text_mode();
    reg_w(BIOS_0_SCRATCH, 0x12345678u);
    reg_w(PALETTE_INDEX, 0);
    reg_w(PALETTE_DATA, 0x00112233u);
    cfg_w(0x4C, 4, 0xAABBCCDDu);
    vram[0x1234] = 0x99;
    vga.plane[0][5] = 0x77;

    rage128_reset(&card);
    CHECK(cfg_r(0x00, 4) == 0x52461002u);           /* the device id in force at reset */
    CHECK(cfg_r(0x04, 2) == 0x0080u);
    CHECK(cfg_r(0x10, 4) == 0x00000008u);
    CHECK(cfg_r(0x2C, 4) == 0);
    uint64_t v;
    CHECK(!rage128_mem_access(&card, false, REG_BASE, 4, &v));
    map_card();
    CHECK(reg_r(BIOS_0_SCRATCH) == 0);
    CHECK(reg_r(0x030) == 0x880F4F41u);
    CHECK(vram[0x1234] == 0x99);                    /* the frame buffer survives */
    CHECK(vga.plane[0][5] == 0x77);
    CHECK(vga.seq[2] == 0 && vga.gc[6] == 0 && vga.misc == 0x01);
    CHECK(io_r(0x3CC, 1) == 0x01);
    reg_w(PALETTE_INDEX, 1u << 16);
    CHECK(reg_r(PALETTE_DATA) == 0);

    /* Host pointers can be re-attached around a raw copy of the state. */
    static Rage128 copy;
    static Vga vcopy;
    copy = card;
    vcopy = vga;
    uint8_t vram2[16] = { 0 };
    rage128_rebind(&copy, &vcopy, &wiring);
    vga_attach_vram(&vcopy, vram2, sizeof vram2);
    CHECK(vcopy.vram_size == sizeof vram2);
    CHECK(rage128_cfg_read(&copy, 0x00, 4) == 0x52461002u);
}

static uint8_t dma_ram[0x10000];
static unsigned dma_reads, dma_writes;

static bool dma_read(void *opaque, uint64_t addr, void *out, unsigned len) {
    (void)opaque;
    if (addr >= sizeof dma_ram || len > sizeof dma_ram - addr) return false;
    memcpy(out, dma_ram + addr, len);
    dma_reads++;
    return true;
}

static bool dma_write(void *opaque, uint64_t addr, const void *in, unsigned len) {
    (void)opaque;
    if (addr >= sizeof dma_ram || len > sizeof dma_ram - addr) return false;
    memcpy(dma_ram + addr, in, len);
    dma_writes++;
    return true;
}

static void dma_put(unsigned addr, uint32_t v) {
    for (unsigned i = 0; i < 4; i++) dma_ram[addr + i] = v >> (i * 8);
}

static void cce_poll(void) {
    const Rage128Dma dma = {dma_read, dma_write, NULL};
    rage128_cce_tick(&card, &dma);
}

static void cce_start(void) {
    power_on();
    map_card();
    memset(dma_ram, 0, sizeof dma_ram);
    dma_reads = dma_writes = 0;
    /* Noncontiguous physical pages, accessed through the PCI GART. */
    dma_put(0x1000, 0x4000);
    dma_put(0x1004, 0x8000);
    reg_w(0x17c, 0x1000);
    reg_w(0x030, 0);
    CHECK(reg_r(0x7fc) == (1u << 30)); /* high-level decoder ready at reset */
    reg_w(0x7fc, 0); /* hold the microengine while exercising DMA prefetch */
    reg_w(0x704, 0x2800000a); /* 8 KiB, 192-word DMA FIFO, no writeback */
}

static void test_cce(void) {
    cce_start();
    for (unsigned i = 0; i < 256; i++) dma_put(0x4000 + i * 4, 0x80000000);
    reg_w(0x714, 64);
    cce_poll();
    CHECK(dma_reads == 0 && reg_r(0x710) == 0); /* PCI bus master off */
    cfg_w(4, 2, 7);
    reg_w(0x030, 1u << 6);
    cce_poll();
    CHECK(dma_reads == 0); /* BUS_MASTER_DIS also gates DMA */
    reg_w(0x030, 0);
    reg_w(0x700, 0x02000000); /* AGP-space alias of the PCI GART ring */
    cce_poll();
    CHECK(reg_r(0x710) == 64); /* XP primes NOPs with the microengine stopped */
    CHECK(card.cce.count == 64 && reg_r(0x7b8) == 128);
    CHECK(dma_writes == 0);
    reg_w(0x714, 256);
    cce_poll();
    CHECK(reg_r(0x710) == 192 && card.cce.count == 192); /* bounded FIFO */
    reg_w(0x7fc, 1u << 30);
    cce_poll();
    CHECK(reg_r(0x710) == 256 && reg_r(0x7b8) == 192);

    /* Ring wrap crosses a GART page and resumes a split type-0 packet.
     * No register writes occur until all of that packet has arrived. */
    reg_w(0x710, 2047);
    reg_w(0x714, 0);
    dma_put(0x8ffc, (1u << 16) | (BIOS_0_SCRATCH / 4));
    cce_poll();
    CHECK(reg_r(0x710) == 0 && card.cce.have == 1);
    CHECK(reg_r(BIOS_0_SCRATCH) == 0);
    static Rage128 saved;
    saved = card; /* the partial packet survives a raw snapshot/rebind */
    rage128_reset(&card);
    card = saved;
    rage128_rebind(&card, &vga, &wiring);
    dma_put(0x4000, 0x11223344);
    dma_put(0x4004, 0x55667788);
    reg_w(0x70c, 0x2000);
    reg_w(0x704, 0x2000000a); /* enable physical read-pointer writeback */
    reg_w(0x714, 2);
    cce_poll();
    CHECK(reg_r(BIOS_0_SCRATCH) == 0x11223344);
    CHECK(reg_r(BIOS_0_SCRATCH + 4) == 0x55667788);
    CHECK(dma_ram[0x2000] == 2 && dma_ram[0x2001] == 0 && dma_writes == 2);

    /* Type-1 writes two unrelated registers; type-0 ONE_REG_WR repeatedly
     * targets the same register instead of spilling to its neighbour. */
    dma_put(0x4008, 0x40000000 | (BIOS_0_SCRATCH / 4) |
                          ((BIOS_0_SCRATCH + 12) / 4 << 11));
    dma_put(0x400c, 0xaabbccdd);
    dma_put(0x4010, 0xeeff0011);
    dma_put(0x4014, (1u << 16) | 0x8000 | (BIOS_0_SCRATCH / 4));
    dma_put(0x4018, 0x1234);
    dma_put(0x401c, 0x5678);
    reg_w(0x714, 8);
    cce_poll();
    CHECK(reg_r(BIOS_0_SCRATCH) == 0x5678);
    CHECK(reg_r(BIOS_0_SCRATCH + 4) == 0x55667788);
    CHECK(reg_r(BIOS_0_SCRATCH + 12) == 0xeeff0011);

    /* A bad GART entry cannot acknowledge unfetched commands. */
    dma_put(0x1000, 0xfffff000);
    reg_w(0x714, 9);
    cce_poll();
    CHECK(reg_r(0x710) == 8);
    /* PIO uses the same packet parser, without requiring bus mastering. */
    reg_w(0x704, 0x10000000);
    cfg_w(4, 2, 3);
    reg_w(0x1000, BIOS_0_SCRATCH / 4);
    reg_w(0x1004, 0xabcdef01);
    cce_poll();
    CHECK(reg_r(BIOS_0_SCRATCH) == 0xabcdef01);
    reg_w(0x7d4, 5);
    reg_w(0x7dc, 0x12345678);
    reg_w(0x7e0, 0x9abcdef0);
    reg_w(0x7d8, 5);
    CHECK(reg_r(0x7dc) == 0x12345678 && reg_r(0x7e0) == 0x9abcdef0);
    CHECK(reg_r(0x7d4) == 6);
    reg_w(0x1000, BIOS_0_SCRATCH / 4);
    reg_w(0x0f0, 1);
    CHECK(card.cce.have == 0 && card.cce.count == 0);
}

static void submit(unsigned op, const uint32_t *words, unsigned n) {
    reg_w(0x1000, 0xc0000000 | ((n - 1) << 16) | (op << 8));
    for (unsigned i = 0; i < n; i++) reg_w(0x1004, words[i]);
    cce_poll();
}
#define SUBMIT(op, ...) do { const uint32_t words[] = {__VA_ARGS__}; \
    submit(op, words, sizeof words / sizeof words[0]); } while (0)

static void test_cce_drawing(void) {
    cce_start();
    reg_w(0x704, 0x10000000);
    reg_w(0x7fc, 1u << 30);
    memset(vram, 0, sizeof vram);
    reg_w(DEFAULT_OFFSET, 0);
    reg_w(DEFAULT_PITCH, 2); /* 16 pixels */
    reg_w(DEFAULT_SC_BOTTOM_RIGHT, 0x000f000f);
    const uint32_t fill = 0x70f002d0; /* 8bpp, solid PATCOPY, defaults */
    SUBMIT(0x9a, fill, 0x5a, 0x00020003, 0x00040002);
    CHECK(vram[3 * 16 + 2] == 0x5a && vram[4 * 16 + 5] == 0x5a);
    CHECK(vram[3 * 16 + 1] == 0 && vram[5 * 16 + 2] == 0);
    SUBMIT(0x91, fill, 0x7b, 0x00060008, 0x0008000a);
    CHECK(vram[6 * 16 + 8] == 0x7b && vram[7 * 16 + 9] == 0x7b);
    CHECK(vram[8 * 16 + 8] == 0);
    /* Copy overlaps its source in both axes. */
    for (unsigned y = 0; y < 3; y++) for (unsigned x = 0; x < 4; x++)
        vram[y * 16 + x] = 1 + y * 4 + x;
    const uint32_t copy = 0x72cc32f0;
    SUBMIT(0x9b, copy, 0, 0x00010001, 0x00040003);
    for (unsigned y = 0; y < 3; y++) for (unsigned x = 0; x < 4; x++)
        CHECK(vram[(y + 1) * 16 + x + 1] == 1 + y * 4 + x);
    SUBMIT(0x28, copy, 0x00010001, 0x00040007, 0x00040003);
    for (unsigned y = 0; y < 3; y++) for (unsigned x = 0; x < 4; x++)
        CHECK(vram[(y + 7) * 16 + x + 4] == 1 + y * 4 + x);
    /* Text setup can be sent without a bitmap, then reused by NEXTCHAR. */
    SUBMIT(0x94, 0x73cc02f0, 0xe1, 0x19);
    SUBMIT(0x19, 0x000a0000, 0x00010008, 0x000000a5);
    CHECK(vram[10 * 16] == 0xe1 && vram[10 * 16 + 1] == 0x19);
    CHECK(vram[10 * 16 + 7] == 0xe1);
    CHECK(!card.host.active);
    /* Color-keyed source pixels leave the destination intact. */
    vram[0] = 1; vram[1] = 2; vram[32] = vram[33] = 9;
    SUBMIT(0x9c, copy, 0x01000005, 1, 0, 0, 0x00020000, 0x00010002);
    CHECK(vram[32] == 9 && vram[33] == 2);
    /* Nearest-sample 2x RGB scaling. */
    uint32_t colors[] = {0xff123456, 0xffabcdef};
    memcpy(vram + 0x1000, colors, sizeof colors);
    SUBMIT(0x96, 0x7acc36f0, 0x100, 0, 0x02180840, 6, 0x1000, 1,
           0, 0x8000, 0x10000, 0, 0x00010004);
    uint32_t out[4]; memcpy(out, vram, sizeof out);
    CHECK(out[0] == colors[0] && out[1] == colors[0]);
    CHECK(out[2] == colors[1] && out[3] == colors[1]);
    /* Half-transparent red over opaque blue, additive SRCALPHA/INVSRCALPHA. */
    colors[0] = 0x80ff0000; memcpy(vram + 0x1000, colors, 4);
    colors[1] = 0xff0000ff; memcpy(vram, colors + 1, 4);
    SUBMIT(0x96, 0x7acc36f0, 0x00540100, 0x200, 0x02180840, 6, 0x1000, 1,
           0, 0x10000, 0x10000, 0, 0x00010001);
    memcpy(out, vram, 4);
    CHECK((out[0] & 0xffffff) == 0x80007f);
    CHECK(reg_r(0x7b8) == 192);
    /* A truncated second rectangle must not draw even the first one. */
    uint8_t before = vram[200];
    SUBMIT(0x9a, fill, 0xff, 0x0008000c, 0x00010001, 0);
    CHECK(vram[200] == before);
    CHECK(card.cce.have && (reg_r(0x7b8) & 0x80000000));
    reg_w(0x0f0, 1);
}

static void test_cce_copy_directions(void) {
    /* Overlapping copies need all four traversal combinations. In particular,
     * copying up/left used to send 3 to the composite direction register,
     * whose direction bits are at 31 and 15, and walk backwards instead. */
    for (unsigned op = 0; op < 2; op++) for (unsigned dir = 0; dir < 4; dir++) {
        cce_start();
        reg_w(0x704, 0x10000000);
        reg_w(0x7fc, 1u << 30);
        reg_w(DEFAULT_OFFSET, 0);
        reg_w(DEFAULT_PITCH, 2);
        reg_w(DEFAULT_SC_BOTTOM_RIGHT, 0x000f000f);
        uint8_t expected[16 * 16];
        for (unsigned i = 0; i < sizeof expected; i++) vram[i] = expected[i] = i;
        unsigned sx = 3, sy = 3, dx = dir & 1 ? 2 : 4, dy = dir & 2 ? 2 : 4;
        for (unsigned y = 0; y < 4; y++) for (unsigned x = 0; x < 5; x++)
            expected[(dy + y) * 16 + dx + x] = vram[(sy + y) * 16 + sx + x];
        SUBMIT(op ? 0x28 : 0x9b, 0x72cc32f0,
               sx << 16 | sy, dx << 16 | dy, 5u << 16 | 4u);
        CHECK((reg_r(0x16c0) & 3) == dir);
        CHECK(memcmp(vram, expected, sizeof expected) == 0);
        CHECK(reg_r(0x7b8) == 192);
    }

    /* Exact packet from the XP mouse-over crash: copy a 417x110 rectangle
     * from (111,146) to (106,144) in a 640-pixel, 32bpp surface at 0x8000.
     * XP's 0x28 builder packs X/width in the HIGH half, just like 0x9b;
     * swapping halves misplaced window contents even after fixing direction. */
    cce_start();
    reg_w(0x704, 0x10000000);
    reg_w(0x7fc, 1u << 30);
    reg_w(DEFAULT_OFFSET, 0x8000);
    reg_w(DEFAULT_PITCH, 80);
    reg_w(DEFAULT_SC_BOTTOM_RIGHT, 0x1fff1fff);
    static uint32_t expected[640 * 530];
    for (unsigned i = 0; i < sizeof expected / sizeof *expected; i++)
        expected[i] = 0xff000000u | i;
    memcpy(vram + 0x8000, expected, sizeof expected);
    for (unsigned y = 0; y < 110; y++)
        memmove(expected + (144 + y) * 640 + 106,
                expected + (146 + y) * 640 + 111, 417 * sizeof *expected);
    SUBMIT(0x28, 0x72cc36f0, 0x006f0092, 0x006a0090, 0x01a1006e);
    CHECK(memcmp(vram + 0x8000, expected, sizeof expected) == 0);
    CHECK(reg_r(0x7b8) == 192);
}

static void test_engine_source_bounds(void) {
    /* A source before VRAM must not wrap its end address to zero and pass
     * the bounds check. Also cover a pixel straddling the end of VRAM. */
    const unsigned types[] = {2, 4, 6}, bytes[] = {1, 2, 4};
    for (unsigned t = 0; t < 3; t++) {
        power_on();
        map_card();
        reg_w(DEFAULT_OFFSET, 0);
        reg_w(DEFAULT_PITCH, 2);
        reg_w(DEFAULT_SC_BOTTOM_RIGHT, 0x000f000f);
        reg_w(DP_GUI_MASTER_CNTL, 0x72cc30f0 | types[t] << 8);
        const unsigned bpp = bytes[t], dest = 16 * bpp;
        memset(vram, 0x5a, 32 * bpp);
        reg_w(0x1434, 0x00003fff); /* source (-1,0): invalid, then valid */
        reg_w(DST_Y_X, 0x00010000);
        reg_w(DST_WIDTH_HEIGHT, 0x00020001);
        for (unsigned k = 0; k < bpp; k++) {
            CHECK(vram[dest + k] == 0);
            CHECK(vram[dest + bpp + k] == 0x5a);
        }
        /* A negative coordinate can still name VRAM when the surface has
         * a nonzero base; preserve that valid signed address arithmetic. */
        reg_w(0x15ac, 16);
        reg_w(DST_Y_X, 0x00010000);
        reg_w(DST_WIDTH_HEIGHT, 0x00010001);
        CHECK(vram[dest] == 0x5a);
        size_t original_size = vga.vram_size;
        vga.vram_size = 0x1000 + bpp - 1;
        reg_w(0x15ac, 0x1000);
        reg_w(0x1434, 0);
        reg_w(DST_WIDTH_HEIGHT, 0x00010001);
        CHECK(vram[dest] == 0);
        vga.vram_size = original_size;
    }
}

static void test_cce_polyline(void) {
    cce_start();
    reg_w(0x704, 0x10000000);
    reg_w(0x7fc, 1u << 30);
    reg_w(DEFAULT_OFFSET, 0);
    reg_w(DEFAULT_PITCH, 2);
    reg_w(DEFAULT_SC_BOTTOM_RIGHT, 0x000f000f);
    memset(vram, 0, 256);
    /* A closed XOR polyline must draw each corner exactly once. */
    SUBMIT(0x95, 0x705a32d0, 0xff,
           0x00010001, 0x00010005, 0x00050005, 0x00050001, 0x00010001);
    for (unsigned y = 0; y < 16; y++) for (unsigned x = 0; x < 16; x++) {
        bool edge = ((x == 1 || x == 5) && y >= 1 && y <= 5) ||
                    ((y == 1 || y == 5) && x >= 1 && x <= 5);
        CHECK(vram[y * 16 + x] == (edge ? 255 : 0));
    }
    const int dx[] = {1,1,0,-1,-1,-1,0,1}, dy[] = {0,1,1,1,0,-1,-1,-1};
    for (unsigned d = 0; d < 8; d++) {
        memset(vram, 0, 256);
        uint8_t expected[256] = {0};
        for (int i = 0; i < 4; i++) expected[(8 + i * dy[d]) * 16 + 8 + i * dx[d]] = 0x5a;
        SUBMIT(0x95, 0x70f032d0, 0x5a, 0x00080008,
               (unsigned)(8 + 4 * dy[d]) << 16 | (unsigned)(8 + 4 * dx[d]));
        CHECK(memcmp(vram, expected, sizeof expected) == 0);
    }
    /* Task Manager's actual graph-grid command: a clipped vertical line
     * in an offscreen 32bpp surface, using explicit pitch and scissors. */
    const unsigned base = 0xf7e8u << 5, pitch = 32 * 8 * 4;
    memset(vram + base, 0, pitch * 60);
    SUBMIT(0x95, 0x72f036da, 0x0400f7e8, 0x00000009, 0x00390009,
           0x00008040, 0x00000009, 0x003a0009);
    for (unsigned y = 0; y < 60; y++) {
        uint32_t v; memcpy(&v, vram + base + y * pitch + 9 * 4, sizeof v);
        CHECK(v == (y < 58 ? 0x8040u : 0));
    }
    CHECK(reg_r(0x7b8) == 192);
    /* Missing the second vertex cannot consume or apply new settings. */
    uint32_t color = reg_r(DP_BRUSH_FRGD_CLR);
    SUBMIT(0x95, 0x70f032d0, 0x66, 0x00010001);
    CHECK(reg_r(DP_BRUSH_FRGD_CLR) == color);
    CHECK(card.cce.have && (reg_r(0x7b8) & 0x80000000));
    reg_w(0x0f0, 1);
}

int main(void) {
    test_config();
    test_bars();
    test_rom();
    test_registers();
    test_vga_ports();
    test_crtc_shadow();
    test_legacy_window();
    test_vga_planar();
    test_aperture();
    test_raster();
    test_vga_raster();
    test_scanout();
    test_engine();
    test_reset();
    test_cce();
    test_cce_drawing();
    test_cce_copy_directions();
    test_engine_source_bounds();
    test_cce_polyline();
    printf("rage128: %u checks passed\n", checks);
    return 0;
}
