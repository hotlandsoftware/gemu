#ifndef _WIN32
#  define _POSIX_C_SOURCE 199309L
#endif
#include "um6578.h"
#include "rp2c02.h"  /* rp2c02_palette_rgb, reused for VNC's 64-colour table */
#include "gemu/screendump.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Input action table ─────────────────────────────────────────────────── */

/* Names/keys match nes_actions[] in machine_nes.c exactly (same physical
 * device, same gemu.ini [nes-controller] section — see .ini_section below). */
static const GemuActionDef um6578_actions[UM6578_NUM_ACTIONS] = {
    { "A",      GEMU_ACTION(UM6578_ACT_A),      "z"           },
    { "B",      GEMU_ACTION(UM6578_ACT_B),      "x"           },
    { "Select", GEMU_ACTION(UM6578_ACT_SELECT), "Right Shift" },
    { "Start",  GEMU_ACTION(UM6578_ACT_START),  "Return"      },
    { "Up",     GEMU_ACTION(UM6578_ACT_UP),     "Up"          },
    { "Down",   GEMU_ACTION(UM6578_ACT_DOWN),   "Down"        },
    { "Left",   GEMU_ACTION(UM6578_ACT_LEFT),   "Left"        },
    { "Right",  GEMU_ACTION(UM6578_ACT_RIGHT),  "Right"       },
};

/* MAME's nes_sh6578 INPUT_PORTS: bit0=Button2(B), bit1=Button1(A), bit2=Select,
 * bit3=Start, bit4=Up, bit5=Down, bit6=Left, bit7=Right — all IP_ACTIVE_HIGH
 * (1 = pressed, shifted out directly, no inversion). */
static uint8_t um6578_joypad_byte(uint32_t held) {
    uint8_t v = 0;
    if (held & GEMU_ACTION(UM6578_ACT_B))      v |= 0x01u;
    if (held & GEMU_ACTION(UM6578_ACT_A))      v |= 0x02u;
    if (held & GEMU_ACTION(UM6578_ACT_SELECT)) v |= 0x04u;
    if (held & GEMU_ACTION(UM6578_ACT_START))  v |= 0x08u;
    if (held & GEMU_ACTION(UM6578_ACT_UP))     v |= 0x10u;
    if (held & GEMU_ACTION(UM6578_ACT_DOWN))   v |= 0x20u;
    if (held & GEMU_ACTION(UM6578_ACT_LEFT))   v |= 0x40u;
    if (held & GEMU_ACTION(UM6578_ACT_RIGHT))  v |= 0x80u;
    return v;
}

/* ── CPU bus ─────────────────────────────────────────────────────────────── */

static uint8_t um6578_read(uint16_t addr, void *ud);
static void    um6578_write(uint16_t addr, uint8_t val, void *ud);

static uint8_t bank_read(Um6578State *s, int bank, uint16_t offset) {
    uint32_t address = (offset & 0x0FFFu) | ((uint32_t)s->bankswitch[bank] << 12);
    return address < s->rom_size ? s->rom[address] : 0xFFu;
}

static void bank_write(Um6578State *s, int bank, uint16_t offset, uint8_t val) {
    uint32_t address = (offset & 0x0FFFu) | ((uint32_t)s->bankswitch[bank] << 12);
    if (address < s->rom_size) s->rom[address] = val;
}

static void do_dma(Um6578State *s) {
    if (!(s->dma_control & 0x80)) return;

    uint16_t source = (uint16_t)(s->dma_source[0] | (s->dma_source[1] << 8));
    uint16_t dest   = (uint16_t)(s->dma_dest[0]   | (s->dma_dest[1]   << 8));
    uint16_t length = (uint16_t)(s->dma_length[0] | (s->dma_length[1] << 8));

    for (int i = 0; i <= (int)length; i++) {
        uint8_t data;
        if (source & 0x8000) {
            uint32_t trueaddr = (uint32_t)(source & 0x7FFFu) | ((uint32_t)(s->dma_bank & 0x1F) * 0x8000u);
            data = trueaddr < s->rom_size ? s->rom[trueaddr] : 0xFFu;
        } else {
            data = um6578_read((uint16_t)(source & 0x7FFFu), s);
        }

        if (s->dma_control & 0x20)
            um6578_write((uint16_t)dest, data, s);
        else
            s->ppu.vram[dest] = data;

        source++;
        dest++;
    }
}

