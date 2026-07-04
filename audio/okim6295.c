#include "okim6295.h"
#include <stdio.h>
#include <string.h>

/* Standard OKI/Dialogic ADPCM tables (the same decode algorithm used by the
 * MSM5205/MSM6295 family; not printed in the datasheet itself, but a fixed,
 * universally-reproduced public algorithm). */
static const int8_t  index_shift[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
static const int16_t step_size[49] = {
    16,   17,   19,   21,   23,   25,   28,   31,   34,   37,
    41,   45,   50,   55,   60,   66,   73,   80,   88,   97,
    107,  118,  130,  143,  157,  173,  190,  209,  230,  253,
    279,  307,  337,  371,  408,  449,  494,  544,  598,  658,
    724,  796,  876,  963, 1060, 1166, 1282, 1411, 1552,
};

/* Reduction/attenuation table from the datasheet, 0dB..-24dB in ~3dB steps,
 * precomputed as linear gain (10^(dB/20)) to avoid a libm dependency. */
static const float atten_gain[9] = {
    1.0000f, 0.6918f, 0.5012f, 0.3467f, 0.2512f,
    0.1884f, 0.1259f, 0.0944f, 0.0631f,
};

static int16_t adpcm_decode(Okim6295Channel *c, uint8_t nibble) {
    int step_val = step_size[c->step_index];
    int diff = step_val >> 3;
    if (nibble & 4) diff += step_val;
    if (nibble & 2) diff += step_val >> 1;
    if (nibble & 1) diff += step_val >> 2;
    if (nibble & 8) c->signal -= diff; else c->signal += diff;
    if (c->signal > 2047) c->signal = 2047;
    if (c->signal < -2048) c->signal = -2048;
    c->step_index += index_shift[nibble & 7];
    if (c->step_index < 0) c->step_index = 0;
    if (c->step_index > 48) c->step_index = 48;
    return (int16_t)c->signal;
}

static void start_channel(Okim6295 *o, int idx, uint8_t phrase, int atten) {
    uint32_t base = (uint32_t)phrase * 8u;
    if (base + 6 > o->rom_size) return; /* phrase table entry out of range */
    const uint8_t *e = o->rom + base;
    uint32_t sa = ((uint32_t)(e[0] & 0x03) << 16) | ((uint32_t)e[1] << 8) | e[2];
    uint32_t ea = ((uint32_t)(e[3] & 0x03) << 16) | ((uint32_t)e[4] << 8) | e[5];

    Okim6295Channel *c = &o->ch[idx];
    if (sa > ea || ea >= o->rom_size) { c->playing = false; c->signal = 0; return; }
    c->pos          = sa;
    c->end          = ea;
    c->low_nibble   = false;
    c->signal       = 0;
    c->step_index   = 0;
    c->attenuation  = (atten < 0) ? 0 : (atten > 8 ? 8 : atten);
    c->playing      = true;
}

static void advance_channel(Okim6295Channel *c, const uint8_t *rom, uint32_t rom_size) {
    if (!c->playing) return;
    if (c->pos >= rom_size) { c->playing = false; c->signal = 0; return; }
    uint8_t byte = rom[c->pos];
    uint8_t nibble = c->low_nibble ? (byte & 0x0Fu) : (byte >> 4);
    adpcm_decode(c, nibble);
    if (c->low_nibble) {
        if (c->pos >= c->end) { c->playing = false; c->signal = 0; return; }
        c->pos++;
    }
    c->low_nibble = !c->low_nibble;
}

/* ── Command protocol ────────────────────────────────────────────────────── */

void oki6295_write(Okim6295 *o, uint8_t val) {
    if (o->cmd_pending) {
        o->cmd_pending = false;
        int atten = val & 0x0F;
        for (int c = 0; c < OKIM6295_N_CHANNELS; c++)
            if (val & (0x10u << c))
                start_channel(o, c, o->cmd_phrase, atten);
        return;
    }
    if (val & 0x80) {
        uint8_t phrase = val & 0x7F;
        if (phrase != 0) { /* phrase 0 ("all zero") is invalid, per datasheet */
            o->cmd_phrase  = phrase;
            o->cmd_pending = true;
        }
        return;
    }
    /* Single-byte stop: bits 4-7 select which channels to silence. */
    for (int c = 0; c < OKIM6295_N_CHANNELS; c++) {
        if (val & (0x10u << c)) {
            o->ch[c].playing = false;
            o->ch[c].signal  = 0;
        }
    }
}

uint8_t oki6295_read(Okim6295 *o) {
    uint8_t v = 0;
    for (int c = 0; c < OKIM6295_N_CHANNELS; c++)
        if (o->ch[c].playing) v |= (uint8_t)(1u << c);
    return v;
}

/* ── Timing / sample generation ─────────────────────────────────────────── */

void oki6295_tick(Okim6295 *o) {
    o->step_acc += 1.0;
    if (o->step_acc >= o->step_period) {
        o->step_acc -= o->step_period;

        float mix = 0.0f;
        for (int c = 0; c < OKIM6295_N_CHANNELS; c++) {
            advance_channel(&o->ch[c], o->rom, o->rom_size);
            mix += ((float)o->ch[c].signal / 2048.0f) * atten_gain[o->ch[c].attenuation] * 0.25f;
        }
        o->current_output = mix;
    }

    o->sample_acc += 1.0;
    if (o->sample_acc >= o->clock_pps) {
        o->sample_acc -= o->clock_pps;
        if (o->frame_n < 1024)
            o->frame_buf[o->frame_n++] = o->current_output;
    }
}

void oki6295_flush(Okim6295 *o) {
    if (!o->audio_dev || o->frame_n == 0) {
        o->frame_n = 0;
        return;
    }
    SDL_QueueAudio(o->audio_dev, o->frame_buf, (Uint32)(o->frame_n * (int)sizeof(float)));
    o->frame_n = 0;
}

/* ── Init / destroy ──────────────────────────────────────────────────────── */

bool oki6295_init(Okim6295 *o, uint32_t tick_clock_hz, uint32_t native_sample_rate_hz,
                  const uint8_t *rom, uint32_t rom_size) {
    memset(o, 0, sizeof(*o));
    o->rom          = rom;
    o->rom_size     = rom_size;
    o->step_period  = (double)tick_clock_hz / (double)native_sample_rate_hz;
    o->clock_pps    = (double)tick_clock_hz / (double)OKIM6295_SAMPLE_RATE;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "oki6295: SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec want = {
        .freq     = OKIM6295_SAMPLE_RATE,
        .format   = AUDIO_F32SYS,
        .channels = 1,
        .samples  = 512,
    };
    SDL_AudioSpec have;
    o->audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!o->audio_dev) {
        fprintf(stderr, "oki6295: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    SDL_PauseAudioDevice(o->audio_dev, 0);
    return true;
}

void oki6295_destroy(Okim6295 *o) {
    if (o->audio_dev) {
        SDL_CloseAudioDevice(o->audio_dev);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        o->audio_dev = 0;
    }
}
