#pragma once
#include "mos6502cfg.h"
#include "fds.h"
#include "mos6502.h"
#include "rp2c02.h"
#include "apu2a03.h"
#include "gemu/gemu_display.h"
#if defined(HAVE_ALSA) || defined(HAVE_WINMIDI)
#  include "../audio/apu_midi.h"
#endif
#include "gemu/vnc.h"
#include "gemu/monitor.h"
#ifdef GEMU_GTK
#  include "../vga/hex_editor.h"
#endif
#ifdef HAVE_ROB
#  include "rob.h"
#  include "rob_window.h"
#endif
#include <stdint.h>
#include <stdbool.h>

/* ── NES controller buttons (shift-register order: A comes out first) ─────── */
#define NES_BTN_A      0x01u
#define NES_BTN_B      0x02u
#define NES_BTN_SELECT 0x04u
#define NES_BTN_START  0x08u
#define NES_BTN_UP     0x10u
#define NES_BTN_DOWN   0x20u
#define NES_BTN_LEFT   0x40u
#define NES_BTN_RIGHT  0x80u

#define NES_GG_MAX 16

typedef struct {
    uint16_t addr;
    uint8_t  val;
    uint8_t  cmp;
    bool     has_cmp;
    char     code[9]; /* 6- or 8-char code + NUL */
} NesGgPatch;

typedef struct {
    uint8_t  prg_banks;   /* × 16 KB */
    uint8_t  chr_banks;   /* × 8 KB  (0 = CHR RAM) */
    uint8_t  mapper;
    uint8_t  mirror;      /* RP2C02_MIRROR_* */
    bool     has_battery;
} NesCart;

