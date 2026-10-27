#pragma once
/*
 * TIA sound - the Atari 2600's two audio channels.
 *
 * Each channel has a 5-bit frequency divider (AUDF), a 4-bit control
 * register selecting how a 4-bit and a 5-bit polynomial counter combine
 * (AUDC: pure tones, buzzes, noise) and a 4-bit volume (AUDV).  The model
 * follows the gate-level analysis of the chip (as used by Stella): the TIA
 * clocks both channels in two phases twice per scanline, giving one output
 * sample per channel at ~31.4 kHz, which is resampled here for SDL.
 */
#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdint.h>

#define TIA_SOUND_RATE  44100
#define TIA_SOUND_BUF   4096

typedef struct {
    uint8_t audc, audf, audv;
    uint8_t div_counter;      /* counts up to AUDF */
    uint8_t noise_counter;    /* 5-bit polynomial */
    uint8_t pulse_counter;    /* 4-bit polynomial */
    bool    clock_enable;
    bool    noise_feedback;
    bool    noise_bit4;
    bool    pulse_hold;
} TiaChannel;

typedef struct {
    TiaChannel ch[2];
    uint8_t    counter;       /* audio clock position, 0-227 (free-running) */
    uint32_t   sum[2], sum_n; /* volumes summed every color clock since the last sample */

    /* Resampler: box-filter the ~31.4 kHz TIA samples down to TIA_SOUND_RATE. */
    double     tia_rate;      /* TIA samples per second (2 per scanline) */
    double     phase;         /* fraction of an output sample accumulated */
    double     acc;
    double     dc_x, dc_y;    /* DC-blocking high-pass state */
    float      buf[TIA_SOUND_BUF];
    int        buf_n;

    SDL_AudioDeviceID dev;
} TiaSound;

/* tia_rate: TIA audio samples per second (color clock / 114).  Opens an SDL
 * audio device when want_output; otherwise samples are discarded. */
bool tia_sound_init(TiaSound *t, double tia_rate, bool want_output);
void tia_sound_reset(TiaSound *t);         /* registers and counters; keeps the device */
void tia_sound_destroy(TiaSound *t);
void tia_sound_write(TiaSound *t, uint8_t reg, uint8_t val);   /* reg: 0x15-0x1A */
/* One color clock: phase 0 after clocks 9 and 81, phase 1 (and an output
 * sample, the average of the volumes since the last one) after 37 and 149. */
void tia_sound_clock(TiaSound *t);
void tia_sound_flush(TiaSound *t);        /* queue buffered samples to SDL */
uint32_t tia_sound_queued(const TiaSound *t);   /* bytes waiting in SDL's queue */
