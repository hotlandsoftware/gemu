/* Sound streams and the mixer (core/src/dev/stream.c).  Run: ninja check-stream */
#include "gemu/dev/scheduler.h"
#include "gemu/dev/state.h"
#include "gemu/dev/stream.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(expr) do { checks++; if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

static GemuMachine *m;

/* A beeper: its level, flipped by a timer; its stream says the level. */
typedef struct {
    bool        level;
    float       amp;
    GemuStream *stream;
    GemuTimer  *t;
    GemuTime    toggles[4096];
    unsigned    n_toggles;
} Beeper;

static void beep_update(void *ud, float *const *out, unsigned n) {
    Beeper *b = ud;
    for (unsigned i = 0; i < n; i++) {
        out[0][i] = b->level ? b->amp : 0.0f;
        if (out[1]) out[1][i] = b->level ? -b->amp : 0.0f;
    }
}
static void beep_toggle(void *ud, int param) {
    (void)param;
    Beeper *b = ud;
    gemu_stream_update(b->stream);        /* up to now with the old level */
    b->level = !b->level;
    if (b->n_toggles < 4096) b->toggles[b->n_toggles++] = gemu_now(m);
}
static bool beep_init(GemuDevice *d) { return GEMU_STATE_VAR(d, "level", ((Beeper *)d->priv)->level); }
static const GemuDeviceType beep_type = { .name = "beeper", .size = sizeof(Beeper), .init = beep_init };

/* The sink: everything mixed */
static float got[1 << 20];
static size_t n_got;
static unsigned sink_channels = 1;
static void sink(void *ud, const float *s, size_t frames) {
    (void)ud;
    for (size_t i = 0; i < frames * sink_channels && n_got < sizeof got / sizeof got[0]; i++)
        got[n_got++] = s[i];
}

/* The level a beeper's input sample j had: toggled at each recorded time */
static bool level_at_sample(const Beeper *b, unsigned rate, uint64_t j, bool start) {
    bool l = start;
    for (unsigned i = 0; i < b->n_toggles; i++)
        if (j >= gemu_cycles_in(rate, b->toggles[i])) l = !l;
    return l;
}
/* What the mixer should make for output sample K: the box average */
static float expect(const Beeper *b, unsigned srate, unsigned orate, uint64_t K, float amp) {
    uint64_t lo = K * srate / orate, hi = (K + 1) * srate / orate;
    if (hi == lo) return level_at_sample(b, srate, lo, false) ? amp : 0.0f;
    float sum = 0;
    for (uint64_t j = lo; j < hi; j++) sum += level_at_sample(b, srate, j, false) ? amp : 0.0f;
    return sum / (float)(hi - lo);
}

static GemuDevice *add_beeper(const char *name, unsigned rate, unsigned outputs, float amp) {
    GemuDevice *d = gemu_device_add(m, &beep_type, name, NULL, NULL);
    CHECK(d);
    Beeper *b = d->priv;
    b->amp = amp;
    b->stream = gemu_stream_new(d, "out", outputs, rate, beep_update, b);
    b->t = gemu_timer_new(d, beep_toggle, b);
    CHECK(b->stream && b->t);
    return d;
}

static void fresh(unsigned orate, unsigned channels) {
    if (m) gemu_machine_free(m);
    m = gemu_machine_new("snd");
    CHECK(gemu_machine_start(m));
    n_got = 0;
    sink_channels = channels;
    gemu_mixer_set_output(m, orate, channels, sink, NULL);
}

/* Run to t, mixing every 10 ms as a machine's frames would */
static void run_mixing(GemuTime t) {
    while (gemu_machine_time(m) < t) {
        GemuTime next = gemu_machine_time(m) + GEMU_MSEC(10);
        gemu_machine_run_until(m, next < t ? next : t);
        gemu_mixer_run(m);
    }
}

