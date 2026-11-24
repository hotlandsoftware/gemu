#pragma once
/* The NES/Famicom cartridge slot and the boards that plug into it.
 *
 * A NesCart is one cartridge: its iNES image (PRG ROM, CHR ROM or RAM),
 * 8 KB of PRG-RAM, and the board's mapper.  The console calls it for CPU
 * accesses at $4020-$FFFF, for the PPU's pattern-table fetches
 * ($0000-$1FFF), and when the board wants them, for nametable accesses, a
 * scanline clock and every CPU (M2) cycle.
 *
 * Banking goes through generic windows: four 8 KB PRG windows at
 * $8000/$A000/$C000/$E000 (prg_off) and eight 1 KB CHR windows (chr_off),
 * byte offsets into prg[] and chr[].  A board points them; the common
 * path reads through them without calling the board.  The few boards that
 * do more on a fetch (MMC2's latches, MMC5's split banks and RAM windows)
 * have their own hooks.
 *
 * The board drives the PPU's nametable mirroring itself (ppu->mirror, its
 * CIRAM A10 wiring) and, for the boards with a nametable hook, reads the
 * PPU's CIRAM (ppu->vram).  MMC5 also looks at the PPU's rendering state,
 * which the real chip works out by watching the PPU bus.
 *
 * The cartridge's /IRQ is `irq`; irq_changed() is called when it changes.
 *
 * Mappers: 0 NROM, 1 MMC1, 2 UxROM, 3 CNROM, 4 MMC3, 5 MMC5, 7 AxROM,
 * 9 MMC2, 24/26 VRC6, 66 GxROM, 119 TQROM, 178 Waixing, 228 Action 52, the
 * boards in nes_boards.c, and the VS. System's board (nes_cart_init_vs).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "rp2c02.h"

struct NesCart;

/* A board.  Every hook but reset may be NULL. */
typedef struct NesMapper {
    uint16_t    number;          /* iNES mapper number */
    const char *name;
    void    (*reset)(struct NesCart *c);
    /* CPU write at $4020-$FFFF: true if the board took it.  Otherwise
     * $6000-$7FFF goes to PRG-RAM when prg_ram() says it's mapped. */
    bool    (*write)(struct NesCart *c, uint16_t addr, uint8_t val);
    /* CPU read at $4020-$7FFF: true if the board drives the bus. */
    bool    (*read)(struct NesCart *c, uint16_t addr, uint8_t *val);
    /* $6000-$7FFF is PRG-RAM (NULL: never). */
    bool    (*prg_ram)(const struct NesCart *c);
    /* $8000-$FFFF when it isn't the PRG windows. */
    uint8_t (*prg_read)(const struct NesCart *c, uint16_t addr);
    /* A pattern-table fetch when it isn't the CHR windows. */
    uint8_t (*chr_read)(struct NesCart *c, uint16_t addr);
    /* Nametables ($2000-$2FFF) instead of the PPU's own mirroring. */
    uint8_t (*nt_read)(struct NesCart *c, uint16_t addr);
    void    (*nt_write)(struct NesCart *c, uint16_t addr, uint8_t val);
    /* The PPU's scanline clock (rp2c02's irq_scanline). */
    void    (*scanline)(struct NesCart *c);
    /* One M2 cycle; returns the expansion audio level. */
    float   (*tick)(struct NesCart *c);
    /* The 2A03's OUT pins ($4016 writes); only the VS. System's board. */
    void    (*out)(struct NesCart *c, uint8_t val);
    /* $4020-$5FFF without side effects, for debuggers. */
    bool    (*peek)(const struct NesCart *c, uint16_t addr, uint8_t *val);
    /* Board registers for the monitor's mapperdump. */
    void    (*dump)(const struct NesCart *c, char *buf, size_t len);
} NesMapper;

/* What the iNES header says (also the snapshot's cartridge check). */
typedef struct {
    uint8_t  prg_banks;   /* x 16 KB */
    uint8_t  chr_banks;   /* x 8 KB (CHR RAM: 1) */
    uint8_t  mapper;
    uint8_t  mirror;      /* RP2C02_MIRROR_*, from the solder pads */
    bool     has_battery;
} NesCartInfo;

/* Board registers of the boards in nes_boards.c; what each field means
 * depends on the board. */
typedef struct {
    uint8_t  regs[16];
    uint8_t  cmd;            /* bank/command select latch */
    uint16_t chr[8];         /* 1 KB CHR bank numbers (VRC: 9-bit) */
    uint8_t  nt[4];          /* nametable per quadrant: bit 0 = CIRAM page;
                                Namco 163: values below $E0 are CHR ROM banks */
    bool     chr_disabled;   /* mapper 185: CHR reads return $FF */
    bool     prg6_rom;       /* FME-7: $6000-$7FFF maps ROM at prg6_offset */
    bool     prg6_ram;       /* $6000-$7FFF RAM is enabled */
    uint32_t prg6_offset;

    uint16_t irq_counter;
    uint16_t irq_latch;
    bool     irq_enabled;
    bool     irq_counting;
    bool     irq_after;      /* VRC: re-enable after acknowledge */
    bool     irq_mode;       /* VRC: cycle mode; RAMBO-1: CPU-cycle mode */
    bool     irq_reload;
    int      irq_prescaler;

    uint8_t  n163_ram[128];  /* Namco 163 internal RAM (sound registers) */
    uint8_t  n163_addr;
} NesBoard;

