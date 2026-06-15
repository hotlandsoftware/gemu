#include "apu2a03.h"
#include <string.h>
#include <math.h>
#include <stdio.h>

/* ── Static tables ───────────────────────────────────────────────────────── */

static const uint8_t len_table[32] = {
    10,254, 20,  2, 40,  4, 80,  6,
   160,  8, 60, 10, 14, 12, 26, 14,
    12, 16, 24, 18, 48, 20, 96, 22,
   192, 24, 72, 26, 16, 28, 32, 30
};

static const uint16_t noise_period[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068
};

static const uint16_t dmc_rate[16] = {
    428, 380, 340, 320, 286, 254, 226, 214,
    190, 160, 142, 128, 106,  84,  72,  54
};

static const uint8_t duty_seq[4][8] = {
    {0,1,0,0,0,0,0,0},
    {0,1,1,0,0,0,0,0},
    {0,1,1,1,1,0,0,0},
    {1,0,0,1,1,1,1,1},
};

static const uint8_t tri_seq[32] = {
    15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0,
     0, 1, 2, 3, 4, 5,6,7,8,9,10,11,12,13,14,15
};

/* Frame counter step points (CPU cycles, NTSC) */
static const uint32_t fc4[4] = {7457, 14913, 22371, 29829};
static const uint32_t fc5[5] = {7457, 14913, 22371, 29829, 37281};

/* ── Envelope helpers ────────────────────────────────────────────────────── */

static void env_clock(uint8_t *div, uint8_t *dec, bool *start,
                      bool halt, uint8_t vol) {
    if (*start) {
        *start = false;
        *dec   = 15;
        *div   = vol;
    } else if (*div > 0) {
        (*div)--;
    } else {
        *div = vol;
        if (*dec > 0) (*dec)--;
        else if (halt) *dec = 15;
    }
}

static uint8_t env_volume(const bool const_vol, uint8_t vol, uint8_t dec) {
    return const_vol ? vol : dec;
}

/* ── Sweep helper ────────────────────────────────────────────────────────── */

static uint16_t sweep_target(uint16_t period, uint8_t shift,
                              bool neg, int ch) {
    uint16_t delta = period >> shift;
    if (neg) return (uint16_t)(period - delta - (ch == 0 ? 1 : 0));
    return (uint16_t)(period + delta);
}

static bool sweep_mute(uint16_t period, uint8_t shift, bool neg, int ch) {
    if (period < 8) return true;
    uint16_t t = sweep_target(period, shift, neg, ch);
    return t > 0x7FF;
}

/* ── Channel output ──────────────────────────────────────────────────────── */

static float pulse_out(const ApuPulse *p, int ch, bool en) {
    if (!en || p->len == 0) return 0.0f;
    if (p->timer_period < 8) return 0.0f;
    if (sweep_mute(p->timer_period, p->sweep_shift, p->sweep_neg, ch)) return 0.0f;
    if (!duty_seq[p->duty][p->seq]) return 0.0f;
    return (float)env_volume(p->const_vol, p->vol, p->env_dec);
}

static float tri_out(const ApuTri *t, bool en) {
    if (!en || t->len == 0 || t->lin == 0) return 0.0f;
    if (t->timer_period < 2) return 0.0f; /* silence ultra-high freq */
    return (float)tri_seq[t->seq];
}

static float noise_out(const ApuNoise *n, bool en) {
    if (!en || n->len == 0) return 0.0f;
    if (n->lfsr & 1) return 0.0f;
    return (float)env_volume(n->const_vol, n->vol, n->env_dec);
}

static float dmc_out(const ApuDmc *d, bool en) {
    (void)en;
    return (float)(d->level & 0x7F);
}

/* ── NES non-linear mixer ────────────────────────────────────────────────── */

static float mix(float p1, float p2, float tri, float noi, float dmc) {
    float pulse_out = 0.0f;
    if (p1 + p2 > 0.0f)
        pulse_out = 95.88f / (8128.0f / (p1 + p2) + 100.0f);

    float tnd = tri / 8227.0f + noi / 12241.0f + dmc / 22638.0f;
    float tnd_out = 0.0f;
    if (tnd > 0.0f)
        tnd_out = 159.79f / (1.0f / tnd + 100.0f);

    return pulse_out + tnd_out;
}

/* ── Frame counter sequencer ─────────────────────────────────────────────── */

static void clock_envelopes(Apu2a03 *a) {
    env_clock(&a->pulse[0].env_div, &a->pulse[0].env_dec,
              &a->pulse[0].env_start, a->pulse[0].halt, a->pulse[0].vol);
    env_clock(&a->pulse[1].env_div, &a->pulse[1].env_dec,
              &a->pulse[1].env_start, a->pulse[1].halt, a->pulse[1].vol);
    env_clock(&a->noise.env_div, &a->noise.env_dec,
              &a->noise.env_start, a->noise.halt, a->noise.vol);
    /* Triangle linear counter */
    if (a->tri.lin_reload)
        a->tri.lin = a->tri.lin_load;
    else if (a->tri.lin > 0)
        a->tri.lin--;
    if (!a->tri.halt) a->tri.lin_reload = false;
}

