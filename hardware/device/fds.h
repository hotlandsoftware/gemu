#pragma once
#include <stdint.h>
#include <stdbool.h>

#define FDS_SIDE_BYTES      65500u   /* raw bytes per disk side in .fds file */
#define FDS_RAM_SIZE        0x8000u  /* 32 KB work RAM: CPU $6000–$DFFF       */
#define FDS_BIOS_SIZE       0x2000u  /* 8 KB BIOS ROM:  CPU $E000–$FFFF       */
#define FDS_CHR_SIZE        0x2000u  /* 8 KB CHR RAM:   PPU $0000–$1FFF       */
#define FDS_CYCLES_PER_BYTE 150u     /* CPU cycles between disk byte transfers */

typedef struct {
    uint8_t  bios[FDS_BIOS_SIZE];
    uint8_t  ram[FDS_RAM_SIZE];   /* CPU $6000–$DFFF */

    uint8_t *raw_disk;   /* disk_sides * FDS_SIDE_BYTES: raw .fds block data (mutable) */
    uint8_t  disk_sides;
    uint8_t  cur_side;
    bool     disk_inserted;
    uint32_t disk_change_cycles; /* transient no-disk pulse after side flip */
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
    bool     drive_ctrl_written;

    /* Disk block state machine (FCEUX-style logical block tracking) */
    uint8_t  blk_type;      /* 0=init 1=volume 2=filecnt 3=filehdr 4=filedata */
    uint32_t blk_start;     /* byte offset in raw_disk[cur_side] for block start */
    uint32_t blk_len;       /* length of current block in bytes */
    uint32_t blk_addr;      /* byte offset within current block */
    uint16_t blk_filesize;  /* file size captured from header block (for data block len) */
    uint8_t  blk_access;    /* 0 = first-write guard pending for this block */
    int32_t  disk_irq_ctr;  /* countdown to disk-transfer IRQ (-1 = inactive) */

    /* Byte received from disk during read ($4031) */
    uint8_t  read_data;

    /* Transfer flag: byte ready for CPU; cleared by $4030/$4031 read */
    bool     transfer_flag;

    /* Set when raw_disk has been modified; triggers save on quit */
    bool     dirty;

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
bool    fds_disk_load_save(FdsState *f, const char *path);
bool    fds_disk_save(FdsState *f, const char *path);
void    fds_disk_eject(FdsState *f);
bool    fds_disk_flip(FdsState *f);

/* Advance one CPU cycle.  Returns true while any IRQ condition is asserted. */
bool    fds_tick(FdsState *f);

uint8_t fds_reg_read (FdsState *f, uint16_t addr);
void    fds_reg_write(FdsState *f, uint16_t addr, uint8_t val);
