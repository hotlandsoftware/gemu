/*
 * GEMU's 86Box layer: sound and music handlers (<86box/sound.h>) as core
 * streams.
 *
 * 86Box ticks sound_pos_global/music_pos_global once a sample and calls the
 * handlers when a buffer is full.  Here a device's handlers of one kind
 * share a stream (stereo, 48000 or 49716 Hz) whose update calls them for
 * the samples since the last one, in pieces of at most a buffer;
 * `start` is the sample their buffers begin at.  On every entry into the
 * device's code (or a sub-device's: the AdLib's OPL) the *_pos_global says
 * how far the present is past `start` - where a driver generates up to
 * before a register change.  A timer brings the stream up to date every half
 * buffer, so that is never more than a buffer.
 */
#include "compat_priv.h"
#include <86box/86box.h>
#include <86box/sound.h>
#include "gemu/dev/stream.h"
#include <stdlib.h>
#include <string.h>

int sound_pos_global;
int music_pos_global;

typedef void (*GetBuffer)(int32_t *buffer, uint16_t len, void *priv);

typedef struct Handler {
    GetBuffer       get_buffer;
    void           *priv;
    struct Handler *next;
} Handler;

typedef struct {
    Gemu86BoxDev *owner;
    unsigned      rate, buflen;
    int          *pos;            /* sound_pos_global or music_pos_global */
    Handler      *handlers;
    GemuStream   *stream;
    GemuTimer    *timer;
    uint64_t      start;
    int32_t      *buf;
} Kind;

struct Gemu86BoxSound {
    Kind kind[2];                 /* sound, music */
};

static const struct { const char *name; unsigned rate, buflen; int *pos; } kinds[2] = {
    { "sound", SOUND_FREQ, SOUNDBUFLEN, &sound_pos_global },
    { "music", MUSIC_FREQ, MUSICBUFLEN, &music_pos_global },
};

static uint64_t now_in(const Kind *k) {
    return gemu_cycles_in(k->rate, gemu_now(k->owner->dev->machine));
}

/* The handlers' buffers start again at `at` (a load, a resync): each is
 * called for no samples, which makes it drop what it had made. */
static void restart(Kind *k, uint64_t at) {
    *k->pos = 0;
    for (Handler *h = k->handlers; h; h = h->next)
        h->get_buffer(k->buf, 0, h->priv);
    k->start = at;
}

static void update(void *ud, float *const *out, unsigned n) {
    Kind *k = ud;
    Gemu86BoxDev *prev = gemu_86box_enter(k->owner);
    uint64_t target = now_in(k);
    if (k->start != target - n)   /* the stream resynced (a load, a long gap) */
        restart(k, target - n);
    for (unsigned done = 0; done < n;) {
        unsigned c = n - done < k->buflen ? n - done : k->buflen;
        *k->pos = (int)c;
        memset(k->buf, 0, c * 2 * sizeof *k->buf);
        for (Handler *h = k->handlers; h; h = h->next)
            h->get_buffer(k->buf, (uint16_t)c, h->priv);
        for (unsigned i = 0; i < c; i++) {
            out[0][done + i] = (float)k->buf[2 * i] / 32768.0f;
            out[1][done + i] = (float)k->buf[2 * i + 1] / 32768.0f;
        }
        done += c;
        k->start += c;
    }
    *k->pos = 0;
    gemu_86box_leave(prev);
}

static void tick(void *ud, int param) {
    (void)param;
    gemu_stream_update(((Kind *)ud)->stream);
}

static void add_handler(int which, GetBuffer get_buffer, void *priv) {
    Gemu86BoxDev *b = gemu_86box_need(kinds[which].name);
    if (!b->in_init)
        fatal("%s_add_handler outside the device's init\n", kinds[which].name);
    if (!b->sound && !(b->sound = calloc(1, sizeof *b->sound)))
        fatal("out of memory\n");
    Kind *k = &b->sound->kind[which];
    if (!k->stream) {
        k->owner = b;
        k->rate = kinds[which].rate;
        k->buflen = kinds[which].buflen;
        k->pos = kinds[which].pos;
        if (!(k->buf = calloc(k->buflen * 2, sizeof *k->buf)) ||
            !(k->stream = gemu_stream_new(b->dev, kinds[which].name, 2, k->rate, update, k)) ||
            !(k->timer = gemu_timer_new(b->dev, tick, k)))
            fatal("%s_add_handler: no stream\n", kinds[which].name);
        gemu_timer_start_clocks(k->timer, k->rate, k->buflen / 2, k->buflen / 2, 0);
        k->start = now_in(k);
    }
    Handler *h = calloc(1, sizeof *h);
    if (!h)
        fatal("out of memory\n");
    *h = (Handler){ get_buffer, priv, NULL };
    Handler **pp = &k->handlers;
    while (*pp)
        pp = &(*pp)->next;
    *pp = h;
}

void sound_add_handler(GetBuffer get_buffer, void *priv) { add_handler(0, get_buffer, priv); }
void music_add_handler(GetBuffer get_buffer, void *priv) { add_handler(1, get_buffer, priv); }

void gemu_86box_sound_enter(Gemu86BoxDev *b) {
    while (b && !b->sound)
        b = b->parent86;
    if (!b)
        return;
    for (int i = 0; i < 2; i++) {
        Kind *k = &b->sound->kind[i];
        if (!k->stream)
            continue;
        uint64_t t = now_in(k);
        uint64_t p = t > k->start ? t - k->start : 0;
        *k->pos = (int)(p < k->buflen ? p : k->buflen);
    }
}

void gemu_86box_sound_pre_save(Gemu86BoxDev *b) {
    for (int i = 0; b->sound && i < 2; i++)
        if (b->sound->kind[i].stream)
            gemu_stream_update(b->sound->kind[i].stream);   /* the drivers made it to now */
}

void gemu_86box_sound_post_load(Gemu86BoxDev *b) {
    for (int i = 0; b->sound && i < 2; i++) {
        Kind *k = &b->sound->kind[i];
        if (!k->stream)
            continue;
        Gemu86BoxDev *prev = gemu_86box_enter(b);
        restart(k, now_in(k));
        gemu_86box_leave(prev);
    }
}

void gemu_86box_sound_free(Gemu86BoxDev *b) {
    if (!b->sound)
        return;
    for (int i = 0; i < 2; i++) {        /* the streams and timers go with the device */
        Kind *k = &b->sound->kind[i];
        while (k->handlers) {
            Handler *h = k->handlers;
            k->handlers = h->next;
            free(h);
        }
        free(k->buf);
    }
    free(b->sound);
    b->sound = NULL;
}
