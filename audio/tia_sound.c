#include "tia_sound.h"
#include <stdio.h>
#include <string.h>

/* ── Channel: the two clock phases ───────────────────────────────────────── */

static void channel_phase0(TiaChannel *c) {
    if (c->clock_enable) {
        c->noise_bit4 = c->noise_counter & 0x01u;

        switch (c->audc & 0x03u) {
        case 0x00: case 0x01: c->pulse_hold = false; break;
        case 0x02: c->pulse_hold = (c->noise_counter & 0x1Eu) != 0x02u; break;
        case 0x03: c->pulse_hold = !c->noise_bit4; break;
        }

        if ((c->audc & 0x03u) == 0x00u)
            c->noise_feedback =
                ((c->pulse_counter ^ c->noise_counter) & 0x01u) ||
                !(c->noise_counter || c->pulse_counter != 0x0Au) ||
                !(c->audc & 0x0Cu);
        else
            c->noise_feedback =
                (((c->noise_counter & 0x04u) ? 1u : 0u) ^ (c->noise_counter & 0x01u)) ||
                c->noise_counter == 0;
    }

    c->clock_enable = c->div_counter == c->audf;
    if (c->div_counter == c->audf || c->div_counter == 0x1Fu) c->div_counter = 0;
    else                                                      c->div_counter++;
}

static void channel_phase1(TiaChannel *c) {
    if (c->clock_enable) {
        bool pulse_feedback = false;
        switch (c->audc >> 2) {
        case 0x00:
            pulse_feedback =
                (((c->pulse_counter & 0x02u) ? 1u : 0u) ^ (c->pulse_counter & 0x01u)) &&
                c->pulse_counter != 0x0Au && (c->audc & 0x03u);
            break;
        case 0x01: pulse_feedback = !(c->pulse_counter & 0x08u); break;
        case 0x02: pulse_feedback = !c->noise_bit4; break;
        case 0x03:
            pulse_feedback = !((c->pulse_counter & 0x02u) || !(c->pulse_counter & 0x0Eu));
            break;
        }

        c->noise_counter >>= 1;
        if (c->noise_feedback) c->noise_counter |= 0x10u;

        if (!c->pulse_hold) {
            c->pulse_counter = (uint8_t)(~(c->pulse_counter >> 1) & 0x07u);
            if (pulse_feedback) c->pulse_counter |= 0x08u;
        }
    }
}

/* ── Output ──────────────────────────────────────────────────────────────── */

/* The channels share a resistor network, so loud sums compress
 * (Stella's mixing table: v/30 * (30 + 30) / (30 + v)). */
static float mix(int sum) {
    return (float)(sum / 30.0 * 60.0 / (30.0 + sum));
}

static void emit(TiaSound *t) {
    uint8_t v0 = (uint8_t)(t->sum[0] / t->sum_n), v1 = (uint8_t)(t->sum[1] / t->sum_n);
    t->sum[0] = t->sum[1] = t->sum_n = 0;
    if (!t->dev) return;

    /* Box filter: each TIA sample lasts about 1.4 output samples; every
     * output sample averages whatever TIA samples overlap it. */
    double v = mix(v0 + v1);
    double left = TIA_SOUND_RATE / t->tia_rate;
    while (left > 0) {
        double take = left < 1.0 - t->phase ? left : 1.0 - t->phase;
        t->acc += v * take;
        t->phase += take;
        left -= take;
        if (t->phase < 1.0 - 1e-9) break;
        /* DC blocker: the TIA output is unipolar. */
        double y = t->acc - t->dc_x + 0.995 * t->dc_y;
        t->dc_x = t->acc; t->dc_y = y;
        if (t->buf_n < TIA_SOUND_BUF) t->buf[t->buf_n++] = (float)(y * 0.5);
        t->acc = 0;
        t->phase = 0;
    }
}

void tia_sound_clock(TiaSound *t) {
    for (int i = 0; i < 2; i++) t->sum[i] += (t->ch[i].pulse_counter & 0x01u) * t->ch[i].audv;
    t->sum_n++;
    switch (++t->counter) {
    case 10: case 82:
        channel_phase0(&t->ch[0]);
        channel_phase0(&t->ch[1]);
        break;
    case 38: case 150:
        channel_phase1(&t->ch[0]);
        channel_phase1(&t->ch[1]);
        emit(t);
        break;
    case 228:
        t->counter = 0;
        break;
    }
}

void tia_sound_flush(TiaSound *t) {
    if (t->dev && t->buf_n)
        SDL_QueueAudio(t->dev, t->buf, (Uint32)(t->buf_n * sizeof(float)));
    t->buf_n = 0;
}

uint32_t tia_sound_queued(const TiaSound *t) {
    return t->dev ? SDL_GetQueuedAudioSize(t->dev) : 0;
}

void tia_sound_write(TiaSound *t, uint8_t reg, uint8_t val) {
    switch (reg) {
    case 0x15: t->ch[0].audc = val & 0x0Fu; break;
    case 0x16: t->ch[1].audc = val & 0x0Fu; break;
    case 0x17: t->ch[0].audf = val & 0x1Fu; break;
    case 0x18: t->ch[1].audf = val & 0x1Fu; break;
    case 0x19: t->ch[0].audv = val & 0x0Fu; break;
    case 0x1A: t->ch[1].audv = val & 0x0Fu; break;
    }
}

/* ── Init ────────────────────────────────────────────────────────────────── */

void tia_sound_reset(TiaSound *t) {
    memset(t->ch, 0, sizeof(t->ch));
    t->sum[0] = t->sum[1] = t->sum_n = 0;
}

bool tia_sound_init(TiaSound *t, double tia_rate, bool want_output) {
    memset(t, 0, sizeof(*t));
    t->tia_rate = tia_rate;
    if (!want_output) return true;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "tia: SDL audio init failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec want = {
        .freq = TIA_SOUND_RATE, .format = AUDIO_F32SYS, .channels = 1, .samples = 512,
    }, have;
    t->dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!t->dev) {
        fprintf(stderr, "tia: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    SDL_PauseAudioDevice(t->dev, 0);
    return true;
}

void tia_sound_destroy(TiaSound *t) {
    if (t->dev) {
        SDL_CloseAudioDevice(t->dev);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        t->dev = 0;
    }
}
