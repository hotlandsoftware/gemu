/* NES mappers 24 and 26: Konami VRC6a and VRC6b (26 swaps A0 and A1),
 * with their expansion audio: two pulse channels and a sawtooth. */
#include "cart/nes_cart_internal.h"
#include <stdio.h>
#include <string.h>

static void vrc6_reset(NesCart *c) {
    c->prg_off[0] = 0;
    c->prg_off[1] = 0x2000u;
    c->prg_off[2] = 0;
    c->prg_off[3] = (nc_prg_8k(c) - 1u) * 0x2000u;   /* $E000: the last 8KB */
    memset(c->chr_off, 0, sizeof(c->chr_off));
    c->m.vrc6 = (NesVrc6){0};
    c->irq = false;
}

/* One CPU cycle: the IRQ counter and the audio timers.  Returns the
 * expansion audio level. */
static float vrc6_tick(NesCart *c) {
    NesVrc6 *v = &c->m.vrc6;
    /* IRQ: a prescaler of 341 CPU cycles in scanline mode, every cycle in CPU mode */
    if (v->irq_enabled) {
        bool fire = false;
        if (!v->irq_mode) {
            if (++v->prescaler >= 341) { v->prescaler = 0; fire = true; }
        } else {
            fire = true;
        }
        if (fire) {
            if (v->irq_counter == 0xFF) {
                v->irq_counter = v->irq_latch;
                nc_irq_raise(c);
            } else {
                v->irq_counter++;
            }
        }
    }

    if (v->halt) return 0.0f;

    float p1 = 0.0f;
    if (v->p1_en) {
        if (v->p1_timer == 0) {
            v->p1_timer = v->p1_period;
            v->p1_seq   = (v->p1_seq + 1u) & 15u;
        } else {
            v->p1_timer--;
        }
        uint8_t vol = v->p1_ctrl & 0x0Fu;
        if ((v->p1_ctrl & 0x80u) || v->p1_seq <= ((v->p1_ctrl >> 4) & 7u))
            p1 = (float)vol;
    }

    float p2 = 0.0f;
    if (v->p2_en) {
        if (v->p2_timer == 0) {
            v->p2_timer = v->p2_period;
            v->p2_seq   = (v->p2_seq + 1u) & 15u;
        } else {
            v->p2_timer--;
        }
        uint8_t vol = v->p2_ctrl & 0x0Fu;
        if ((v->p2_ctrl & 0x80u) || v->p2_seq <= ((v->p2_ctrl >> 4) & 7u))
            p2 = (float)vol;
    }

    /* Sawtooth: six additions per 14-clock cycle (even steps 2-12) */
    float saw = 0.0f;
    if (v->saw_en) {
        if (v->saw_timer == 0) {
            v->saw_timer = v->saw_period;
            v->saw_step++;
            if (v->saw_step >= 14u) {
                v->saw_step  = 0;
                v->saw_accum = 0;
            } else if ((v->saw_step & 1u) == 0u) {
                v->saw_accum += v->saw_rate;
            }
        } else {
            v->saw_timer--;
        }
        saw = (float)(v->saw_accum >> 3);
    }

    /* The pulses mix like the 2A03's (volume 0-15 in the same units); the
     * sawtooth is scaled to about one 2A03 pulse at full volume. */
    float pulse = (p1 + p2 > 0.0f) ? 95.88f / (8128.0f / (p1 + p2) + 100.0f) : 0.0f;
    return pulse + saw * (0.15f / 31.0f);
}