static void clock_lengths_sweep(Apu2a03 *a) {
    /* Length counters */
    for (int i = 0; i < 2; i++)
        if (!a->pulse[i].halt && a->pulse[i].len > 0) a->pulse[i].len--;
    if (!a->tri.halt && a->tri.len > 0) a->tri.len--;
    if (!a->noise.halt && a->noise.len > 0) a->noise.len--;

    /* Sweep units */
    for (int i = 0; i < 2; i++) {
        ApuPulse *p = &a->pulse[i];
        if (p->sweep_div == 0 && p->sweep_en && p->sweep_shift > 0
            && !sweep_mute(p->timer_period, p->sweep_shift, p->sweep_neg, i)) {
            p->timer_period = sweep_target(p->timer_period, p->sweep_shift,
                                           p->sweep_neg, i);
        }
        if (p->sweep_div == 0 || p->sweep_reload) {
            p->sweep_div    = p->sweep_period;
            p->sweep_reload = false;
        } else {
            p->sweep_div--;
        }
    }
}

static void clock_frame(Apu2a03 *a, bool half) {
    clock_envelopes(a);
    if (half) clock_lengths_sweep(a);
}

/* ── APU tick (one CPU cycle) ────────────────────────────────────────────── */

void apu2a03_tick(Apu2a03 *a) {
    a->cycle++;
    a->apu_clk = !a->apu_clk;

    /* Triangle: clocked every CPU cycle */
    if (a->tri.timer > 0) {
        a->tri.timer--;
    } else {
        a->tri.timer = a->tri.timer_period;
        if (a->tri.len > 0 && a->tri.lin > 0)
            a->tri.seq = (a->tri.seq + 1) & 31;
    }

    /* Pulse, noise, DMC: clocked every 2 CPU cycles (APU clock) */
    if (a->apu_clk) {
        for (int i = 0; i < 2; i++) {
            ApuPulse *p = &a->pulse[i];
            if (p->timer > 0) {
                p->timer--;
            } else {
                p->timer = p->timer_period;
                p->seq   = (p->seq + 1) & 7;
            }
        }

        /* Noise */
        if (a->noise.timer > 0) {
            a->noise.timer--;
        } else {
            a->noise.timer = a->noise.period;
            uint8_t feedback = a->noise.mode
                ? ((a->noise.lfsr >> 6) & 1) ^ (a->noise.lfsr & 1)
                : ((a->noise.lfsr >> 1) & 1) ^ (a->noise.lfsr & 1);
            a->noise.lfsr = (uint16_t)((a->noise.lfsr >> 1) | (feedback << 14));
        }

        /* DMC */
        if (a->ch_en[4]) {
            if (a->dmc.timer > 0) {
                a->dmc.timer--;
            } else {
                a->dmc.timer = a->dmc.period;
                if (!a->dmc.silence) {
                    if (a->dmc.shift & 1) {
                        if (a->dmc.level <= 125) a->dmc.level += 2;
                    } else {
                        if (a->dmc.level >= 2)   a->dmc.level -= 2;
                    }
                    a->dmc.shift >>= 1;
                }
                if (--a->dmc.bits_rem == 0) {
                    a->dmc.bits_rem = 8;
                    if (a->dmc.buf_empty) {
                        a->dmc.silence = true;
                    } else {
                        a->dmc.silence  = false;
                        a->dmc.shift    = a->dmc.sample_buf;
                        a->dmc.buf_empty = true;
                    }
                }
                /* Refill DMC sample buffer from memory */
                if (a->dmc.buf_empty && a->dmc.bytes_rem > 0 && a->mem_read) {
                    a->dmc.sample_buf = a->mem_read(a->dmc.cur_addr, a->mem_ud);
                    a->dmc.buf_empty  = false;
                    a->dmc.cur_addr   = (a->dmc.cur_addr == 0xFFFF)
                                        ? 0x8000 : a->dmc.cur_addr + 1;
                    if (--a->dmc.bytes_rem == 0) {
                        if (a->dmc.loop) {
                            a->dmc.cur_addr  = a->dmc.start_addr;
                            a->dmc.bytes_rem = a->dmc.start_len;
                        } else if (a->dmc.irq_en) {
                            a->dmc.irq_flag = true;
                        }
                    }
                }
            }
        }
    }

    /* Frame counter */
    {
        uint32_t steps = a->fc_mode ? 5 : 4;
        const uint32_t *pts = a->fc_mode ? fc5 : fc4;
        uint32_t pos = a->fc_div % (a->fc_mode ? 37282u : 29830u);
        a->fc_div++;

        for (uint32_t i = 0; i < steps; i++) {
            if (pos == pts[i]) {
                /* 5-step mode step 3 (cycle 29829) is a no-op on hardware */
                if (a->fc_mode && i == 3) break;
                bool half = (i == 1) || (i == 3) || (a->fc_mode && i == 4);
                clock_frame(a, half);
                if (!a->fc_mode && i == 3 && !a->fc_irq_inhibit)
                    a->fc_irq = true;
                break;
            }
        }
    }

    /* Sample generation */
    a->sample_acc += 1.0;
    double cps = (double)APU2A03_CPU_CLOCK / (double)APU2A03_SAMPLE_RATE;
    if (a->sample_acc >= cps) {
        a->sample_acc -= cps;
        if (a->frame_n < 1024) {
            float s = mix(
                pulse_out(&a->pulse[0], 0, a->ch_en[0]),
                pulse_out(&a->pulse[1], 1, a->ch_en[1]),
                tri_out(&a->tri,           a->ch_en[2]),
                noise_out(&a->noise,       a->ch_en[3]),
                dmc_out(&a->dmc,           a->ch_en[4])
            ) + a->fds_in;
            a->frame_buf[a->frame_n++] = s;
        }
    }
}

