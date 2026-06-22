#pragma once
#include <stdint.h>
#include <stdbool.h>

#define FDS_SIDE_BYTES      65500u   /* raw bytes per disk side in .fds file */
#define FDS_RAM_SIZE        0x8000u  /* 32 KB work RAM: CPU $6000–$DFFF       */
#define FDS_BIOS_SIZE       0x2000u  /* 8 KB BIOS ROM:  CPU $E000–$FFFF       */
#define FDS_CHR_SIZE        0x2000u  /* 8 KB CHR RAM:   PPU $0000–$1FFF       */
#define FDS_CYCLES_PER_BYTE 150u     /* CPU cycles between disk byte transfers */
#define FDS_MASK_BYTES      ((FDS_SIDE_BYTES + 7u) / 8u) /* 1 bit per phys byte */

typedef struct {
    uint8_t  bios[FDS_BIOS_SIZE];
    uint8_t  ram[FDS_RAM_SIZE];   /* CPU $6000–$DFFF */

    uint8_t *disk;        /* disk_sides * FDS_SIDE_BYTES: physical byte stream  */
    uint8_t *fwd_mask;    /* disk_sides * FDS_MASK_BYTES: 1=forward to CPU     */
    uint8_t *raw_disk;    /* disk_sides * FDS_SIDE_BYTES: raw .fds block data  */
    uint8_t  disk_sides;
    uint8_t  cur_side;
    bool     disk_inserted;
    bool     hle_mode;    /* true = no BIOS ROM, stub installed, files pre-loaded */

    /* Timer IRQ — $4020/$4021 latch, $4022 control */
    uint16_t timer_latch;
    uint16_t timer_ctr;
    bool     timer_enabled;
    bool     timer_repeat;
    bool     timer_pending;

    /* $4023 master I/O enable (bit 0) */
    bool     disk_io_en;

    /* $4025 drive control register (stored verbatim) */
    uint8_t  drive_ctrl;

    /* Disk transfer state */
    uint32_t disk_pos;       /* current byte offset within the current side */
    uint32_t cyc_acc;        /* cycles accumulated toward next byte */
    uint8_t  read_data;      /* last byte received from disk ($4031) */
    bool     transfer_flag;  /* byte ready; cleared by $4031 or $4030 read */
    bool     end_of_head;    /* disk_pos reached end of side */

    /* ---- FDS wavetable synthesizer ($4040–$408A) ---- */
    uint8_t  snd_wav[64];    /* 6-bit waveform table ($4040–$407F) */
    uint8_t  snd_mod[64];    /* 3-bit modulation table (written via $4088) */

    /* Volume envelope ($4080) */
    bool     snd_vol_dis;    /* bit7: disable envelope (direct volume) */
    bool     snd_vol_grow;   /* bit6: 1=grow, 0=decay */
    uint8_t  snd_vol_spd;    /* bits5-0: envelope speed / direct level */
    uint8_t  snd_vol;        /* current output volume (0–32; capped at 32) */
    uint8_t  snd_vol_div;    /* volume envelope sub-divider */

    /* Main oscillator ($4082–$4083) */
    bool     snd_wav_halt;   /* $4083 bit7: halt oscillator and all envelopes */
    bool     snd_env_halt;   /* $4083 bit6: halt envelopes only */
    uint16_t snd_freq;       /* 12-bit main frequency */
    uint32_t snd_phase;      /* 20-bit main oscillator phase accumulator */

    /* Modulation unit ($4084–$4087) */
    bool     snd_mod_dis;    /* $4087 bit7: disable modulation */
    bool     snd_gain_dis;   /* $4084 bit7: disable gain envelope (direct gain) */
    bool     snd_gain_grow;  /* $4084 bit6 */
    uint8_t  snd_gain_spd;   /* $4084 bits5-0: gain envelope speed / direct level */
    uint8_t  snd_gain;       /* current modulation gain (0–63) */
    uint8_t  snd_gain_div;   /* gain envelope sub-divider */
    uint16_t snd_mod_freq;   /* 12-bit modulation frequency */
    uint32_t snd_mod_phase;  /* 20-bit modulation phase accumulator */
    int8_t   snd_mod_cnt;    /* signed modulation counter (–64..+63) */

    /* $4089 */
    bool     snd_wav_write;  /* bit7: wave write enable (mutes output) */
    uint8_t  snd_master_vol; /* bits1-0: master volume (0=full, 1=2/3, 2=1/2, 3=2/5) */

    /* $408A: master envelope clock */
    uint8_t  snd_env_spd;    /* master envelope speed */
    uint32_t snd_env_div;    /* master envelope clock accumulator */
} FdsState;

float   fds_audio_tick(FdsState *f);   /* advance synthesizer one CPU cycle; returns sample */

bool    fds_bios_load(FdsState *f, const char *path);
bool    fds_disk_load(FdsState *f, const char *path);
void    fds_disk_eject(FdsState *f);

/* Advance one CPU cycle.  Returns true while any IRQ condition is asserted. */
bool    fds_tick(FdsState *f);

uint8_t fds_reg_read (FdsState *f, uint16_t addr);
void    fds_reg_write(FdsState *f, uint16_t addr, uint8_t val);