static bool vrc6_write(NesCart *c, uint16_t addr, uint8_t val) {
    if (addr < 0x8000) return false;
    NesVrc6 *v = &c->m.vrc6;
    if (c->ops == &nes_mapper_vrc6b) {   /* A0 and A1 swapped */
        uint8_t a01 = addr & 3u;
        addr = (uint16_t)((addr & ~3u) | ((a01 & 1u) << 1) | ((a01 >> 1) & 1u));
    }
    uint32_t prg_8k = nc_prg_8k(c);
    uint32_t chr_1k = nc_chr_1k(c);

    if (addr < 0x9000) {
        /* $8000: 16KB at $8000-$BFFF (bits 3:0) */
        uint32_t bank = (uint32_t)(val & 0x0Fu) % c->info.prg_banks;
        c->prg_off[0] = bank * 0x4000u;
        c->prg_off[1] = bank * 0x4000u + 0x2000u;
    } else if (addr < 0xA000) {
        switch (addr & 3u) {
        case 0: v->p1_ctrl   = val; break;
        case 1: v->p1_period = (uint16_t)((v->p1_period & 0xFF00u) | val); break;
        case 2:
            v->p1_period = (uint16_t)((v->p1_period & 0x00FFu) | ((uint16_t)(val & 0x0Fu) << 8));
            v->p1_en     = (val >> 7) & 1;
            break;
        case 3: v->halt = val & 1; break;
        }
    } else if (addr < 0xB000) {
        switch (addr & 3u) {
        case 0: v->p2_ctrl   = val; break;
        case 1: v->p2_period = (uint16_t)((v->p2_period & 0xFF00u) | val); break;
        case 2:
            v->p2_period = (uint16_t)((v->p2_period & 0x00FFu) | ((uint16_t)(val & 0x0Fu) << 8));
            v->p2_en     = (val >> 7) & 1;
            break;
        }
    } else if (addr < 0xC000) {
        switch (addr & 3u) {
        case 0: v->saw_rate   = val & 0x3Fu; break;
        case 1: v->saw_period = (uint16_t)((v->saw_period & 0xFF00u) | val); break;
        case 2:
            v->saw_period = (uint16_t)((v->saw_period & 0x00FFu) | ((uint16_t)(val & 0x0Fu) << 8));
            v->saw_en     = (val >> 7) & 1;
            break;
        case 3:
            nc_mirror_vhab(c, (val >> 2) & 3u);
            break;
        }
    } else if (addr < 0xD000) {
        /* $C000: 8KB at $C000-$DFFF (bits 4:0) */
        if ((addr & 3u) == 0)
            c->prg_off[2] = ((uint32_t)(val & 0x1Fu) % prg_8k) * 0x2000u;
    } else if (addr < 0xE000) {
        /* $D000-$D003: 1KB CHR banks at PPU $0000-$0FFF */
        if (chr_1k) c->chr_off[addr & 3u] = ((uint32_t)val % chr_1k) * 0x400u;
    } else if (addr < 0xF000) {
        /* $E000-$E003: 1KB CHR banks at PPU $1000-$1FFF */
        if (chr_1k) c->chr_off[4u + (addr & 3u)] = ((uint32_t)val % chr_1k) * 0x400u;
    } else {
        /* $F000-$F002: IRQ */
        switch (addr & 3u) {
        case 0: v->irq_latch = val; break;
        case 1:
            v->irq_after   = val & 1u;
            v->irq_enabled = (val >> 1) & 1u;
            v->irq_mode    = (val >> 2) & 1u;
            if (v->irq_enabled) {
                v->irq_counter = v->irq_latch;
                v->prescaler   = 0;
            } else {
                nc_irq_ack(c);
            }
            break;
        case 2:
            nc_irq_ack(c);
            v->irq_counter = v->irq_latch;
            v->irq_enabled = v->irq_after;
            v->prescaler   = 0;
            break;
        }
    }
    return true;
}

static bool always(const NesCart *c) { (void)c; return true; }

static void vrc6_dump(const NesCart *c, char *buf, size_t len) {
    const NesVrc6 *v = &c->m.vrc6;
    snprintf(buf, len, "irq latch=%02X counter=%02X enabled=%d after=%d mode=%d prescaler=%d\n",
             v->irq_latch, v->irq_counter, v->irq_enabled, v->irq_after, v->irq_mode,
             v->prescaler);
}

const NesMapper nes_mapper_vrc6a = {
    24, "VRC6a", .reset = vrc6_reset, .write = vrc6_write, .prg_ram = always,
    .tick = vrc6_tick, .dump = vrc6_dump,
};

const NesMapper nes_mapper_vrc6b = {
    26, "VRC6b", .reset = vrc6_reset, .write = vrc6_write, .prg_ram = always,
    .tick = vrc6_tick, .dump = vrc6_dump,
};
