/*
 * Sound streams and the mixer.  (.work/modular-devices, 2.6)
 *
 * A device that makes sound has a stream: a sample rate, 1 or 2 output
 * channels and an update callback that writes the next n samples (floats,
 * -1..1).  Before changing anything audible (a register write, a line's
 * edge) the device calls gemu_stream_update(), which brings the stream up
 * to gemu_now() with the old settings - so sound follows emulated time to
 * the sample, as MAME's sound_stream::update() does.
 *
 * Each machine has a mixer.  gemu_mixer_run() (once a frame, say) updates
 * every stream to now, resamples each to the output rate - a box average
 * when the stream's rate is higher (square waves and speaker PWM come out
 * right), the nearest earlier sample when lower - sums them, and hands the
 * result to the sink (a GemuAudioOut, typically) and to a WAV capture if
 * one is running.  With neither, the samples are dropped: streams never
 * pile up.
 *
 * Streams and the mixer keep no save state: a load, time going backwards
 * or a gap of more than a second resyncs them to now.  What makes the
 * sound is the devices' state, so after a load the sound is what it was -
 * but for the one output sample the load falls inside, whose average lacks
 * the input samples from before the load.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "gemu/dev/device.h"

typedef struct GemuStream GemuStream;

/* outputs: 1 or 2.  NULL on a bad argument or a duplicate name. */
GemuStream *gemu_stream_new(GemuDevice *d, const char *name, unsigned outputs, unsigned rate,
        void (*update)(void *ud, float *const *out, unsigned n), void *ud);
void        gemu_stream_update(GemuStream *s);
void        gemu_stream_set_gain(GemuStream *s, float gain);   /* default 1 */
unsigned    gemu_stream_rate(const GemuStream *s);
const char *gemu_stream_name(const GemuStream *s);

/* The mixer: output rate and channels (1 or 2), and where samples go
 * (interleaved; NULL for nowhere). */
void gemu_mixer_set_output(GemuMachine *m, unsigned rate, unsigned channels,
                           void (*sink)(void *ud, const float *samples, size_t frames), void *ud);
void gemu_mixer_run(GemuMachine *m);
/* Record the mixer's output to a 16-bit PCM WAV file (false: can't open
 * it, or one is running already); stop and finish the file. */
bool gemu_mixer_wav_start(GemuMachine *m, const char *path);
bool gemu_mixer_wav_stop(GemuMachine *m);
bool gemu_mixer_wav_running(const GemuMachine *m);
