/* Exercise the actual mono/stereo ring and SDL callback with a dummy sink. */
#include "../core/src/audio_out.c"
#include <assert.h>
#include <math.h>

int main(void) {
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    for (unsigned channels = 1; channels <= 2; channels++) {
        GemuAudioOut *a = gemu_audio_open_channels("test", 48000, channels);
        assert(a);
        SDL_PauseAudioDevice(a->dev, 1);
        float samples[1024], out[1024];
        for (unsigned i = 0; i < 512; i++)
            for (unsigned c = 0; c < channels; c++)
                samples[i * channels + c] = c ? -.5f : .25f;
        gemu_audio_queue(a, samples, 512 * channels);
        assert(gemu_audio_queued(a) == 512 * channels * sizeof(float));
        callback(a, (Uint8 *)out, 512 * channels * sizeof(float));
        assert(!gemu_audio_queued(a) && !gemu_audio_underruns(a));
        for (unsigned i = 128 * channels; i < 512 * channels; i++)
            assert(out[i] == samples[i]);
        callback(a, (Uint8 *)out, 512 * channels * sizeof(float));
        assert(gemu_audio_underruns(a) == 1);
        assert(out[0] > 0 && out[510 * channels] < out[0]);
        if (channels == 2) assert(out[1] < 0 && out[1021] > out[1]);
        gemu_audio_queue(a, samples, 512 * channels);
        gemu_audio_flush(a);
        assert(!gemu_audio_queued(a));
        callback(a, (Uint8 *)out, sizeof out);
        for (unsigned i = 0; i < 1024; i++) assert(out[i] == 0);
        if (channels == 2) {
            gemu_audio_queue(a, samples, 3);
            assert(gemu_audio_queued(a) == 2 * sizeof(float));
        }
        gemu_audio_close(a);
    }
    puts("SDL audio: mono/stereo channel separation, fade, underrun, flush and whole frames passed");
    return 0;
}
