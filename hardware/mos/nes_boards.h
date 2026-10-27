#pragma once
/* NES cartridge boards in nes_boards.c - the simpler discrete-logic and ASIC
 * mappers.  They all bank through NesState's fine windows (mmc3_prg_offsets:
 * 4 x 8 KB, mmc3_chr_offsets: 8 x 1 KB), so machine_nes.c reads PRG and CHR
 * for them the same way it does for MMC3; the board code only decides where
 * the windows point.
 *
 *   11 Color Dreams       16 Bandai FCG/LZ93D50   19 Namco 163
 *   23 Konami VRC2b/VRC4  33 Taito TC0190         64 Tengen RAMBO-1
 *   69 Sunsoft FME-7      70 Bandai 74161         71 Camerica
 *   78 Irem/Jaleco 74161  79 AVE NINA-03/06       87 Jaleco/Konami CHR
 *   95 Namco 3425         112 NTDEC/Asder         113 NINA-03/06 multicart
 *   132 TXC 22211         150 Sachen SA-015       180 UNROM (fixed $8000)
 *   184 Sunsoft-1         185 CNROM + CHR protect 206 Namco 108/118
 *   243 Sachen SA-020A
 */
#include <stdbool.h>
#include <stdint.h>

struct NesState;

/* Board registers; what each field means depends on the mapper. */
typedef struct {
    uint8_t  regs[16];
    uint8_t  cmd;            /* bank/command select latch */
    uint16_t chr[8];         /* 1 KB CHR bank numbers (VRC: 9-bit) */
    uint8_t  nt[4];          /* nametable per quadrant: bit 0 = CIRAM page;
                                Namco 163: values below $E0 are CHR ROM banks */
    bool     ticks;          /* nes_board_tick() wants every CPU cycle */
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

bool    nes_board_supported(unsigned mapper);
void    nes_board_reset(struct NesState *s);
/* A CPU write at $4020-$FFFF; false if the board ignores the address. */
bool    nes_board_write(struct NesState *s, uint16_t addr, uint8_t val);
/* A CPU read at $4020-$7FFF; false if the board doesn't drive the bus. */
bool    nes_board_read(struct NesState *s, uint16_t addr, uint8_t *val);
/* True if $6000-$7FFF is plain PRG RAM on this board. */
bool    nes_board_has_prg_ram(const struct NesState *s);
void    nes_board_tick(struct NesState *s, unsigned cycles);
/* PPU hooks (installed by machine_nes.c when the board needs them). */
void    nes_board_scanline(void *ud);
uint8_t nes_board_nt_read(uint16_t addr, void *ud);
void    nes_board_nt_write(uint16_t addr, uint8_t val, void *ud);
bool    nes_board_wants_scanline(unsigned mapper);
bool    nes_board_wants_nt(unsigned mapper);   /* uses nt[] via the nt hooks */
