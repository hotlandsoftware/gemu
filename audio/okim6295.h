#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <SDL2/SDL.h>

/*
 * OKI MSM6295 - 4-channel ADPCM voice synthesis LSI.
 * Reference: OKI Semiconductor MSM6295 datasheet.
 *
 * Command protocol (2 bytes to start a phrase, 1 byte to stop):
 *   byte1 = 1SSSSSSS  (S = 7-bit phrase number, 1-127; 0 is invalid)
 *   byte2 = CCCC AAAA (C = one-hot channel mask bits 4-7, A = attenuation 0-8)
 *   OR a single byte  0CCCC XXXX (C = channel mask, stops those channels)
 * Status read: bits 0-3 = busy flag for channels 1-4 (1 = playing).
 *
 * ROM layout: phrase N's 8-byte address-table entry lives at offset N*8
 * (bytes 0-2 = 18-bit start address, bytes 3-5 = 18-bit end address, each
 * stored as 000000AA/AAAAAAAA/AAAAAAAA covering bits 17:16/15:8/7:0).
 * ADPCM data is 4-bit samples, 2 per byte, high nibble first.
 */

#define OKIM6295_SAMPLE_RATE 44100u
#define OKIM6295_N_CHANNELS  4

typedef struct {
    bool     playing;
    uint32_t pos, end;      /* current/end byte address in ROM */
    bool     low_nibble;    /* which nibble of *pos to decode next */
    int32_t  signal;        /* ADPCM decoder state (12-bit signed) */
    int32_t  step_index;    /* 0-48, index into the step-size table */
    int      attenuation;   /* 0 (0dB) - 8 (-24dB) */
} Okim6295Channel;

typedef struct Okim6295 {
    const uint8_t *rom;
    uint32_t       rom_size;

    Okim6295Channel ch[OKIM6295_N_CHANNELS];

    bool    cmd_pending; /* waiting for the 2nd byte of a phrase-select */
    uint8_t cmd_phrase;

    /* Timing: two nested accumulators, both driven by oki6295_tick() calls
     * at tick_clock_hz. step_period paces ADPCM decoding at the chip's own
     * (much slower) native sample rate; the decoded value is then held
     * (zero-order hold) and re-sampled up to OKIM6295_SAMPLE_RATE via
     * clock_pps, matching how the real chip's DAC output actually behaves
     * and mirroring audio/apu2a03.c's resampling approach. */
    double step_acc, step_period;
    double sample_acc, clock_pps;
    float  current_output;

    float frame_buf[1024];
    int   frame_n;

    SDL_AudioDeviceID audio_dev;
} Okim6295;

bool oki6295_init   (Okim6295 *o, uint32_t tick_clock_hz, uint32_t native_sample_rate_hz,
                     const uint8_t *rom, uint32_t rom_size);
void oki6295_destroy(Okim6295 *o);

void    oki6295_write(Okim6295 *o, uint8_t val);
uint8_t oki6295_read (Okim6295 *o);

void oki6295_tick (Okim6295 *o);  /* call once per tick_clock_hz cycle */
void oki6295_flush(Okim6295 *o);  /* queue frame samples to SDL; call once per frame */