int main(void) {
    /* ── Arguments ── */
    fresh(48000, 1);
    GemuDevice *d0 = gemu_device_add(m, &beep_type, "x", NULL, NULL);
    CHECK(!gemu_stream_new(d0, "s", 3, 48000, beep_update, NULL));
    CHECK(!gemu_stream_new(d0, "s", 1, 0, beep_update, NULL));
    CHECK(!gemu_stream_new(d0, "s", 1, 48000, NULL, NULL));
    CHECK(!gemu_stream_new(d0, "", 1, 48000, beep_update, NULL));
    GemuStream *s0 = gemu_stream_new(d0, "s", 1, 44100, beep_update, d0->priv);
    CHECK(s0 && gemu_stream_rate(s0) == 44100 && !strcmp(gemu_stream_name(s0), "s"));
    CHECK(!gemu_stream_new(d0, "s", 1, 48000, beep_update, NULL));    /* duplicate */

    /* ── A 1 kHz square wave at 192 kHz, mixed to 48 kHz: sample for sample ── */
    fresh(48000, 1);
    Beeper *b = add_beeper("spk", 192000, 1, 0.5f)->priv;
    gemu_mixer_run(m);                                    /* the mixer starts here (0) */
    gemu_timer_start(b->t, GEMU_USEC(500), GEMU_USEC(500), 0);
    run_mixing(GEMU_MSEC(100));
    CHECK(n_got == 4800);
    CHECK(b->n_toggles == 200);
    for (uint64_t k = 0; k < n_got; k++)
        if (got[k] != expect(b, 192000, 48000, k, 0.5f)) {
            fprintf(stderr, "sample %llu: %f, wanted %f\n", (unsigned long long)k, got[k],
                    expect(b, 192000, 48000, k, 0.5f));
            CHECK(0);
        }
    CHECK(got[0] == 0.0f && got[23] == 0.0f && got[24] == 0.5f && got[47] == 0.5f && got[48] == 0.0f);

    /* ── An edge in the middle of an output sample: the box average ── */
    fresh(48000, 1);
    b = add_beeper("spk", 192000, 1, 1.0f)->priv;
    gemu_mixer_run(m);
    /* input sample 4*100 + 2 = 402 is the first high: output 100 is half high */
    GemuTime edge = gemu_clocks(192000, 402);
    gemu_timer_start(b->t, edge, 0, 0);
    run_mixing(GEMU_MSEC(5));
    CHECK(got[99] == 0.0f && got[100] == 0.5f && got[101] == 1.0f && got[239] == 1.0f);

    /* ── Two streams, gains; stereo output; a stereo stream ── */
    fresh(48000, 2);
    Beeper *a = add_beeper("a", 48000, 1, 0.25f)->priv;
    Beeper *c = add_beeper("c", 96000, 2, 0.5f)->priv;    /* right channel is -left */
    gemu_stream_set_gain(a->stream, 2.0f);
    gemu_mixer_run(m);
    gemu_timer_start(a->t, GEMU_MSEC(1), 0, 0);           /* a high from 1 ms */
    gemu_timer_start(c->t, GEMU_MSEC(2), 0, 0);           /* c high from 2 ms */
    run_mixing(GEMU_MSEC(4));
    CHECK(n_got == 192 * 2);
    /* frame 30 (0.625 ms): nothing; 60 (1.25 ms): a; 120 (2.5 ms): a and c */
    CHECK(got[60] == 0.0f && got[61] == 0.0f);
    CHECK(got[120] == 0.5f && got[121] == 0.5f);          /* a: 0.25 x 2 in both */
    CHECK(got[240] == 1.0f && got[241] == 0.0f);          /* + c: +0.5 left, -0.5 right */

    /* ... into a mono sink the stereo stream is averaged */
    fresh(48000, 1);
    c = add_beeper("c", 48000, 2, 0.5f)->priv;
    gemu_mixer_run(m);
    gemu_timer_start(c->t, GEMU_MSEC(1), 0, 0);
    run_mixing(GEMU_MSEC(2));
    CHECK(got[60] == 0.0f);

    /* ── A slower stream is held ── */
    fresh(48000, 1);
    b = add_beeper("slow", 8000, 1, 1.0f)->priv;
    gemu_mixer_run(m);
    gemu_timer_start(b->t, gemu_clocks(8000, 10), 0, 0);  /* input sample 10 on */
    run_mixing(GEMU_MSEC(3));
    for (uint64_t k = 0; k < n_got; k++)
        CHECK(got[k] == ((k * 8000 / 48000) >= 10 ? 1.0f : 0.0f));
    CHECK(got[59] == 0.0f && got[60] == 1.0f);

    /* ── WAV capture: what the sink got, as 16-bit PCM ── */
    fresh(48000, 1);
    b = add_beeper("spk", 192000, 1, 0.5f)->priv;
    gemu_mixer_run(m);
    gemu_timer_start(b->t, GEMU_USEC(333), GEMU_USEC(333), 0);
    CHECK(!gemu_mixer_wav_running(m) && gemu_mixer_wav_start(m, "out.wav"));
    CHECK(!gemu_mixer_wav_start(m, "other.wav") && gemu_mixer_wav_running(m));
    run_mixing(GEMU_MSEC(50));
    CHECK(gemu_mixer_wav_stop(m) && !gemu_mixer_wav_stop(m));
    FILE *f = fopen("out.wav", "rb");
    CHECK(f);
    static uint8_t wav[44 + 4800 * 2 + 16];
    size_t wn = fread(wav, 1, sizeof wav, f);
    fclose(f);
    CHECK(wn == 44 + 2400 * 2 && !memcmp(wav, "RIFF", 4) && !memcmp(wav + 8, "WAVEfmt ", 8));
    CHECK(wav[22] == 1 && (wav[24] | wav[25] << 8 | wav[26] << 16) == 48000 && wav[34] == 16);
    CHECK((wav[40] | wav[41] << 8 | wav[42] << 16) == 4800);
    for (size_t k = 0; k < 2400; k++) {
        int16_t v = (int16_t)(wav[44 + 2 * k] | wav[45 + 2 * k] << 8);
        CHECK(v == (int16_t)lrintf(got[k] * 32767.0f));
    }
    CHECK(!gemu_mixer_wav_start(m, "/nonexistent/dir/x.wav"));

    /* ── Time jumping back (a load) resyncs: no burst, no stale samples ── */
    uint8_t *st;
    size_t stn;
    CHECK(gemu_machine_save_state(m, &st, &stn));
    run_mixing(gemu_machine_time(m) + GEMU_MSEC(200));
    size_t before = n_got;
    CHECK(gemu_machine_load_state(m, st, stn));
    gemu_mixer_run(m);                                    /* the resync: nothing out */
    CHECK(n_got == before);
    run_mixing(gemu_machine_time(m) + GEMU_MSEC(10));
    CHECK(n_got == before + 480);
    /* ... and forward, by less than a second: still a resync, the gap isn't played */
    uint8_t *ahead;
    size_t aheadn;
    run_mixing(gemu_machine_time(m) + GEMU_MSEC(300));
    CHECK(gemu_machine_save_state(m, &ahead, &aheadn));
    CHECK(gemu_machine_load_state(m, st, stn));
    run_mixing(gemu_machine_time(m) + GEMU_MSEC(10));
    before = n_got;
    CHECK(gemu_machine_load_state(m, ahead, aheadn));    /* 300 ms on */
    gemu_mixer_run(m);
    CHECK(n_got == before);
    run_mixing(gemu_machine_time(m) + GEMU_MSEC(10));
    CHECK(n_got == before + 480);
    /* the sound goes on from the loaded moment: nothing is dropped */
    CHECK(gemu_machine_load_state(m, st, stn));
    before = n_got;
    run_mixing(gemu_machine_time(m) + GEMU_MSEC(10));
    CHECK(n_got == before + 480);
    free(ahead);
    free(st);

    /* ── Nobody mixing for a long time: no pile-up, no burst afterwards ── */
    gemu_machine_run_until(m, gemu_machine_time(m) + GEMU_SEC(5));   /* toggling all along */
    before = n_got;
    gemu_mixer_run(m);
    CHECK(n_got == before);                               /* more than a second: resync */
    run_mixing(gemu_machine_time(m) + GEMU_MSEC(10));
    CHECK(n_got == before + 480);

    /* ── Without an output the mixer does nothing; streams go with devices ── */
    gemu_mixer_set_output(m, 48000, 1, NULL, NULL);
    run_mixing(gemu_machine_time(m) + GEMU_MSEC(10));
    gemu_device_destroy(gemu_device_find(m, "spk"));
    run_mixing(gemu_machine_time(m) + GEMU_MSEC(10));
    gemu_machine_free(m);
    m = NULL;
    printf("stream: %u checks passed\n", checks);
    return 0;
}
