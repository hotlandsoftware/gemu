/* Sound streams and the mixer: see gemu/dev/stream.h. */
#include "dev_priv.h"
#include "gemu/dev/stream.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned __int128 u128;

struct GemuStream {
    GemuMachine *m;
    GemuDevice  *dev;
    char         name[32];
    unsigned     outputs, rate;
    void       (*update)(void *ud, float *const *out, unsigned n);
    void        *ud;
    float        gain;
    uint64_t     produced;      /* samples made up to here (an absolute index) */
    uint64_t     consumed;      /* buf[c][0] is this sample */
    float       *buf[2];
    size_t       len, cap;      /* samples [consumed, produced) */
    GemuStream  *next;
};

static void drop_front(GemuStream *s, size_t n) {
    if (n >= s->len) {
        s->consumed += s->len;
        s->len = 0;
        return;
    }
    for (unsigned c = 0; c < s->outputs; c++)
        memmove(s->buf[c], s->buf[c] + n, (s->len - n) * sizeof(float));
    s->len -= n;
    s->consumed += n;
}

static bool reserve(GemuStream *s, size_t need) {
    if (need <= s->cap)
        return true;
    size_t cap = s->cap ? s->cap : 4096;
    while (cap < need)
        cap *= 2;
    for (unsigned c = 0; c < s->outputs; c++) {
        float *b = realloc(s->buf[c], cap * sizeof(float));
        if (!b)
            return false;
        s->buf[c] = b;
    }
    s->cap = cap;
    return true;
}

static void resync(GemuStream *s, uint64_t at) {
    s->produced = s->consumed = at;
    s->len = 0;
}

GemuStream *gemu_stream_new(GemuDevice *d, const char *name, unsigned outputs, unsigned rate,
        void (*update)(void *ud, float *const *out, unsigned n), void *ud) {
    if (!d || !name || !*name || strlen(name) >= 32 || (outputs != 1 && outputs != 2) || !rate ||
        !update)
        return NULL;
    GemuMachine *m = d->machine;
    for (GemuStream *x = m->streams; x; x = x->next)
        if (x->dev == d && !strcmp(x->name, name))
            return NULL;
    GemuStream *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->m = m;
    s->dev = d;
    strcpy(s->name, name);
    s->outputs = outputs;
    s->rate = rate;
    s->update = update;
    s->ud = ud;
    s->gain = 1.0f;
    resync(s, gemu_cycles_in(rate, gemu_now(m)));
    GemuStream **pp = &m->streams;
    while (*pp)
        pp = &(*pp)->next;
    *pp = s;
    return s;
}

unsigned gemu_stream_rate(const GemuStream *s) { return s->rate; }
const char *gemu_stream_name(const GemuStream *s) { return s->name; }
void gemu_stream_set_gain(GemuStream *s, float g) { s->gain = g; }

void gemu_stream_update(GemuStream *s) {
    uint64_t target = gemu_cycles_in(s->rate, gemu_now(s->m));
    if (target < s->produced || target - s->produced > s->rate) {
        resync(s, target);                  /* a load, or a long gap */
        return;
    }
    uint64_t n = target - s->produced;
    if (!n)
        return;
    /* unmixed for two seconds: nobody is listening - keep the last one */
    if (s->len + n > 2 * (size_t)s->rate)
        drop_front(s, s->len + n - s->rate);
    if (!reserve(s, s->len + n)) {
        resync(s, target);
        return;
    }
    float *out[2] = { s->buf[0] + s->len, s->outputs > 1 ? s->buf[1] + s->len : NULL };
    s->update(s->ud, out, (unsigned)n);
    s->len += n;
    s->produced = target;
}

/* Everything starts again at the loaded moment, the mixer included: its
 * next run plays from there (a resync at that run would drop what came
 * between). */
void gemu_streams_after_load(GemuMachine *m) {
    for (GemuStream *s = m->streams; s; s = s->next)
        resync(s, gemu_cycles_in(s->rate, gemu_now(m)));
    GemuMixer *x = &m->mixer;
    x->synced = x->rate != 0;
    if (x->rate)
        x->out_pos = gemu_cycles_in(x->rate, gemu_now(m));
}

/* ── The mixer ────────────────────────────────────────────────────────────── */

void gemu_mixer_set_output(GemuMachine *m, unsigned rate, unsigned channels,
                           void (*sink)(void *ud, const float *samples, size_t frames), void *ud) {
    GemuMixer *x = &m->mixer;
    x->rate = rate;
    x->channels = channels == 2 ? 2 : 1;
    x->sink = sink;
    x->sink_ud = ud;
    x->synced = false;
}

/* Sample i of channel c (clamped into what's buffered). */
static float sample_at(const GemuStream *s, unsigned c, uint64_t i) {
    if (!s->len)
        return 0.0f;
    uint64_t j = i < s->consumed ? 0 : i - s->consumed;
    if (j >= s->len)
        j = s->len - 1;
    return s->buf[c < s->outputs ? c : 0][j];
}

