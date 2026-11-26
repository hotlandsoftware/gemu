/* The PC's speaker as a core device: see pc_speaker.h. */
#include "audio/pc_speaker.h"
#include "gemu/dev/line.h"
#include "gemu/dev/option.h"
#include "gemu/dev/state.h"
#include "gemu/dev/stream.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Driven high, the cone's peak-to-peak matches the old tone generator's
 * square wave (+-6000 of 32767) once the DC is filtered out. */
#define SPEAKER_LEVEL 0.366f
#define DC_CUTOFF_HZ  20.0

typedef struct {
    GemuStream *stream;
    GemuLineIn *timer, *data;
    bool        timer_in, data_in;
    float       x1, y1;         /* the DC blocker */
    float       r;
} Speaker;

static void speaker_update(void *ud, float *const *out, unsigned n) {
    Speaker *p = ud;
    float x = p->timer_in && p->data_in ? SPEAKER_LEVEL : 0.0f;
    if (x == p->x1 && p->y1 == 0.0f) {             /* still and settled: silence */
        memset(out[0], 0, n * sizeof(float));
        return;
    }
    for (unsigned i = 0; i < n; i++) {
        float y = x - p->x1 + p->r * p->y1;
        /* a steady cone decays towards 0: stop before the denormals, which
         * are many times slower to compute with */
        if (fabsf(y) < 1e-9f) y = 0.0f;
        p->x1 = x;
        p->y1 = y;
        out[0][i] = y;
    }
}

static void input(void *ud, int idx, bool level) {
    Speaker *p = ud;
    bool was = p->timer_in && p->data_in;
    bool timer = idx == 0 ? level : p->timer_in, data = idx == 1 ? level : p->data_in;
    /* The cone only moves when timer AND data does: with PB1 low the PIT
     * can run as fast as it likes without costing anything here. */
    if ((timer && data) != was)
        gemu_stream_update(p->stream);  /* up to this edge with the old level */
    p->timer_in = timer;
    p->data_in = data;
}

static const GemuOptionDef speaker_opts[] = {
    { "rate", "the stream's sample rate, Hz", GEMU_OPT_INT, "192000", 8000, 1000000, NULL },
    { NULL, NULL, 0, NULL, 0, 0, NULL },
};

static bool speaker_init(GemuDevice *d) {
    Speaker *p = d->priv;
    unsigned rate = (unsigned)gemu_opt_int(d, "rate");
    p->r = (float)(1.0 - 2.0 * 3.14159265358979 * DC_CUTOFF_HZ / rate);
    if (!(p->stream = gemu_stream_new(d, "out", 1, rate, speaker_update, p)) ||
        !(p->timer = gemu_line_in(d, "timer", input, p, 0)) ||
        !(p->data = gemu_line_in(d, "data", input, p, 1)))
        return false;
    GEMU_STATE_VAR(d, "timer", p->timer_in);
    GEMU_STATE_VAR(d, "data", p->data_in);
    GEMU_STATE_VAR(d, "x1", p->x1);
    GEMU_STATE_VAR(d, "y1", p->y1);
    return true;
}

static void speaker_reset(GemuDevice *d, GemuResetKind kind) {
    (void)kind;
    Speaker *p = d->priv;
    gemu_stream_update(p->stream);
    p->timer_in = gemu_line_in_level(p->timer);
    p->data_in = gemu_line_in_level(p->data);
}

static void speaker_describe(GemuDevice *d, char *buf, size_t len) {
    const Speaker *p = d->priv;
    snprintf(buf, len, "timer %s, data %s: cone %s", p->timer_in ? "high" : "low",
             p->data_in ? "high" : "low", p->timer_in && p->data_in ? "out" : "in");
}

const GemuDeviceType gemu_pc_speaker_type = {
    .name = "pc-speaker", .desc = "IBM PC speaker", .size = sizeof(Speaker),
    .options = speaker_opts, .init = speaker_init, .reset = speaker_reset,
    .describe = speaker_describe,
};
