#include "pcspk.h"
#include <SDL2/SDL.h>
#include <stdlib.h>

#define PCSPK_SAMPLE_RATE 44100
#define PCSPK_BUFFER_SAMPLES 512
#define PCSPK_AMPLITUDE 6000

struct RcaPcSpeaker {
    SDL_AudioDeviceID dev;
    int sample_rate;
    unsigned frequency_hz;
    double phase;
    uint8_t gate;
    bool active;
};

static void pcspk_audio(void *ud, Uint8 *stream, int len) {
    RcaPcSpeaker *spk = ud;
    int16_t *out = (int16_t *)stream;
    int samples = len / (int)sizeof(*out);
    double step = (double)spk->frequency_hz / (double)spk->sample_rate;

    for (int i = 0; i < samples; i++) {
        int16_t sample = 0;
        if (spk->gate) {
            sample = (spk->phase < 0.5) ? PCSPK_AMPLITUDE : -PCSPK_AMPLITUDE;
            spk->phase += step;
            while (spk->phase >= 1.0)
                spk->phase -= 1.0;
        }
        out[i] = sample;
    }
}

RcaPcSpeaker *rca_pcspk_create(unsigned frequency_hz) {
    RcaPcSpeaker *spk = calloc(1, sizeof(*spk));
    if (!spk)
        return NULL;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        free(spk);
        return NULL;
    }

    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq = PCSPK_SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = PCSPK_BUFFER_SAMPLES;
    want.callback = pcspk_audio;
    want.userdata = spk;

    SDL_AudioSpec have;
    spk->dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!spk->dev) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        free(spk);
        return NULL;
    }

    spk->sample_rate = have.freq;
    spk->frequency_hz = frequency_hz ? frequency_hz : 250u;
    spk->active = true;
    SDL_PauseAudioDevice(spk->dev, 0);
    return spk;
}

void rca_pcspk_destroy(RcaPcSpeaker *spk) {
    if (!spk)
        return;
    if (spk->dev)
        SDL_CloseAudioDevice(spk->dev);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    free(spk);
}

void rca_pcspk_set_gate(RcaPcSpeaker *spk, uint8_t gate) {
    if (!spk || !spk->dev)
        return;
    SDL_LockAudioDevice(spk->dev);
    spk->gate = gate ? 1u : 0u;
    SDL_UnlockAudioDevice(spk->dev);
}

void rca_pcspk_set_freq(RcaPcSpeaker *spk, unsigned frequency_hz) {
    if (!spk || !spk->dev)
        return;
    SDL_LockAudioDevice(spk->dev);
    spk->frequency_hz = frequency_hz ? frequency_hz : 1u;
    SDL_UnlockAudioDevice(spk->dev);
}

bool rca_pcspk_is_active(const RcaPcSpeaker *spk) {
    return spk && spk->active;
}
