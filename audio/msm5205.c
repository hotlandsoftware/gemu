/*
 * OKI MSM5205 - see msm5205.h.  The step table and the step adaptation
 * are OKI's (the same ADPCM as the MSM6258/MSM6295); the difference for a
 * code is the sum of step, step/2, step/4 for its magnitude bits plus
 * step/8, as the chip's adder computes it.
 */
#include "msm5205.h"
#include <math.h>

static int  diff_table[49 * 16];
static bool tables_built;

static void build_tables(void) {
    static const int bits[16][4] = {
        { 1, 0, 0, 0 }, { 1, 0, 0, 1 }, { 1, 0, 1, 0 }, { 1, 0, 1, 1 },
        { 1, 1, 0, 0 }, { 1, 1, 0, 1 }, { 1, 1, 1, 0 }, { 1, 1, 1, 1 },
        { -1, 0, 0, 0 }, { -1, 0, 0, 1 }, { -1, 0, 1, 0 }, { -1, 0, 1, 1 },
        { -1, 1, 0, 0 }, { -1, 1, 0, 1 }, { -1, 1, 1, 0 }, { -1, 1, 1, 1 },
    };
    for (int step = 0; step < 49; step++) {
        int s = (int)floor(16.0 * pow(11.0 / 10.0, (double)step));   /* 16, 17, 19 ... 1552 */
        for (int n = 0; n < 16; n++)
            diff_table[step * 16 + n] = bits[n][0] *
                (s * bits[n][1] + s / 2 * bits[n][2] + s / 4 * bits[n][3] + s / 8);
    }
    tables_built = true;
}

void msm5205_init(Msm5205 *m) {
    if (!tables_built) build_tables();
    m->signal = 0;
    m->step = 0;
    m->reset = false;
    m->bits3 = false;
}

void msm5205_set_reset(Msm5205 *m, bool reset) {
    m->reset = reset;
    if (reset) { m->signal = 0; m->step = 0; }
}

void msm5205_vck(Msm5205 *m, uint8_t code) {
    static const int adjust[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
    if (m->reset) return;
    unsigned n = m->bits3 ? (unsigned)(code & 7u) << 1 : code & 15u;
    int s = m->signal + diff_table[m->step * 16 + (int)n];
    if (s > 2047) s = 2047;
    if (s < -2048) s = -2048;
    m->signal = (int16_t)s;
    int st = m->step + adjust[n & 7u];
    m->step = st < 0 ? 0 : st > 48 ? 48 : st;
}

float msm5205_output(const Msm5205 *m) {
    /* the 10-bit DAC takes the signal's top bits */
    return m->reset ? 0.0f : (float)(m->signal >> 2) / 512.0f;
}
