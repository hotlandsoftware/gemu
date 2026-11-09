/* Audio output with fade-out on underrun - see gemu/audio_out.h.
 *
 * One producer (the emulator thread, gemu_audio_queue) and one consumer
 * (SDL's audio callback) share a ring with free-running indices, so
 * neither ever waits for the other. */
#include "gemu/audio_out.h"
#include <SDL2/SDL.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define RING (1u << 15)   /* samples: 0.7 s at 44.1 kHz, far above any run loop's queue */

struct GemuAudioOut {
    SDL_AudioDeviceID dev;
    float             ring[RING];
    atomic_uint       r, w;            /* free-running: w - r samples are waiting */
    atomic_uint       underruns;
    /* the callback's own */
    float             last;            /* the last sample played */
    float             tail;            /* the level faded out to, crossfaded from on resume */
    float             gain;            /* the stream's share of the mix: 0 after an underrun, ramping to 1 */
    float             fade, ramp;      /* per-sample decay and ramp step */
    bool              starved;
};

static void callback(void *ud, Uint8 *stream, int len) {
    GemuAudioOut *a = ud;
    float *out = (float *)stream;
    int n = len / (int)sizeof(float);
    unsigned r = atomic_load_explicit(&a->r, memory_order_relaxed);
    unsigned w = atomic_load_explicit(&a->w, memory_order_acquire);
    for (int i = 0; i < n; i++) {
        float s;
        if (r != w) {
            s = a->ring[r++ & (RING - 1)];
            a->starved = false;
            if (a->gain < 1.0f) {   /* coming back: crossfade from the faded level */
                a->tail *= a->fade;
                a->gain += a->ramp;
                if (a->gain > 1.0f) a->gain = 1.0f;
                s = s * a->gain + a->tail * (1.0f - a->gain);
            }
        } else {
            if (!a->starved) {
                a->starved = true;
                atomic_fetch_add_explicit(&a->underruns, 1, memory_order_relaxed);
            }
            s = a->last * a->fade;
            a->tail = s;
            a->gain = 0.0f;
        }
        out[i] = s;
        a->last = s;
    }
    atomic_store_explicit(&a->r, r, memory_order_release);
}

GemuAudioOut *gemu_audio_open(const char *who, int rate) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "%s: SDL audio init failed: %s\n", who, SDL_GetError());
        return NULL;
    }
    GemuAudioOut *a = calloc(1, sizeof(*a));
    if (!a) { SDL_QuitSubSystem(SDL_INIT_AUDIO); return NULL; }
    /* Fade with a ~3 ms time constant; crossfade back in over ~2 ms. */
    a->fade = 1.0f - 1.0f / (0.003f * (float)rate);
    a->ramp = 1.0f / (0.002f * (float)rate);
    a->starved = true;   /* the start isn't an underrun; it ramps in like one */
    SDL_AudioSpec want = {
        .freq = rate, .format = AUDIO_F32SYS, .channels = 1, .samples = 512,
        .callback = callback, .userdata = a,
    }, have;
    a->dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!a->dev) {
        fprintf(stderr, "%s: SDL_OpenAudioDevice failed: %s\n", who, SDL_GetError());
        free(a);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return NULL;
    }
    SDL_PauseAudioDevice(a->dev, 0);
    return a;
}

void gemu_audio_close(GemuAudioOut *a) {
    if (!a) return;
    SDL_CloseAudioDevice(a->dev);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    free(a);
}

void gemu_audio_queue(GemuAudioOut *a, const float *samples, size_t n) {
    if (!a) return;
    unsigned w = atomic_load_explicit(&a->w, memory_order_relaxed);
    unsigned r = atomic_load_explicit(&a->r, memory_order_acquire);
    size_t room = RING - (w - r);
    if (n > room) n = room;   /* full: a run loop that isn't pacing - drop the rest */
    for (size_t i = 0; i < n; i++) a->ring[w++ & (RING - 1)] = samples[i];
    atomic_store_explicit(&a->w, w, memory_order_release);
}

uint32_t gemu_audio_queued(const GemuAudioOut *a) {
    if (!a) return 0;
    unsigned w = atomic_load_explicit(&((GemuAudioOut *)a)->w, memory_order_relaxed);
    unsigned r = atomic_load_explicit(&((GemuAudioOut *)a)->r, memory_order_acquire);
    return (uint32_t)((w - r) * sizeof(float));
}

uint32_t gemu_audio_underruns(const GemuAudioOut *a) {
    return a ? atomic_load_explicit(&((GemuAudioOut *)a)->underruns, memory_order_relaxed) : 0;
}