typedef struct {                 /* MMC1 (SxROM) */
    uint8_t  shift;              /* 5-bit serial shift register */
    uint8_t  shift_count;        /* bits received so far (0-4) */
    uint8_t  ctrl;               /* $8000-$9FFF */
    uint8_t  chr0;               /* $A000-$BFFF */
    uint8_t  chr1;               /* $C000-$DFFF */
    uint8_t  prg;                /* $E000-$FFFF */
} NesMmc1;

typedef struct {                 /* MMC3 (TxROM, TQROM) */
    uint8_t  bank_sel;           /* bits 2:0 target, 6 PRG mode, 7 CHR invert */
    uint8_t  regs[8];            /* R0-R7 */
    uint8_t  irq_latch;
    uint8_t  irq_counter;
    bool     irq_reload;
    bool     irq_enabled;
} NesMmc3;

typedef struct {                 /* MMC5 (ExROM) */
    uint8_t  prg_mode;           /* $5100 */
    uint8_t  chr_mode;           /* $5101 */
    uint8_t  exram_mode;         /* $5104 */
    uint8_t  nt_map;             /* $5105: four 2-bit fields */
    uint8_t  fill_tile;          /* $5106 */
    uint8_t  fill_attr;          /* $5107 */
    uint8_t  prg_regs[5];        /* $5113-$5117 */
    uint8_t  chr_regs[12];       /* $5120-$512B */
    uint8_t  chr_hi;             /* $5130 */
    uint8_t  irq_target;         /* $5203 */
    bool     irq_enabled;        /* $5204 bit 7 */
    bool     irq_pending;
    bool     in_frame;
    uint8_t  irq_counter;        /* visible scanlines so far */
    uint8_t  mul[2];             /* $5205-$5206 */
    bool     last_chr_bg;        /* the last CHR write was to $5128-$512B */
    uint8_t  exattr_latch;       /* ExRAM byte for the current BG tile */
    bool     prg_is_ram[4];      /* a PRG window maps PRG-RAM */
    uint8_t  exram[0x400];       /* $5C00-$5FFF / nametable */
} NesMmc5;

typedef struct {                 /* MMC2 (PxROM) */
    uint8_t  prg_reg;            /* $A000: 8 KB at $8000 */
    uint8_t  chr_lfd, chr_lfe;   /* $B000/$C000: left 4 KB, latch FD/FE */
    uint8_t  chr_rfd, chr_rfe;   /* $D000/$E000: right 4 KB */
    bool     latch_l, latch_r;   /* false = FD, true = FE */
} NesMmc2;

typedef struct {                 /* Konami VRC6a/VRC6b */
    uint8_t  irq_latch;
    uint8_t  irq_counter;
    bool     irq_enabled;
    bool     irq_after;          /* enable after the next acknowledge */
    bool     irq_mode;           /* 0 scanline (341 CPU cycles), 1 CPU cycle */
    int      prescaler;
    uint8_t  p1_ctrl;            /* $9000: mode, duty, volume */
    uint16_t p1_period;
    bool     p1_en;
    uint16_t p1_timer;
    uint8_t  p1_seq;
    uint8_t  p2_ctrl;
    uint16_t p2_period;
    bool     p2_en;
    uint16_t p2_timer;
    uint8_t  p2_seq;
    uint8_t  saw_rate;           /* $B000: accumulator rate */
    uint16_t saw_period;
    bool     saw_en;
    uint16_t saw_timer;
    uint8_t  saw_accum;
    uint8_t  saw_step;           /* 0-13 */
    bool     halt;               /* $9003 bit 0 */
} NesVrc6;

typedef struct {                 /* mapper 178 (Waixing) */
    uint8_t  mode;               /* $4800: PRG mode, mirroring */
    uint8_t  prg_lo;             /* $4801: inner 16 KB bank */
    uint8_t  prg_hi;             /* $4802: outer 128 KB block */
} NesM178;

