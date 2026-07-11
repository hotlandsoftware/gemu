#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>

#define APU2A03_SAMPLE_RATE  44100u
#define APU2A03_CPU_CLOCK    1789773u  /* NTSC */

typedef struct {
    uint8_t  duty, seq;
    bool     halt, const_vol;
    uint8_t  vol;
    bool     sweep_en, sweep_neg, sweep_reload;
    uint8_t  sweep_period, sweep_shift, sweep_div;
    uint16_t timer_period, timer;
    uint8_t  len;
    uint8_t  env_div, env_dec;
    bool     env_start;
} ApuPulse;

typedef struct {
    bool     halt;
    uint8_t  lin_load, lin;
    bool     lin_reload;
    uint16_t timer_period, timer;
    uint8_t  seq, len;
} ApuTri;

typedef struct {
    bool     halt, const_vol;
    uint8_t  vol;
    bool     mode;
    uint16_t period, timer;
    uint16_t lfsr;
    uint8_t  len;
    uint8_t  env_div, env_dec;
    bool     env_start;
} ApuNoise;

typedef struct {
    bool     irq_en, loop;
    uint8_t  rate_idx;
    uint8_t  level;               /* 7-bit output DAC */
    uint16_t start_addr, start_len;
    uint16_t cur_addr, bytes_rem;
    uint8_t  sample_buf;
    bool     buf_empty;
    uint8_t  shift, bits_rem;
    bool     silence;
    uint16_t period, timer;
    bool     irq_flag;
} ApuDmc;

typedef struct Apu2a03 {
    ApuPulse pulse[2];
    ApuTri   tri;
    ApuNoise noise;
    ApuDmc   dmc;

    bool     ch_en[5];            /* channel enables (written by 0x4015) */

    /* Frame counter */
    uint8_t  fc_mode;             /* 0=4-step, 1=5-step */
    bool     fc_irq_inhibit, fc_irq;
    uint32_t fc_div;
    uint8_t  fc_step;

    /* Timing */
    uint64_t cycle;               /* total CPU cycles elapsed */
    double   clock_pps;            /* cycles per output sample (CPU_HZ / SAMPLE_RATE) */
    bool     apu_clk;             /* toggles each CPU cycle */

    /* Sample generation */
    double   sample_acc;
    float    frame_buf[1024];
    int      frame_n;

    /* SDL audio */
    SDL_AudioDeviceID audio_dev;

    /* Expansion audio: written before each tick by the host machine */
    float    fds_in;

    /* Compatibility switches */
    bool     sweep_enabled;
    bool     dmc_enabled;
    bool     frame_irq_enabled;

    /* DMC memory reader (set by NES machine) */
    uint8_t (*mem_read)(uint16_t addr, void *ud);
    void    *mem_ud;

    /* Optional tap: called after every register write (NULL = disabled) */
    void (*write_tap)(uint16_t addr, uint8_t val, void *ud);
    void  *write_tap_ud;
} Apu2a03;

bool    apu2a03_init   (Apu2a03 *a, uint32_t cpu_clock_hz);   /* opens SDL audio; returns false on failure */
void    apu2a03_reset  (Apu2a03 *a);
void    apu2a03_destroy(Apu2a03 *a);

void    apu2a03_write  (Apu2a03 *a, uint16_t addr, uint8_t val);
uint8_t apu2a03_read   (Apu2a03 *a, uint16_t addr);

void    apu2a03_tick   (Apu2a03 *a);   /* call once per CPU cycle */
void    apu2a03_flush  (Apu2a03 *a);   /* queue frame samples to SDL; call once per frame */
