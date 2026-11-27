/* The 2A03 APU's save state: see apu2a03_state.h. */
#include "apu2a03_state.h"
#include "gemu/dev/state.h"
#include <stdio.h>

#define M(member) (ok &= GEMU_STATE_MEMBER(d, prefix, x, member))
/* A member of a nested struct, under "<prefix><sub>.<member>". */
#define S(sub, member) (ok &= GEMU_STATE_MEMBER(d, sp, &x->sub, member))

/* The two pulse channels: every field of ApuPulse (packed). */
static const GemuStateField pulse_fields[] = {
    GEMU_STATE_FIELD(ApuPulse, duty),         GEMU_STATE_FIELD(ApuPulse, seq),
    GEMU_STATE_FIELD(ApuPulse, halt),         GEMU_STATE_FIELD(ApuPulse, const_vol),
    GEMU_STATE_FIELD(ApuPulse, vol),
    GEMU_STATE_FIELD(ApuPulse, sweep_en),     GEMU_STATE_FIELD(ApuPulse, sweep_neg),
    GEMU_STATE_FIELD(ApuPulse, sweep_reload),
    GEMU_STATE_FIELD(ApuPulse, sweep_period), GEMU_STATE_FIELD(ApuPulse, sweep_shift),
    GEMU_STATE_FIELD(ApuPulse, sweep_div),
    GEMU_STATE_FIELD(ApuPulse, timer_period), GEMU_STATE_FIELD(ApuPulse, timer),
    GEMU_STATE_FIELD(ApuPulse, len),
    GEMU_STATE_FIELD(ApuPulse, env_div),      GEMU_STATE_FIELD(ApuPulse, env_dec),
    GEMU_STATE_FIELD(ApuPulse, env_start),
};

/* Apu2a03, in order, into the channel structs.  Excluded:
 *   frame_buf, frame_n   output samples waiting for apu2a03_flush() to queue
 *                        them to the host audio device: not machine state
 *                        (a load drops at most one frame's worth of sound)
 *   clock_pps            cycles per output sample: worked out from the CPU
 *                        clock (apu2a03_init) and the NesPins clock setting;
 *                        the machine reapplies it (nes_pins_apply)
 *   audio_dev            the host audio device
 *   no_sound_in, no_sound_out
 *                        follow the NesPins debugging setting, which isn't
 *                        machine state; the machine reapplies them
 *   mem_read, mem_ud, write_tap, write_tap_ud
 *                        callbacks and their contexts: the machine's wiring
 * Registered: every channel field, the frame counter, cycle and apu_clk,
 * fds_in (the expansion input, rewritten before each tick), the
 * compatibility switches (settings: the same values on the same machine)
 * and sample_acc - the resampler's phase, which decides on which cycles
 * samples are taken, so the output after a load is the same sample for
 * sample. */
bool apu2a03_state_add(GemuDevice *d, const char *prefix, Apu2a03 *x) {
    bool ok = true;
    char sp[64], name[64];
    const char *p = prefix ? prefix : "";

    snprintf(name, sizeof name, "%spulse", p);
    ok &= GEMU_STATE_TABLE(d, name, x->pulse, pulse_fields);

    snprintf(sp, sizeof sp, "%stri.", p);
    S(tri, halt); S(tri, lin_load); S(tri, lin); S(tri, lin_reload);
    S(tri, timer_period); S(tri, timer); S(tri, seq); S(tri, len);

    snprintf(sp, sizeof sp, "%snoise.", p);
    S(noise, halt); S(noise, const_vol); S(noise, vol); S(noise, mode);
    S(noise, period); S(noise, timer); S(noise, lfsr); S(noise, len);
    S(noise, env_div); S(noise, env_dec); S(noise, env_start);

    snprintf(sp, sizeof sp, "%sdmc.", p);
    S(dmc, irq_en); S(dmc, loop); S(dmc, rate_idx); S(dmc, level);
    S(dmc, start_addr); S(dmc, start_len); S(dmc, cur_addr); S(dmc, bytes_rem);
    S(dmc, sample_buf); S(dmc, buf_empty); S(dmc, shift); S(dmc, bits_rem);
    S(dmc, silence); S(dmc, period); S(dmc, timer); S(dmc, irq_flag);
    S(dmc, dma_pending); S(dmc, dma_abort); S(dmc, start_delay);

    M(ch_en);
    M(fc_mode); M(fc_irq_inhibit); M(fc_irq); M(fc_div); M(fc_step);
    M(fc_write_delay); M(fc_new); M(fc_clear_pending); M(dma_external);
    M(cycle); M(apu_clk);
    M(sample_acc);
    M(fds_in);
    M(sweep_enabled); M(dmc_enabled); M(frame_irq_enabled);
    return ok;
}