static void wav_write_frames(GemuMixer *x, const float *mix, size_t frames) {
    int16_t pcm[512];
    size_t total = frames * x->channels, done = 0;
    while (done < total) {
        size_t k = total - done < 512 ? total - done : 512;
        for (size_t i = 0; i < k; i++) {
            float v = mix[done + i];
            v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
            pcm[i] = (int16_t)lrintf(v * 32767.0f);
        }
        fwrite(pcm, sizeof(int16_t), k, x->wav);
        done += k;
    }
    x->wav_frames += frames;
}

void gemu_mixer_run(GemuMachine *m) {
    GemuMixer *x = &m->mixer;
    for (GemuStream *s = m->streams; s; s = s->next)
        gemu_stream_update(s);
    if (!x->rate)
        return;
    uint64_t target = gemu_cycles_in(x->rate, gemu_now(m));
    if (!x->synced || target < x->out_pos || target - x->out_pos > x->rate) {
        x->out_pos = target;
        x->synced = true;
        for (GemuStream *s = m->streams; s; s = s->next)
            drop_front(s, s->len);
        return;
    }
    size_t n = (size_t)(target - x->out_pos);
    if (!n)
        return;
    size_t need = n * x->channels;
    if (need > x->mix_cap) {
        float *b = realloc(x->mix, need * sizeof(float));
        if (!b)
            return;
        x->mix = b;
        x->mix_cap = need;
    }
    memset(x->mix, 0, need * sizeof(float));
    for (GemuStream *s = m->streams; s; s = s->next) {
        for (size_t k = 0; k < n; k++) {
            uint64_t K = x->out_pos + k;
            uint64_t lo = (uint64_t)((u128)K * s->rate / x->rate);
            uint64_t hi = (uint64_t)((u128)(K + 1) * s->rate / x->rate);
            float v[2];
            for (unsigned c = 0; c < s->outputs; c++) {
                if (hi > lo) {                    /* the input samples in this output sample */
                    float sum = 0.0f;
                    for (uint64_t i = lo; i < hi; i++)
                        sum += sample_at(s, c, i);
                    v[c] = sum / (float)(hi - lo);
                } else {
                    v[c] = sample_at(s, c, lo);   /* slower stream: hold */
                }
            }
            float *o = &x->mix[k * x->channels];
            if (x->channels == 1)
                o[0] += s->gain * (s->outputs == 2 ? (v[0] + v[1]) * 0.5f : v[0]);
            else {
                o[0] += s->gain * v[0];
                o[1] += s->gain * (s->outputs == 2 ? v[1] : v[0]);
            }
        }
        /* what this mix used is done with */
        uint64_t used = (uint64_t)((u128)target * s->rate / x->rate);
        if (used > s->consumed)
            drop_front(s, (size_t)(used - s->consumed));
    }
    x->out_pos = target;
    if (x->sink)
        x->sink(x->sink_ud, x->mix, n);
    if (x->wav)
        wav_write_frames(x, x->mix, n);
}

/* ── WAV capture ──────────────────────────────────────────────────────────── */

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static void wav_header(uint8_t h[44], unsigned rate, unsigned channels, uint32_t data_bytes) {
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put16(h + 20, 1);                               /* PCM */
    put16(h + 22, (uint16_t)channels);
    put32(h + 24, rate);
    put32(h + 28, rate * channels * 2);
    put16(h + 32, (uint16_t)(channels * 2));
    put16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put32(h + 40, data_bytes);
}

bool gemu_mixer_wav_start(GemuMachine *m, const char *path) {
    GemuMixer *x = &m->mixer;
    if (x->wav || !x->rate || !path)
        return false;
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    uint8_t h[44];
    wav_header(h, x->rate, x->channels, 0);
    fwrite(h, 1, sizeof h, f);
    x->wav = f;
    x->wav_frames = 0;
    return true;
}

bool gemu_mixer_wav_stop(GemuMachine *m) {
    GemuMixer *x = &m->mixer;
    if (!x->wav)
        return false;
    uint64_t bytes = x->wav_frames * x->channels * 2;
    uint8_t h[44];
    wav_header(h, x->rate, x->channels, bytes > 0xFFFFFFD0u ? 0xFFFFFFD0u : (uint32_t)bytes);
    bool ok = fseek(x->wav, 0, SEEK_SET) == 0 && fwrite(h, 1, sizeof h, x->wav) == sizeof h;
    ok = fclose(x->wav) == 0 && ok;
    x->wav = NULL;
    return ok;
}

bool gemu_mixer_wav_running(const GemuMachine *m) { return m->mixer.wav != NULL; }

/* ── Cleanup ──────────────────────────────────────────────────────────────── */

static void free_stream(GemuStream *s) {
    free(s->buf[0]);
    free(s->buf[1]);
    free(s);
}

void gemu_streams_remove_device(GemuDevice *d) {
    for (GemuStream **pp = &d->machine->streams; *pp;) {
        GemuStream *s = *pp;
        if (s->dev == d) {
            *pp = s->next;
            free_stream(s);
        } else {
            pp = &s->next;
        }
    }
}

void gemu_streams_free_all(GemuMachine *m) {
    gemu_mixer_wav_stop(m);
    while (m->streams) {
        GemuStream *s = m->streams;
        m->streams = s->next;
        free_stream(s);
    }
    free(m->mixer.mix);
    m->mixer.mix = NULL;
}