/* ── Register writes ─────────────────────────────────────────────────────── */

void apu2a03_write(Apu2a03 *a, uint16_t addr, uint8_t v) {
    switch (addr) {
    /* ── Pulse 1 ─────── */
    case 0x4000:
        a->pulse[0].duty      = v >> 6;
        a->pulse[0].halt      = (v >> 5) & 1;
        a->pulse[0].const_vol = (v >> 4) & 1;
        a->pulse[0].vol       = v & 0xF;
        break;
    case 0x4001:
        a->pulse[0].sweep_en     = (v >> 7) & 1;
        a->pulse[0].sweep_period = (v >> 4) & 7;
        a->pulse[0].sweep_neg    = (v >> 3) & 1;
        a->pulse[0].sweep_shift  = v & 7;
        a->pulse[0].sweep_reload = true;
        break;
    case 0x4002:
        a->pulse[0].timer_period = (a->pulse[0].timer_period & 0x700) | v;
        break;
    case 0x4003:
        a->pulse[0].timer_period = (a->pulse[0].timer_period & 0xFF) | ((uint16_t)(v & 7) << 8);
        if (a->ch_en[0]) a->pulse[0].len = len_table[v >> 3];
        a->pulse[0].env_start = true;
        a->pulse[0].seq       = 0;
        break;
    /* ── Pulse 2 ─────── */
    case 0x4004:
        a->pulse[1].duty      = v >> 6;
        a->pulse[1].halt      = (v >> 5) & 1;
        a->pulse[1].const_vol = (v >> 4) & 1;
        a->pulse[1].vol       = v & 0xF;
        break;
    case 0x4005:
        a->pulse[1].sweep_en     = (v >> 7) & 1;
        a->pulse[1].sweep_period = (v >> 4) & 7;
        a->pulse[1].sweep_neg    = (v >> 3) & 1;
        a->pulse[1].sweep_shift  = v & 7;
        a->pulse[1].sweep_reload = true;
        break;
    case 0x4006:
        a->pulse[1].timer_period = (a->pulse[1].timer_period & 0x700) | v;
        break;
    case 0x4007:
        a->pulse[1].timer_period = (a->pulse[1].timer_period & 0xFF) | ((uint16_t)(v & 7) << 8);
        if (a->ch_en[1]) a->pulse[1].len = len_table[v >> 3];
        a->pulse[1].env_start = true;
        a->pulse[1].seq       = 0;
        break;
    /* ── Triangle ────── */
    case 0x4008:
        a->tri.halt      = (v >> 7) & 1;
        a->tri.lin_load  = v & 0x7F;
        break;
    case 0x400A:
        a->tri.timer_period = (a->tri.timer_period & 0x700) | v;
        break;
    case 0x400B:
        a->tri.timer_period = (a->tri.timer_period & 0xFF) | ((uint16_t)(v & 7) << 8);
        if (a->ch_en[2]) a->tri.len = len_table[v >> 3];
        a->tri.lin_reload = true;
        break;
    /* ── Noise ──────── */
    case 0x400C:
        a->noise.halt      = (v >> 5) & 1;
        a->noise.const_vol = (v >> 4) & 1;
        a->noise.vol       = v & 0xF;
        break;
    case 0x400E:
        a->noise.mode   = (v >> 7) & 1;
        a->noise.period = noise_period[v & 0xF];
        break;
    case 0x400F:
        if (a->ch_en[3]) a->noise.len = len_table[v >> 3];
        a->noise.env_start = true;
        break;
    /* ── DMC ─────────── */
    case 0x4010:
        a->dmc.irq_en   = (v >> 7) & 1;
        a->dmc.loop     = (v >> 6) & 1;
        a->dmc.rate_idx = v & 0xF;
        a->dmc.period   = dmc_rate[v & 0xF];
        if (!a->dmc.irq_en) a->dmc.irq_flag = false;
        break;
    case 0x4011:
        a->dmc.level = v & 0x7F;
        break;
    case 0x4012:
        a->dmc.start_addr = (uint16_t)(0xC000 + ((uint16_t)v << 6));
        break;
    case 0x4013:
        a->dmc.start_len = (uint16_t)((v << 4) + 1);
        break;
    /* ── Status ──────── */
    case 0x4015:
        for (int i = 0; i < 5; i++) {
            a->ch_en[i] = (v >> i) & 1;
            if (!a->ch_en[i]) {
                if (i < 2) a->pulse[i].len = 0;
                else if (i == 2) a->tri.len = 0;
                else if (i == 3) a->noise.len = 0;
                else { a->dmc.bytes_rem = 0; a->dmc.buf_empty = true; }
            } else if (i == 4 && a->dmc.bytes_rem == 0) {
                a->dmc.cur_addr  = a->dmc.start_addr;
                a->dmc.bytes_rem = a->dmc.start_len;
            }
        }
        a->dmc.irq_flag = false;
        break;
    /* ── Frame counter ─ */
    case 0x4017:
        a->fc_mode        = (v >> 7) & 1;
        a->fc_irq_inhibit = (v >> 6) & 1;
        a->fc_div         = 0;
        a->fc_step        = 0;
        if (a->fc_irq_inhibit) a->fc_irq = false;
        /* 5-step: immediately clock all units */
        if (a->fc_mode) clock_frame(a, true);
        break;
    }
    if (a->write_tap) a->write_tap(addr, v, a->write_tap_ud);
}