typedef struct NesState {
    Mos6502  cpu;
    Rp2c02   ppu;
    const MosConfig *cfg;

    uint8_t  ram[0x800];  /* 2 KB CPU-side work RAM */

    /* Cartridge */
    NesCart  cart;
    uint8_t *prg;         /* PRG ROM (heap) */
    uint8_t *chr;         /* CHR ROM or CHR RAM (heap) */
    bool     chr_is_ram;

    /* Mapper 1 (MMC1/SxROM) */
    uint8_t  mmc1_shift;         /* 5-bit serial shift register */
    uint8_t  mmc1_shift_count;   /* bits received so far (0–4) */
    uint8_t  mmc1_ctrl;          /* internal register: $8000–$9FFF */
    uint8_t  mmc1_chr0;          /* internal register: $A000–$BFFF */
    uint8_t  mmc1_chr1;          /* internal register: $C000–$DFFF */
    uint8_t  mmc1_prg;           /* internal register: $E000–$FFFF */
    uint32_t prg_offsets[2];     /* byte offsets into prg[]: [0]=$8000, [1]=$C000 */
    uint32_t chr_offsets[2];     /* byte offsets into chr[]: [0]=PPU$0000, [1]=PPU$1000 */
    uint8_t  prg_ram[0x2000];    /* 8 KB SRAM at $6000–$7FFF (shared mapper 1/4) */

    /* Mapper 4 (MMC3/TxROM) */
    uint8_t  mmc3_bank_sel;       /* bank select: bits[2:0]=target, [6]=PRG mode, [7]=CHR inv */
    uint8_t  mmc3_regs[8];        /* R0–R7 */
    uint8_t  mmc3_irq_latch;      /* reload value for IRQ counter */
    uint8_t  mmc3_irq_counter;    /* scanline IRQ counter */
    bool     mmc3_irq_reload;     /* counter reloads from latch on next clock */
    bool     mmc3_irq_enabled;
    uint32_t mmc3_prg_offsets[4]; /* 4× 8KB windows: $8000, $A000, $C000, $E000 */
    uint32_t mmc3_chr_offsets[8]; /* 8× 1KB windows: PPU $0000–$1FFF */

    /* Mapper 5 (MMC5) */
    uint8_t  mmc5_prg_mode;           /* $5100: PRG mode 0-3 */
    uint8_t  mmc5_chr_mode;           /* $5101: CHR mode 0-3 */
    uint8_t  mmc5_exram_mode;         /* $5104: ExRAM mode 0-3 */
    uint8_t  mmc5_nt_map;             /* $5105: nametable mapping (4 × 2-bit fields) */
    uint8_t  mmc5_fill_tile;          /* $5106: fill-mode tile index */
    uint8_t  mmc5_fill_attr;          /* $5107: fill-mode palette [1:0] */
    uint8_t  mmc5_prg_regs[5];        /* $5113-$5117: PRG bank regs (index 0=$5113) */
    uint8_t  mmc5_chr_regs[12];       /* $5120-$512B: CHR bank regs (index 0=$5120) */
    uint8_t  mmc5_chr_hi;             /* $5130: upper 2 CHR bank bits */
    uint8_t  mmc5_irq_target;         /* $5203: IRQ compare scanline */
    bool     mmc5_irq_enabled;        /* $5204[7]: IRQ enable */
    bool     mmc5_irq_pending;        /* scanline IRQ pending flag */
    bool     mmc5_in_frame;           /* rendering "in frame" flag */
    uint8_t  mmc5_irq_counter;        /* current visible scanline count */
    uint8_t  mmc5_mul[2];             /* $5205-$5206: multiplier inputs */
    bool     mmc5_last_chr_bg;        /* last CHR write was to $5128-$512B set */
    uint8_t  mmc5_exattr_latch;       /* ExRAM byte for current BG tile (ext-attr) */
    uint32_t mmc5_prg_offsets[4];     /* four 8KB PRG windows for $8000-$FFFF */
    bool     mmc5_prg_is_ram[4];      /* true when window maps to PRG-RAM */
    uint8_t  mmc5_exram[0x400];       /* 1KB ExRAM at CPU $5C00-$5FFF / PPU nametable */

    /* Mapper 9 (MMC2/PxROM) */
    uint8_t  m9_prg_reg;     /* $A000: 8KB PRG bank at $8000-$9FFF (bits 3:0) */
    uint8_t  m9_chr_lfd;     /* $B000: left CHR bank when latch=FD (bits 4:0) */
    uint8_t  m9_chr_lfe;     /* $C000: left CHR bank when latch=FE */
    uint8_t  m9_chr_rfd;     /* $D000: right CHR bank when latch=FD */
    uint8_t  m9_chr_rfe;     /* $E000: right CHR bank when latch=FE */
    bool     m9_latch_l;     /* false=FD side, true=FE side */
    bool     m9_latch_r;

    /* Mapper 24/26 (Konami VRC6a/VRC6b) -- shares mmc3_prg_offsets[0-3] and mmc3_chr_offsets[0-7] */
    uint8_t  vrc6_irq_latch;
    uint8_t  vrc6_irq_counter;
    bool     vrc6_irq_enabled;     /* E: enable IRQ counting immediately */
    bool     vrc6_irq_after;       /* A: enable after next acknowledge */
    bool     vrc6_irq_mode;        /* 0=scanline (341 CPU cycles), 1=CPU cycle */
    int      vrc6_prescaler;       /* counts up to 341 for scanline mode */
    /* Expansion audio */
    uint8_t  vrc6_p1_ctrl;         /* $9000: bit7=mode, bits6:4=duty, bits3:0=vol */
    uint16_t vrc6_p1_period;       /* $9001-$9002[3:0]: 12-bit timer period */
    bool     vrc6_p1_en;           /* $9002[7] */
    uint16_t vrc6_p1_timer;
    uint8_t  vrc6_p1_seq;          /* 0-15 duty sequence position */
    uint8_t  vrc6_p2_ctrl;
    uint16_t vrc6_p2_period;
    bool     vrc6_p2_en;
    uint16_t vrc6_p2_timer;
    uint8_t  vrc6_p2_seq;
    uint8_t  vrc6_saw_rate;        /* $B000[5:0]: 6-bit accumulator rate */
    uint16_t vrc6_saw_period;
    bool     vrc6_saw_en;          /* $B002[7] */
    uint16_t vrc6_saw_timer;
    uint8_t  vrc6_saw_accum;
    uint8_t  vrc6_saw_step;        /* 0-13; resets at 14 */
    bool     vrc6_halt;            /* $9003[0]: freeze all VRC6 audio channels */

    /* Mapper 178 (WaixingFS) */
    uint8_t  m178_mode;    /* $4800: bits[2:1]=PRG mode, bit[0]=mirror */
    uint8_t  m178_prg_lo;  /* $4801: PRG A16..A14 (inner 16KB bank) */
    uint8_t  m178_prg_hi;  /* $4802: PRG A24..A17 (outer 128KB block) */

    Apu2a03  apu;
#if defined(HAVE_ALSA) || defined(HAVE_WINMIDI)
    ApuMidi  apu_midi;
#endif

    /* OAM DMA state */
    bool     dma_pending;
    uint8_t  dma_page;

    /* Controllers */
    uint8_t  ctrl_state[2]; /* live button bits */
    uint8_t  ctrl_shift[2]; /* serial shift register */
    bool     ctrl_strobe;
    bool     mic_noise;     /* FC2 microphone active (drives $4016 D2) */

    char           cart_path_buf[512]; /* path of currently loaded cartridge */
    uint64_t       ppu_synced_cpu_cycle;
    bool           battery_autosave;  /* user opted in to save SRAM on exit */
    char           sav_path[512];     /* ~/.gemu/<game>.sav or AppData equivalent */

    GemuDisplay   *display;  /* display+input handle (NULL if headless) */
    GemuVncServer *vnc;
    GemuMonitor   *monitor;

    /* Game Genie patches */
    NesGgPatch gg_patches[NES_GG_MAX];
    int        gg_count;

    /* Zapper (port 2) */
    int      zapper_x, zapper_y;      /* cursor position in NES pixel coords */
    int      zapper_trigger_ttl;       /* frames remaining in trigger pulse */

    /* Monitor key injection */
    uint8_t  ctrl_inject_mask;        /* button bits to hold */
    int      ctrl_inject_frames;      /* frames remaining (counts down to 0) */

    /* VS. System arcade */
    uint32_t vs_palette_rgb[64]; /* 2C04 palette; loaded from .pal file if present */
    uint8_t  vs_dip;          /* 8-bit DIP switch bank (SW1 1–8) */
    uint8_t  vs_coin_counter; /* last value written/read through $4020-$5fff */
    int      vs_coin_latch[2]; /* coin pulse countdown in frames; >0 = coin signal active */
    bool     vs_coin_prev[2];  /* coin key state from previous frame (edge detection) */
    bool     vs_service;       /* service button state */

    /* Famicom Disk System */
    bool     fds_enabled;
    FdsState fds;

    /* Hex editor (GTK only) */
#ifdef GEMU_GTK
    HexEditor *hex_editor;
#endif

#ifdef HAVE_ROB
    RobState    rob;
    RobWindow  *rob_window;
#endif
} NesState;

NesState *nes_create (const MosConfig *cfg);
void      nes_run    (NesState *s, const MosConfig *cfg);
void      nes_destroy(NesState *s);