static uint8_t um6578_read(uint16_t addr, void *ud) {
    Um6578State *s = ud;

    if (addr < 0x2000) return s->ram_lo[addr];
    if (addr >= 0x2000 && addr <= 0x2007) return ppu_sh6578_read(&s->ppu, (uint8_t)addr);
    if (addr == 0x2008) return ppu_sh6578_read_ext(&s->ppu);
    if (addr >= 0x2040 && addr <= 0x207F) return ppu_sh6578_palette_read(&s->ppu, (uint8_t)(addr - 0x2040));

    if (addr == 0x4015) return apu2a03_read(&s->apu, addr);
    if (addr == 0x4016) {
        uint8_t v = s->iolatch & 1u;
        s->iolatch >>= 1;
        return v;
    }
    if (addr == 0x4017) return 0; /* IN1: unused on every title we support */
    if (addr == 0x4026) return 0; /* EXT: unmodelled input, stubbed to 0 */
    if (addr == 0x4033) return 0; /* IRQ status: unimplemented upstream too */

    if (addr >= 0x4040 && addr <= 0x4047) return s->bankswitch[addr - 0x4040];
    if (addr >= 0x4048 && addr <= 0x404F) {
        switch (addr - 0x4048) {
        case 0: return s->dma_control & 0x7Fu;
        case 1: return s->dma_bank;
        case 2: return s->dma_source[0];
        case 3: return s->dma_source[1];
        case 4: return s->dma_dest[0];
        case 5: return s->dma_dest[1];
        case 6: return s->dma_length[0];
        case 7: return s->dma_length[1];
        }
    }

    if (addr >= 0x5000 && addr <= 0x7FFF) return s->ram_hi[addr - 0x5000];
    if (addr >= 0x8000) return bank_read(s, (addr - 0x8000) >> 12, addr);

    return 0xFF;
}

static void um6578_write(uint16_t addr, uint8_t val, void *ud) {
    Um6578State *s = ud;

    if (addr < 0x2000) { s->ram_lo[addr] = val; return; }
    if (addr >= 0x2000 && addr <= 0x2007) { ppu_sh6578_write(&s->ppu, (uint8_t)addr, val); return; }
    if (addr == 0x2008) { ppu_sh6578_write_ext(&s->ppu, val); return; }
    if (addr >= 0x2040 && addr <= 0x207F) { ppu_sh6578_palette_write(&s->ppu, (uint8_t)(addr - 0x2040), val); return; }

    if (addr == 0x4014) {
        uint8_t page[256];
        uint16_t base = (uint16_t)(val << 8);
        for (int i = 0; i < 256; i++) page[i] = um6578_read((uint16_t)(base + i), s);
        ppu_sh6578_oam_dma(&s->ppu, page);
        return;
    }
    if (addr == 0x4016) {
        bool has_pad = s->cfg->n_ports > 0 && s->cfg->ports[0] == NES_DEVICE_CONTROLLER;
        if ((s->prev_io & 1) && !(val & 1))
            s->iolatch = has_pad ? um6578_joypad_byte(s->held_actions) : 0;
        s->prev_io = val;
        return;
    }
    if (addr >= 0x4000 && addr <= 0x4017) { apu2a03_write(&s->apu, addr, val); return; }

    if (addr == 0x4020) return; /* timing setting control: stub */
    if (addr == 0x4026) return; /* EXT write: stub */
    if (addr == 0x4027) return; /* DAC data register: unmodelled */
    if (addr == 0x4031) return; /* startup protection sequence: logging-only upstream too */
    if (addr == 0x4032) {
        s->irqmask = val;
        if (val & 0x80) s->cpu.irq = false;
        return;
    }
    if (addr == 0x4034) {
        if ((val & 0x80) && (val & 0x20)) s->timer_armed = true;
        else { s->timer_armed = false; s->timer_scanlines_left = 0; }
        return;
    }
    if (addr == 0x4035) { s->timer_scanlines_left = val; return; }

    if (addr >= 0x4040 && addr <= 0x4047) { s->bankswitch[addr - 0x4040] = val; return; }
    if (addr >= 0x4048 && addr <= 0x404F) {
        switch (addr - 0x4048) {
        case 0: s->dma_control = val; do_dma(s); break;
        case 1: s->dma_bank = val; break;
        case 2: s->dma_source[0] = val; break;
        case 3: s->dma_source[1] = val; break;
        case 4: s->dma_dest[0] = val; break;
        case 5: s->dma_dest[1] = val; break;
        case 6: s->dma_length[0] = val; break;
        case 7: s->dma_length[1] = val; break;
        }
        return;
    }

    if (addr >= 0x5000 && addr <= 0x7FFF) { s->ram_hi[addr - 0x5000] = val; return; }
    if (addr >= 0x8000) { bank_write(s, (addr - 0x8000) >> 12, addr, val); return; }
}