uint8_t apu2a03_read(Apu2a03 *a, uint16_t addr) {
    if (addr != 0x4015) return 0;
    uint8_t s = 0;
    if (a->pulse[0].len > 0) s |= 0x01;
    if (a->pulse[1].len > 0) s |= 0x02;
    if (a->tri.len      > 0) s |= 0x04;
    if (a->noise.len    > 0) s |= 0x08;
    if (a->dmc.bytes_rem> 0) s |= 0x10;
    if (a->fc_irq)           s |= 0x40;
    if (a->dmc.irq_flag)     s |= 0x80;
    a->fc_irq = false;
    return s;
}

/* ── SDL audio output ────────────────────────────────────────────────────── */

void apu2a03_flush(Apu2a03 *a) {
    if (!a->audio_dev || a->frame_n == 0) {
        a->frame_n = 0;
        return;
    }
    SDL_QueueAudio(a->audio_dev, a->frame_buf,
                   (Uint32)(a->frame_n * sizeof(float)));
    a->frame_n = 0;
}

/* ── Init / reset / destroy ──────────────────────────────────────────────── */

void apu2a03_reset(Apu2a03 *a) {
    /* Silence all channels, keep audio device and external hooks */
    SDL_AudioDeviceID dev = a->audio_dev;
    uint8_t (*mr)(uint16_t, void*) = a->mem_read;
    void *mu = a->mem_ud;
    void (*tap)(uint16_t, uint8_t, void*) = a->write_tap;
    void *tu = a->write_tap_ud;
    memset(a, 0, sizeof(*a));
    a->audio_dev    = dev;
    a->mem_read     = mr;
    a->mem_ud       = mu;
    a->write_tap    = tap;
    a->write_tap_ud = tu;
    a->noise.lfsr = 1;
    for (int i = 0; i < 8; i++) a->dmc.bits_rem = 8;
    a->dmc.bits_rem = 8;
    a->dmc.buf_empty = true;
    a->dmc.silence   = true;
}

bool apu2a03_init(Apu2a03 *a) {
    memset(a, 0, sizeof(*a));
    a->noise.lfsr    = 1;
    a->dmc.bits_rem  = 8;
    a->dmc.buf_empty = true;
    a->dmc.silence   = true;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "apu: SDL_InitSubSystem(AUDIO) failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_AudioSpec want = {
        .freq     = APU2A03_SAMPLE_RATE,
        .format   = AUDIO_F32SYS,
        .channels = 1,
        .samples  = 512,
    };
    SDL_AudioSpec have;
    a->audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!a->audio_dev) {
        fprintf(stderr, "apu: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    SDL_PauseAudioDevice(a->audio_dev, 0);
    return true;
}

void apu2a03_destroy(Apu2a03 *a) {
    if (a->audio_dev) {
        SDL_CloseAudioDevice(a->audio_dev);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        a->audio_dev = 0;
    }
}