typedef struct NesCart {
    NesCartInfo info;
    const NesMapper *ops;
    uint8_t *prg;                /* PRG ROM (heap) */
    uint8_t *chr;                /* CHR ROM or RAM (heap); TQROM: ROM + 8 KB RAM */
    bool     chr_is_ram;

    /* Wiring (kept by nes_cart_rewire) */
    Rp2c02  *ppu;
    void   (*irq_changed)(void *ud);
    void    *ud;

    /* State */
    uint32_t prg_off[4];         /* 8 KB PRG windows: $8000, $A000, $C000, $E000 */
    uint32_t chr_off[8];         /* 1 KB CHR windows: PPU $0000-$1FFF */
    uint8_t  chr_wmask;          /* CHR windows that are RAM */
    bool     irq;                /* the cartridge's /IRQ, active */
    uint8_t  prg_ram[0x2000];    /* $6000-$7FFF (battery-backed on some) */
    union {
        NesBoard board;
        NesMmc1  mmc1;
        NesMmc3  mmc3;
        NesMmc5  mmc5;
        NesMmc2  mmc2;
        NesVrc6  vrc6;
        NesM178  m178;
    } m;
} NesCart;

/* Load an iNES file / image.  The cartridge must be empty (zeroed, or
 * after nes_cart_free); wiring set beforehand is kept.  Messages go to
 * stderr; the loaded cartridge is described on stdout. */
bool nes_cart_load(NesCart *c, const char *path);
bool nes_cart_load_mem(NesCart *c, const uint8_t *data, size_t len, const char *name);
/* The VS. System's board: 32 KB PRG, two 8 KB CHR banks picked by OUT2
 * (chr NULL: 8 KB CHR RAM), PRG-RAM.  Takes the buffers. */
bool nes_cart_init_vs(NesCart *c, uint8_t *prg32k, uint8_t *chr16k);
/* No PRG, 8 KB of CHR RAM: the Famicom Disk System's RAM adapter. */
bool nes_cart_init_chr_ram(NesCart *c);
/* Free the image; the slot is empty (reads open bus).  Wiring is kept. */
void nes_cart_free(NesCart *c);
bool nes_cart_supported(unsigned mapper);

/* Reset the board's registers (the console's reset line; PRG-RAM kept). */
void nes_cart_reset(NesCart *c);

/* CPU $4020-$7FFF ($8000+: nes_cart_prg); `bus` is the open-bus value. */
uint8_t nes_cart_read(NesCart *c, uint16_t addr, uint8_t bus);
void    nes_cart_write(NesCart *c, uint16_t addr, uint8_t val);
/* $4016 writes: the 2A03's OUT pins. */
void    nes_cart_out(NesCart *c, uint8_t val);
bool    nes_cart_prg_ram_mapped(const NesCart *c);
/* The byte at $8000-$FFFF; needs c->prg.  No side effects. */
static inline uint8_t nes_cart_prg(const NesCart *c, uint16_t addr) {
    if (c->ops->prg_read) return c->ops->prg_read(c, addr);
    return c->prg[c->prg_off[(addr >> 13) & 3u] + (addr & 0x1FFFu)];
}
/* $4020-$FFFF without side effects: false if nothing is there to read. */
bool    nes_cart_peek(const NesCart *c, uint16_t addr, uint8_t *val);
/* The expansion audio level after one M2 cycle; boards without a cycle
 * counter (nes_cart_ticks false) needn't be called. */
static inline bool nes_cart_ticks(const NesCart *c) { return c->ops->tick != NULL; }
static inline float nes_cart_tick(NesCart *c) { return c->ops->tick(c); }

/* PPU hooks, with ud = the NesCart.  Install scanline/nt only when
 * nes_cart_wants_scanline/_nt. */
uint8_t nes_cart_chr_read(uint16_t addr, void *ud);
void    nes_cart_chr_write(uint16_t addr, uint8_t val, void *ud);
uint8_t nes_cart_nt_read(uint16_t addr, void *ud);
void    nes_cart_nt_write(uint16_t addr, uint8_t val, void *ud);
void    nes_cart_scanline(void *ud);
static inline bool nes_cart_wants_scanline(const NesCart *c) { return c->ops->scanline != NULL; }
static inline bool nes_cart_wants_nt(const NesCart *c) { return c->ops->nt_read != NULL; }

/* Sizes of the image's parts. */
static inline size_t nes_cart_prg_size(const NesCart *c) { return c->prg ? (size_t)c->info.prg_banks * 0x4000u : 0; }
static inline size_t nes_cart_chr_size(const NesCart *c) { return c->chr ? (size_t)c->info.chr_banks * 0x2000u : 0; }
/* The CHR RAM a snapshot carries: all of CHR on CHR-RAM carts, the 8 KB
 * beside the ROM on TQROM; NULL if none. */
uint8_t *nes_cart_chr_ram(const NesCart *c, size_t *len);

/* Raw-struct snapshots: clear the pointers in a copy that will be saved;
 * put the live ones back after one is loaded over c. */
void nes_cart_strip(NesCart *c);
void nes_cart_rewire(NesCart *c, const NesCart *live);

/* "mapper=N (name)", the windows and the board's registers. */
void nes_cart_dump(const NesCart *c, char *buf, size_t len);