static uint8_t um6578_apu_mem_read(uint16_t addr, void *ud) {
    return um6578_read(addr, ud);
}

/* ── Input: local display actions + VNC key -> action mapping ──────────── */

#define XK_Up     0xFF52u
#define XK_Down   0xFF54u
#define XK_Left   0xFF51u
#define XK_Right  0xFF53u
#define XK_Return 0xFF0Du
#define XK_Shift_R 0xFFE2u

static void um6578_handle_keys(Um6578State *s, uint32_t held) {
    if (s->display) {
        s->held_actions = held;
    } else if (!s->vnc) {
        s->held_actions = 0;
    }

    if (s->vnc) {
        GemuVncKeyEvent ev;
        while (gemu_vnc_pop_key_event(s->vnc, &ev)) {
            uint32_t bit = 0;
            switch (ev.keysym) {
            case 'z': case 'Z':  bit = GEMU_ACTION(UM6578_ACT_A);      break;
            case 'x': case 'X':  bit = GEMU_ACTION(UM6578_ACT_B);      break;
            case XK_Shift_R:     bit = GEMU_ACTION(UM6578_ACT_SELECT); break;
            case XK_Return:      bit = GEMU_ACTION(UM6578_ACT_START);  break;
            case XK_Up:          bit = GEMU_ACTION(UM6578_ACT_UP);     break;
            case XK_Down:        bit = GEMU_ACTION(UM6578_ACT_DOWN);   break;
            case XK_Left:        bit = GEMU_ACTION(UM6578_ACT_LEFT);   break;
            case XK_Right:       bit = GEMU_ACTION(UM6578_ACT_RIGHT);  break;
            default: break;
            }
            if (!bit) continue;
            if (ev.down) s->held_actions |= bit;
            else         s->held_actions &= ~bit;
        }
    }
}

/* ── Screendump ──────────────────────────────────────────────────────────── */

static bool um6578_screendump(void *ud, const char *path) {
    Um6578State *s = ud;
    int w = SH6578_WIDTH, h = SH6578_HEIGHT;
    uint8_t *rgb = malloc((size_t)w * (size_t)h * 3);
    if (!rgb) return false;
    for (int i = 0; i < w * h; i++) {
        uint32_t c = s->ppu.pixels_argb[i];
        rgb[i * 3 + 0] = (uint8_t)(c >> 16);
        rgb[i * 3 + 1] = (uint8_t)(c >> 8);
        rgb[i * 3 + 2] = (uint8_t)(c);
    }
    bool ok = gemu_screendump(path, rgb, w, h);
    free(rgb);
    return ok;
}

