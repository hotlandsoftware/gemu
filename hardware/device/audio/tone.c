/* A gated square-wave tone as a core device: see tone.h. */
#include "audio/tone.h"
#include "gemu/dev/option.h"
#include "gemu/dev/state.h"
#include "gemu/dev/stream.h"
#include <stdio.h>
#include <string.h>

#define TONE_RATE  44100u
#define TONE_LEVEL (6000.0f / 32767.0f)

typedef struct {
    GemuStream *stream;
    uint8_t     gate;
    uint32_t    freq;
    double      phase;
} Tone;

static void tone_update(void *ud, float *const *out, unsigned n) {
    Tone *t = ud;
    if (!t->gate) {
        memset(out[0], 0, n * sizeof(float));
        return;
    }
    double step = (double)t->freq / TONE_RATE;
    for (unsigned i = 0; i < n; i++) {
        out[0][i] = t->phase < 0.5 ? TONE_LEVEL : -TONE_LEVEL;
        t->phase += step;
        while (t->phase >= 1.0) t->phase -= 1.0;
    }
}

void gemu_tone_set_gate(GemuDevice *d, bool on) {
    Tone *t = d->priv;
    if (t->gate == on) return;
    gemu_stream_update(t->stream);
    t->gate = on;
}

void gemu_tone_set_freq(GemuDevice *d, unsigned hz) {
    Tone *t = d->priv;
    if (t->freq == hz) return;
    gemu_stream_update(t->stream);
    t->freq = hz;
}

static const GemuOptionDef tone_opts[] = {
    { "freq", "pitch (Hz)", GEMU_OPT_INT, "250", 1, 20000, NULL },
    { NULL, NULL, 0, NULL, 0, 0, NULL },
};

static bool tone_init(GemuDevice *d) {
    Tone *t = d->priv;
    t->freq = (uint32_t)gemu_opt_int(d, "freq");
    bool ok = (t->stream = gemu_stream_new(d, "sound", 1, TONE_RATE, tone_update, t)) != NULL;
    ok = ok && GEMU_STATE_MEMBER(d, "", t, gate) && GEMU_STATE_MEMBER(d, "", t, freq) &&
         GEMU_STATE_MEMBER(d, "", t, phase);
    return ok;
}

static void tone_reset(GemuDevice *d, GemuResetKind kind) {
    (void)kind;
    gemu_tone_set_gate(d, false);
}

static void tone_describe(GemuDevice *d, char *buf, size_t len) {
    const Tone *t = d->priv;
    snprintf(buf, len, "%u Hz, %s", t->freq, t->gate ? "sounding" : "silent");
}

const GemuDeviceType gemu_tone_type = {
    .name = "tone", .desc = "Gated square-wave tone (beeper)", .size = sizeof(Tone),
    .init = tone_init, .reset = tone_reset, .describe = tone_describe, .options = tone_opts,
};