/* ── PPU/CPU cycle sync ──────────────────────────────────────────────────── */

static void um6578_sync_ppu(Um6578State *s, uint64_t cpu_cycle) {
    while (s->ppu_synced_cpu_cycle < cpu_cycle) {
        s->ppu_synced_cpu_cycle++;
        ppu_sh6578_tick(&s->ppu);
        if (s->ppu.nmi_pending) {
            s->cpu.nmi = true;
            s->ppu.nmi_pending = false;
        }
        if (s->ppu.dirty) {
            /* one scanline-count IRQ timer tick per PPU scanline crossed */
            if (s->timer_armed && s->timer_scanlines_left > 0) {
                s->timer_scanlines_left--;
                if (s->timer_scanlines_left == 0 && !(s->irqmask & 0x80))
                    s->cpu.irq = true;
            }
            return;
        }
    }
}

/* ── Create / destroy ────────────────────────────────────────────────────── */

Um6578State *um6578_create(const MosConfig *cfg) {
    Um6578State *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->cfg = cfg;

    const char *rom_path = NULL;
    for (int i = 0; i < cfg->n_roms; i++) {
        if (cfg->roms[i].region && !strcmp(cfg->roms[i].region, "maincpu")) {
            rom_path = cfg->roms[i].path;
            break;
        }
    }
    if (!rom_path) {
        fprintf(stderr, "um6578: missing ROM — use -rom roms/um6578\n");
        free(s);
        return NULL;
    }

    FILE *f = fopen(rom_path, "rb");
    if (!f) {
        fprintf(stderr, "um6578: cannot open '%s'\n", rom_path);
        free(s);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || (uint32_t)sz > UM6578_ROM_MAX) {
        fprintf(stderr, "um6578: '%s' has invalid size %ld\n", rom_path, sz);
        fclose(f);
        free(s);
        return NULL;
    }
    s->rom = malloc((size_t)sz);
    if (!s->rom || fread(s->rom, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "um6578: read error on '%s'\n", rom_path);
        fclose(f);
        free(s->rom);
        free(s);
        return NULL;
    }
    fclose(f);
    s->rom_size = (uint32_t)sz;

    ppu_sh6578_init(&s->ppu, UM6578_CPU_HZ, UM6578_REFRESH_HZ, UM6578_LINES_TOTAL, UM6578_VBLANK_LINE);

    mos6502_init(&s->cpu);
    s->cpu.mem_read  = um6578_read;
    s->cpu.mem_write = um6578_write;
    s->cpu.mem_ud    = s;
    /* real M6502 core on this hardware — decimal mode stays enabled
     * (decimal_disable defaults false), unlike the NES's 2A03. */

    s->monitor = gemu_monitor_create();
    if (!s->monitor) { free(s->rom); free(s); return NULL; }
    gemu_monitor_set_screendump_cb(s->monitor, um6578_screendump, s);

    if (cfg->vnc_addr) {
        s->vnc = gemu_vnc_create(cfg->vnc_addr, SH6578_WIDTH, SH6578_HEIGHT);
        if (s->vnc)
            gemu_vnc_set_palette(s->vnc, rp2c02_palette_rgb, 64);
        else
            fprintf(stderr, "um6578: failed to start VNC at %s\n", cfg->vnc_addr);
    }
    gemu_monitor_set_vnc(s->monitor, s->vnc);

    if (cfg->sound == MOS_SOUND_2A03) {
        if (!apu2a03_init(&s->apu, (uint32_t)(UM6578_CPU_HZ + 0.5)))
            fprintf(stderr, "um6578: APU audio init failed (continuing silently)\n");
        s->apu.mem_read = um6578_apu_mem_read;
        s->apu.mem_ud   = s;
    }

    if (cfg->display_type != GEMU_DISPLAY_NONE) {
        /* Same physical device as NES's standard controller — share its
         * gemu.ini section (and the "nes-controller" heading shown at the
         * top of the Tab key-binding overlay) rather than inventing a
         * separate, machine-specific one. */
        bool has_pad = cfg->n_ports > 0 && cfg->ports[0] == NES_DEVICE_CONTROLLER;
        s->display = gemu_display_create(cfg->display_type, &(GemuDisplayConfig){
            .title       = "GEMU",
            .fb_width    = SH6578_WIDTH,
            .fb_height   = SH6578_HEIGHT,
            .scale       = cfg->display_scale,
            .renderer    = cfg->display_renderer,
            .actions     = um6578_actions,
            .n_actions   = UM6578_NUM_ACTIONS,
            .ini_section = has_pad ? "nes-controller" : "um6578",
        });
    }

    for (int i = 0; i < 8; i++) s->bankswitch[i] = (uint8_t)i;
    mos6502_reset(&s->cpu);
    return s;
}

void um6578_destroy(Um6578State *s) {
    if (!s) return;
    apu2a03_destroy(&s->apu);
    gemu_monitor_destroy(s->monitor);
    gemu_display_destroy(s->display);
    gemu_vnc_destroy(s->vnc);
    free(s->rom);
    free(s);
}

/* ── Run loop ────────────────────────────────────────────────────────────── */

void um6578_run(Um6578State *s, const MosConfig *cfg) {
    gemu_monitor_start(s->monitor);

    bool quit = false;
    while (!quit) {
        Uint32 t0 = SDL_GetTicks();

        uint32_t held = 0;
        if (s->display) {
            held = gemu_display_poll(s->display);
            if (gemu_display_should_quit(s->display)) break;
            if (gemu_display_reset_requested(s->display)) {
                gemu_display_clear_flags(s->display);
                for (int i = 0; i < 8; i++) s->bankswitch[i] = (uint8_t)i;
                mos6502_reset(&s->cpu);
            }
        }

        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if (cmd == GEMU_MON_QUIT) {
                if (cfg->no_shutdown) gemu_monitor_shutdown_or_pause(s->monitor, true);
                else { quit = true; break; }
            } else if (cmd == GEMU_MON_RESET) {
                for (int i = 0; i < 8; i++) s->bankswitch[i] = (uint8_t)i;
                mos6502_reset(&s->cpu);
            } else if (cmd == GEMU_MON_CUSTOM) {
                gemu_monitor_unknown_command(s->monitor);
            }
        }
        if (quit) break;
        if (s->display) gemu_display_set_paused(s->display, gemu_monitor_is_paused(s->monitor));

        um6578_handle_keys(s, held);

        if (!gemu_monitor_is_paused(s->monitor)) {
            s->ppu.dirty = false;
            while (!s->ppu.dirty) {
                if (gemu_monitor_check_exec(s->monitor, s->cpu.PC)) break;
                uint64_t prev = s->cpu.cycle_count;
                mos6502_step(&s->cpu);
                uint64_t delta = s->cpu.cycle_count - prev;
                for (uint64_t i = 0; i < delta; i++)
                    if (s->apu.audio_dev) apu2a03_tick(&s->apu);
                if (s->apu.fc_irq || s->apu.dmc.irq_flag)
                    s->cpu.irq = true;
                um6578_sync_ppu(s, s->cpu.cycle_count);
                if (gemu_monitor_is_paused(s->monitor)) break;
            }
            apu2a03_flush(&s->apu);
            s->frame++;
        }

        if (s->display)
            gemu_display_render(s->display, s->ppu.pixels_argb, SH6578_WIDTH, SH6578_HEIGHT);
        if (s->vnc)
            gemu_vnc_update(s->vnc, s->ppu.pixels, SH6578_WIDTH, SH6578_HEIGHT);

        Uint32 dt = SDL_GetTicks() - t0;
        Uint32 frame_ms = 1000u / 60u;
        if (dt < frame_ms) SDL_Delay(frame_ms - dt);
    }

    gemu_monitor_stop(s->monitor);
    printf("um6578: %llu frames\n", (unsigned long long)s->frame);
}
