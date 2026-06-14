#include "nes.h"
#include "fds.h"
#include "fds_hle.h"
#include "../vga/nes_display.h"
#include "../audio/apu2a03.h"
#include "gemu/memory.h"
#include "gemu/screendump.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef _WIN32
#  include <direct.h>
#  define gemu_mkdir(p)   _mkdir(p)
#  define strcasecmp      _stricmp
#  define strncasecmp     _strnicmp
#else
#  include <sys/stat.h>
#  include <strings.h>
#  define gemu_mkdir(p) mkdir((p), 0755)
#endif
#ifdef GEMU_GTK
#  include <gtk/gtk.h>
#  include "gemu/gtk_menu.h"
#endif

/* ── Battery-backed SRAM persistence ────────────────────────────────────── */

static void nes_game_basename(const char *path, char *out, size_t len) {
    const char *name = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') name = p + 1;
    const char *dot = strrchr(name, '.');
    size_t n = dot ? (size_t)(dot - name) : strlen(name);
    if (n >= len) n = len - 1;
    memcpy(out, name, n);
    out[n] = '\0';
}

static void nes_build_sav_path(const char *game, char *out, size_t len) {
#ifdef _WIN32
    const char *base = getenv("LOCALAPPDATA");
    if (!base || !base[0]) base = getenv("APPDATA");
    if (!base || !base[0]) base = "C:\\Users\\Default\\AppData\\Local";
    snprintf(out, len, "%s\\gemu\\%s.sav", base, game);
#else
    const char *home = getenv("HOME");
    if (!home || !home[0]) home = "/tmp";
    snprintf(out, len, "%s/.gemu/%s.sav", home, game);
#endif
}

static void nes_ensure_sav_dir(const char *sav_path) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", sav_path);
    char *sep = strrchr(dir, '/');
#ifdef _WIN32
    char *sep2 = strrchr(dir, '\\');
    if (sep2 > sep) sep = sep2;
#endif
    if (sep) { *sep = '\0'; gemu_mkdir(dir); }
}

static bool nes_battery_prompt(void) {
#ifdef GEMU_GTK
    GtkWidget *dlg = gtk_message_dialog_new(
        NULL, GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        "This game has a battery.\n"
        "Do you want GEMU to save data automatically?");
    gtk_window_set_title(GTK_WINDOW(dlg), "Battery-backed Save");
    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    while (gtk_events_pending()) gtk_main_iteration_do(FALSE);
    return resp == GTK_RESPONSE_YES;
#else
    SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Yes" },
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "No"  },
    };
    SDL_MessageBoxData data = {
        .flags       = SDL_MESSAGEBOX_INFORMATION,
        .window      = NULL,
        .title       = "Battery-backed Save",
        .message     = "This game has a battery.\n"
                       "Do you want GEMU to save data automatically?",
        .numbuttons  = 2,
        .buttons     = buttons,
        .colorScheme = NULL,
    };
    int choice = 0;
    SDL_ShowMessageBox(&data, &choice);
    return choice == 1;
#endif
}

static void nes_sav_load(NesState *s) {
    FILE *f = fopen(s->sav_path, "rb");
    if (!f) return;
    fread(s->prg_ram, 1, sizeof(s->prg_ram), f);
    fclose(f);
    printf("nes: loaded save '%s'\n", s->sav_path);
}

static void nes_sav_save(NesState *s) {
    if (!s->battery_autosave || !s->sav_path[0]) return;
    nes_ensure_sav_dir(s->sav_path);
    FILE *f = fopen(s->sav_path, "wb");
    if (!f) { fprintf(stderr, "nes: cannot write save '%s'\n", s->sav_path); return; }
    fwrite(s->prg_ram, 1, sizeof(s->prg_ram), f);
    fclose(f);
    printf("nes: saved to '%s'\n", s->sav_path);
}

static void nes_battery_setup(NesState *s) {
    if (!s->cart.has_battery) return;
    char game[256];
    nes_game_basename(s->cart_path_buf, game, sizeof(game));
    nes_build_sav_path(game, s->sav_path, sizeof(s->sav_path));
    FILE *existing = fopen(s->sav_path, "rb");
    if (existing) {
        fclose(existing);
        s->battery_autosave = true;
        nes_sav_load(s);
    } else {
        s->battery_autosave = nes_battery_prompt();
    }
}

/* ── iNES cartridge loading ──────────────────────────────────────────────── */

static bool ines_load(NesState *s, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "nes: cannot open '%s'\n", path); return false; }

    uint8_t hdr[16];
    if (fread(hdr, 1, 16, f) != 16 ||
        hdr[0] != 'N' || hdr[1] != 'E' || hdr[2] != 'S' || hdr[3] != 0x1A) {
        fprintf(stderr, "nes: '%s' is not a valid iNES file\n", path);
        fclose(f); return false;
    }

    s->cart.prg_banks   = hdr[4];
    s->cart.chr_banks   = hdr[5];
    s->cart.mapper      = (hdr[7] & 0xF0) | (hdr[6] >> 4);
    s->cart.has_battery = (hdr[6] >> 1) & 1;

    bool four_screen = (hdr[6] >> 3) & 1;
    bool vertical    = hdr[6] & 1;
    if      (four_screen) s->cart.mirror = RP2C02_MIRROR_4SCREEN;
    else if (vertical)    s->cart.mirror = RP2C02_MIRROR_VERTICAL;
    else                  s->cart.mirror = RP2C02_MIRROR_HORIZONTAL;

    switch (s->cart.mapper) {
    case 0: case 1: case 2: case 4: case 66: case 178: case 228: break;
    default:
        fprintf(stderr, "nes: mapper %u not supported (NROM/0, MMC1/1, UxROM/2, MMC3/4, GxROM/66, WaixingFS/178, Action52/228)\n",
                s->cart.mapper);
        fclose(f); return false;
    }
    if (s->cart.prg_banks == 0) {
        fprintf(stderr, "nes: invalid PRG bank count 0\n");
        fclose(f); return false;
    }
    if (s->cart.mapper == 0 && s->cart.prg_banks > 2) {
        fprintf(stderr, "nes: NROM requires 1 or 2 PRG banks, got %u\n", s->cart.prg_banks);
        fclose(f); return false;
    }

    /* Skip optional 512-byte trainer */
    if ((hdr[6] >> 2) & 1) fseek(f, 512, SEEK_CUR);

    uint32_t prg_bytes = (uint32_t)s->cart.prg_banks * 0x4000;
    s->prg = malloc(prg_bytes);
    if (!s->prg || fread(s->prg, 1, prg_bytes, f) != prg_bytes) {
        fprintf(stderr, "nes: failed to read PRG ROM\n");
        fclose(f); free(s->prg); s->prg = NULL; return false;
    }

    if (s->cart.chr_banks > 0) {
        uint32_t chr_bytes = (uint32_t)s->cart.chr_banks * 0x2000;
        s->chr = malloc(chr_bytes);
        if (!s->chr || fread(s->chr, 1, chr_bytes, f) != chr_bytes) {
            fprintf(stderr, "nes: failed to read CHR ROM\n");
            fclose(f); free(s->chr); s->chr = NULL; free(s->prg); return false;
        }
        s->chr_is_ram = false;
    } else {
        /* CHR RAM: 8 KB writable by PPU */
        s->chr = calloc(1, 0x2000);
        if (!s->chr) { fclose(f); free(s->prg); return false; }
        s->chr_is_ram = true;
        s->cart.chr_banks = 1;
    }

    fclose(f);
    snprintf(s->cart_path_buf, sizeof(s->cart_path_buf), "%s", path);
    printf("nes: loaded '%s' — mapper %u, %u×16KB PRG, %u×8KB CHR%s\n",
           path, s->cart.mapper,
           s->cart.prg_banks, s->cart.chr_banks,
           s->chr_is_ram ? " (RAM)" : "");
    return true;
}

/* ── Mapper 1 (MMC1/SxROM) ───────────────────────────────────────────────── */

static void mmc1_update_banks(NesState *s) {
    uint8_t  prg_mode  = (s->mmc1_ctrl >> 2) & 3;
    uint8_t  chr_mode  = (s->mmc1_ctrl >> 4) & 1;
    uint8_t  prg_bank  =  s->mmc1_prg  & 0x0F;
    uint32_t prg_size  = (uint32_t)s->cart.prg_banks * 0x4000u;
    uint32_t prg_last  = prg_size - 0x4000u;
    uint32_t chr_size  = (uint32_t)s->cart.chr_banks * 0x2000u;

    switch (prg_mode) {
    case 0: case 1:   /* 32 KB: switch both halves together */
        s->prg_offsets[0] = ((uint32_t)(prg_bank & ~1u) * 0x4000u) % prg_size;
        s->prg_offsets[1] =  s->prg_offsets[0] + 0x4000u;
        break;
    case 2:           /* fix first bank at $8000, switch $C000 */
        s->prg_offsets[0] = 0;
        s->prg_offsets[1] = ((uint32_t)prg_bank * 0x4000u) % prg_size;
        break;
    case 3:           /* switch $8000, fix last bank at $C000 */
        s->prg_offsets[0] = ((uint32_t)prg_bank * 0x4000u) % prg_size;
        s->prg_offsets[1] = prg_last;
        break;
    }

    if (chr_mode == 0) {   /* 8 KB: one window */
        s->chr_offsets[0] = ((uint32_t)(s->mmc1_chr0 & ~1u) * 0x1000u) % chr_size;
        s->chr_offsets[1] =  s->chr_offsets[0] + 0x1000u;
    } else {               /* 4 KB x 2: independent windows */
        s->chr_offsets[0] = ((uint32_t)s->mmc1_chr0 * 0x1000u) % chr_size;
        s->chr_offsets[1] = ((uint32_t)s->mmc1_chr1 * 0x1000u) % chr_size;
    }

    static const uint8_t mirror_map[] = {
        RP2C02_MIRROR_SINGLE_A, RP2C02_MIRROR_SINGLE_B,
        RP2C02_MIRROR_VERTICAL, RP2C02_MIRROR_HORIZONTAL
    };
    s->ppu.mirror = mirror_map[s->mmc1_ctrl & 3];
}

static void mmc1_serial_write(NesState *s, uint16_t addr, uint8_t val) {
    if (val & 0x80) {          /* reset: force PRG mode 3 */
        s->mmc1_shift       = 0;
        s->mmc1_shift_count = 0;
        s->mmc1_ctrl       |= 0x0C;
        mmc1_update_banks(s);
        return;
    }
    /* Shift bit 0 in from the MSB side; after 5 writes the register is full. */
    s->mmc1_shift = (s->mmc1_shift >> 1) | ((val & 1u) << 4);
    if (++s->mmc1_shift_count < 5) return;

    uint8_t reg = (addr >> 13) & 3;
    switch (reg) {
    case 0: s->mmc1_ctrl = s->mmc1_shift; break;
    case 1: s->mmc1_chr0 = s->mmc1_shift; break;
    case 2: s->mmc1_chr1 = s->mmc1_shift; break;
    case 3: s->mmc1_prg  = s->mmc1_shift; break;
    }
    s->mmc1_shift       = 0;
    s->mmc1_shift_count = 0;
    mmc1_update_banks(s);
}

/* ── Mapper 4 (MMC3/TxROM) ───────────────────────────────────────────────── */

static void mmc3_update_banks(NesState *s) {
    uint32_t prg_8k   = (uint32_t)s->cart.prg_banks * 2;   /* total 8 KB banks */
    uint32_t chr_1k   = (uint32_t)s->cart.chr_banks * 8;   /* total 1 KB banks */

    /* PRG: four 8 KB windows at $8000, $A000, $C000, $E000 */
    uint8_t  prg_mode = (s->mmc3_bank_sel >> 6) & 1;
    uint32_t r6 = (s->mmc3_regs[6] % prg_8k) * 0x2000u;
    uint32_t r7 = (s->mmc3_regs[7] % prg_8k) * 0x2000u;
    uint32_t f0 = (prg_8k - 2) * 0x2000u;   /* second-to-last fixed bank */
    uint32_t f1 = (prg_8k - 1) * 0x2000u;   /* last fixed bank */

    if (!prg_mode) {
        s->mmc3_prg_offsets[0] = r6;  s->mmc3_prg_offsets[1] = r7;
        s->mmc3_prg_offsets[2] = f0;  s->mmc3_prg_offsets[3] = f1;
    } else {
        s->mmc3_prg_offsets[0] = f0;  s->mmc3_prg_offsets[1] = r7;
        s->mmc3_prg_offsets[2] = r6;  s->mmc3_prg_offsets[3] = f1;
    }

    /* CHR: eight 1 KB windows at PPU $0000–$1FFF.
     * R0/R1 select 2 KB pairs (bit 0 ignored); R2–R5 select 1 KB banks.
     * CHR-invert bit swaps which half gets the 2 KB banks. */
    if (s->chr_is_ram || chr_1k == 0) return;

    uint8_t inv = (s->mmc3_bank_sel >> 7) & 1;
    uint32_t c[8];
    c[0] = ((s->mmc3_regs[0] & 0xFEu)      % chr_1k) * 0x400u;
    c[1] = (((s->mmc3_regs[0] & 0xFEu) + 1) % chr_1k) * 0x400u;
    c[2] = ((s->mmc3_regs[1] & 0xFEu)      % chr_1k) * 0x400u;
    c[3] = (((s->mmc3_regs[1] & 0xFEu) + 1) % chr_1k) * 0x400u;
    c[4] = (s->mmc3_regs[2] % chr_1k) * 0x400u;
    c[5] = (s->mmc3_regs[3] % chr_1k) * 0x400u;
    c[6] = (s->mmc3_regs[4] % chr_1k) * 0x400u;
    c[7] = (s->mmc3_regs[5] % chr_1k) * 0x400u;

    if (!inv) {
        /* 2 KB at $0000, 2 KB at $0800, 1 KB×4 at $1000–$1FFF */
        for (int i = 0; i < 4; i++) s->mmc3_chr_offsets[i]   = c[i];
        for (int i = 0; i < 4; i++) s->mmc3_chr_offsets[i+4] = c[i+4];
    } else {
        /* 1 KB×4 at $0000–$0FFF, 2 KB at $1000, 2 KB at $1800 */
        for (int i = 0; i < 4; i++) s->mmc3_chr_offsets[i]   = c[i+4];
        for (int i = 0; i < 4; i++) s->mmc3_chr_offsets[i+4] = c[i];
    }
}

static void mmc3_cpu_write(NesState *s, uint16_t addr, uint8_t val) {
    bool odd = (addr & 1) != 0;
    if (addr < 0xA000) {
        if (!odd) { s->mmc3_bank_sel = val; }
        else      { s->mmc3_regs[s->mmc3_bank_sel & 7] = val; mmc3_update_banks(s); }
    } else if (addr < 0xC000) {
        if (!odd && s->cart.mirror != RP2C02_MIRROR_4SCREEN)
            s->ppu.mirror = (val & 1) ? RP2C02_MIRROR_HORIZONTAL : RP2C02_MIRROR_VERTICAL;
        /* $A001 PRG-RAM protect: ignored */
    } else if (addr < 0xE000) {
        if (!odd) { s->mmc3_irq_latch = val; }
        else      { s->mmc3_irq_reload = true; s->mmc3_irq_counter = 0; }
    } else {
        if (!odd) { s->mmc3_irq_enabled = false; s->cpu.irq = false; }
        else      { s->mmc3_irq_enabled = true; }
    }
}

static void mmc3_irq_scanline(void *ud) {
    NesState *s = ud;
    if (s->mmc3_irq_reload || s->mmc3_irq_counter == 0) {
        s->mmc3_irq_counter = s->mmc3_irq_latch;
        s->mmc3_irq_reload  = false;
    } else {
        s->mmc3_irq_counter--;
    }
    if (s->mmc3_irq_counter == 0 && s->mmc3_irq_enabled)
        s->cpu.irq = true;
}

/* ── Mapper 178 (WaixingFS) ──────────────────────────────────────────────── */

static void m178_update_banks(NesState *s) {
    uint8_t  prg_mode = (s->m178_mode >> 1) & 3u;
    uint32_t prg_size = (uint32_t)s->cart.prg_banks * 0x4000u;
    uint32_t outer    = (uint32_t)s->m178_prg_hi * 0x20000u;
    uint32_t inner    = (uint32_t)(s->m178_prg_lo & 7u) * 0x4000u;

    switch (prg_mode) {
    case 0: /* NROM-256/BNROM: 32KB page, CPU A14 = PRG A14 */
        {
            uint32_t base = (outer + (uint32_t)(s->m178_prg_lo & 6u) * 0x4000u) % prg_size;
            s->prg_offsets[0] = base;
            s->prg_offsets[1] = (base + 0x4000u) % prg_size;
        }
        break;
    case 1: /* UNROM: switchable $8000–$BFFF, last inner bank fixed at $C000 */
        s->prg_offsets[0] = (outer + inner) % prg_size;
        s->prg_offsets[1] = (outer + 7u * 0x4000u) % prg_size;
        break;
    case 2: /* NROM-128: same 16KB bank mirrored at both windows */
        s->prg_offsets[0] = (outer + inner) % prg_size;
        s->prg_offsets[1] = (outer + inner) % prg_size;
        break;
    default: /* mode 3: UNROM variant — treat as mode 1 */
        s->prg_offsets[0] = (outer + inner) % prg_size;
        s->prg_offsets[1] = (outer + 7u * 0x4000u) % prg_size;
        break;
    }

    s->ppu.mirror = (s->m178_mode & 1u) ? RP2C02_MIRROR_HORIZONTAL : RP2C02_MIRROR_VERTICAL;
}

/* ── CHR bus callbacks (PPU address space 0x0000–0x1FFF) ─────────────────── */

static uint8_t nes_chr_read(uint16_t addr, void *ud) {
    NesState *s = ud;
    if (!s->chr) return 0;
    if (s->cart.mapper == 4) {
        if (s->chr_is_ram) return s->chr[addr & 0x1FFF];
        return s->chr[s->mmc3_chr_offsets[addr >> 10] + (addr & 0x3FF)];
    }
    if (s->cart.mapper >= 1) {
        uint8_t slot = addr >= 0x1000 ? 1 : 0;
        return s->chr[s->chr_offsets[slot] + (addr & 0x0FFF)];
    }
    return s->chr[addr & 0x1FFF];
}

static void nes_chr_write(uint16_t addr, uint8_t val, void *ud) {
    NesState *s = ud;
    if (!s->chr_is_ram) return;
    if (s->cart.mapper == 4) { s->chr[addr & 0x1FFF] = val; return; }
    if (s->cart.mapper >= 1) {
        uint8_t slot = addr >= 0x1000 ? 1 : 0;
        s->chr[s->chr_offsets[slot] + (addr & 0x0FFF)] = val;
    } else {
        s->chr[addr & 0x1FFF] = val;
    }
}

/* ── CPU memory map ──────────────────────────────────────────────────────── */

/* ── Game Genie ──────────────────────────────────────────────────────────── */

static const char gg_alpha[] = "APZLGITYEOXUKSVN";

static bool gg_decode(const char *code, uint16_t *addr, uint8_t *val,
                      uint8_t *cmp, bool *has_cmp) {
    int len = (int)strlen(code);
    if (len != 6 && len != 8) return false;
    uint8_t n[8] = {0};
    for (int i = 0; i < len; i++) {
        const char *p = strchr(gg_alpha, toupper((unsigned char)code[i]));
        if (!p) return false;
        n[i] = (uint8_t)(p - gg_alpha);
    }
    /* NES Game Genie bit layout per nesgg.txt:
     *   val[7]=n0[3]  val[6:4]=n1[2:0]  val[3]=n5[3](6ch)/n7[3](8ch)  val[2:0]=n0[2:0]
     *   addr[14:12]=n3[2:0]  addr[11]=n4[3]   addr[10:8]=n5[2:0]
     *   addr[7]=n1[3]  addr[6:4]=n2[2:0]  addr[3]=n3[3]  addr[2:0]=n4[2:0]
     *   cmp[7]=n6[3]  cmp[6:4]=n7[2:0]  cmp[3]=n5[3]  cmp[2:0]=n6[2:0]  (8-char) */
    *addr = 0x8000u
        | ((uint16_t)(n[3] & 7u) << 12)
        | ((uint16_t)(n[4] & 8u) <<  8)
        | ((uint16_t)(n[5] & 7u) <<  8)
        | ((uint16_t)(n[1] & 8u) <<  4)
        | ((uint16_t)(n[2] & 7u) <<  4)
        | ((uint16_t)(n[3] & 8u))
        | ((uint16_t)(n[4] & 7u));
    *has_cmp = (len == 8);
    if (*has_cmp) {
        *val = (uint8_t)(((n[0] & 8u) << 4) | ((n[1] & 7u) << 4) | (n[7] & 8u) | (n[0] & 7u));
        *cmp = (uint8_t)(((n[6] & 8u) << 4) | ((n[7] & 7u) << 4) | (n[5] & 8u) | (n[6] & 7u));
    } else {
        *val = (uint8_t)(((n[0] & 8u) << 4) | ((n[1] & 7u) << 4) | (n[5] & 8u) | (n[0] & 7u));
        *cmp = 0;
    }
    return true;
}

static uint8_t nes_prg_direct(const NesState *s, uint16_t addr) {
    if (addr < 0x8000 || !s->prg) return 0;
    if (s->cart.mapper == 4) {
        uint8_t slot = (uint8_t)((addr - 0x8000u) >> 13);
        return s->prg[s->mmc3_prg_offsets[slot] + (addr & 0x1FFFu)];
    }
    if (s->cart.mapper >= 1) {
        uint8_t slot = addr >= 0xC000 ? 1 : 0;
        return s->prg[s->prg_offsets[slot] + (addr & 0x3FFF)];
    }
    uint32_t off = addr - 0x8000u;
    if (s->cart.prg_banks == 1) off &= 0x3FFF;
    return s->prg[off];
}

static void nes_sync_ppu_to_cpu(NesState *s, uint64_t cpu_cycle) {
    while (s->ppu_synced_cpu_cycle < cpu_cycle) {
        s->ppu_synced_cpu_cycle++;
        if (s->fds_enabled)
            s->cpu.irq = fds_tick(&s->fds);
        for (int i = 0; i < 3; i++) {
            rp2c02_tick(&s->ppu);
            if (s->ppu.nmi_pending) {
                s->cpu.nmi = true;
                s->ppu.nmi_pending = false;
            }
            if (s->ppu.dirty)
                return;
        }
    }
}

static void nes_gamegenie_cmd(NesState *s, const char *line) {
    /* skip "gamegenie" */
    while (*line && !isspace((unsigned char)*line)) line++;
    while (*line && isspace((unsigned char)*line)) line++;

    char sub[32] = {0};
    int  si = 0;
    while (*line && !isspace((unsigned char)*line) && si < 31)
        sub[si++] = (char)tolower((unsigned char)*line++);
    while (*line && isspace((unsigned char)*line)) line++;
    const char *arg = line;
    /* trim trailing whitespace from arg */
    char argbuf[32] = {0};
    snprintf(argbuf, sizeof(argbuf), "%s", arg);
    for (int i = (int)strlen(argbuf) - 1; i >= 0 && isspace((unsigned char)argbuf[i]); i--)
        argbuf[i] = '\0';
    arg = argbuf;

    if (strcmp(sub, "list") == 0) {
        if (!*arg) {
            if (s->gg_count == 0) { printf("gamegenie: no patches active\n"); return; }
            printf("Active Game Genie patches:\n");
            for (int i = 0; i < s->gg_count; i++) {
                NesGgPatch *g = &s->gg_patches[i];
                uint8_t actual = nes_prg_direct(s, g->addr);
                if (g->has_cmp) {
                    const char *status = (actual == g->cmp) ? "active" : "mismatch";
                    printf("  %-8s  $%04X = $%02X  (if $%02X)  ROM=$%02X [%s]\n",
                           g->code, g->addr, g->val, g->cmp, actual, status);
                } else {
                    printf("  %-8s  $%04X = $%02X  ROM=$%02X [active]\n",
                           g->code, g->addr, g->val, actual);
                }
            }
        } else {
            bool found = false;
            for (int i = 0; i < s->gg_count; i++) {
                if (strcasecmp(s->gg_patches[i].code, arg) == 0) {
                    NesGgPatch *g = &s->gg_patches[i];
                    uint8_t actual = nes_prg_direct(s, g->addr);
                    if (g->has_cmp) {
                        const char *status = (actual == g->cmp) ? "active" : "mismatch";
                        printf("  %-8s  $%04X = $%02X  (if $%02X)  ROM=$%02X [%s]\n",
                               g->code, g->addr, g->val, g->cmp, actual, status);
                    } else {
                        printf("  %-8s  $%04X = $%02X  ROM=$%02X [active]\n",
                               g->code, g->addr, g->val, actual);
                    }
                    found = true; break;
                }
            }
            if (!found) printf("gamegenie: code '%s' not found\n", arg);
        }
        return;
    }

    if (strcmp(sub, "add") == 0) {
        if (!*arg) { printf("usage: gamegenie add <code>\n"); return; }
        if (s->gg_count >= NES_GG_MAX) {
            printf("gamegenie: patch limit reached (%d max)\n", NES_GG_MAX); return;
        }
        for (int i = 0; i < s->gg_count; i++) {
            if (strcasecmp(s->gg_patches[i].code, arg) == 0) {
                printf("gamegenie: %s is already active\n", arg); return;
            }
        }
        uint16_t paddr; uint8_t pval, pcmp; bool has_cmp;
        if (!gg_decode(arg, &paddr, &pval, &pcmp, &has_cmp)) {
            printf("gamegenie: invalid code '%s'\n", arg); return;
        }
        NesGgPatch *g = &s->gg_patches[s->gg_count++];
        g->addr = paddr; g->val = pval; g->cmp = pcmp; g->has_cmp = has_cmp;
        snprintf(g->code, sizeof(g->code), "%s", arg);
        if (has_cmp)
            printf("gamegenie: added %s → $%04X = $%02X (if $%02X)\n", arg, paddr, pval, pcmp);
        else
            printf("gamegenie: added %s → $%04X = $%02X\n", arg, paddr, pval);
        return;
    }

    if (strcmp(sub, "delete") == 0 || strcmp(sub, "remove") == 0) {
        if (!*arg) { printf("usage: gamegenie delete <code>\n"); return; }
        for (int i = 0; i < s->gg_count; i++) {
            if (strcasecmp(s->gg_patches[i].code, arg) == 0) {
                memmove(&s->gg_patches[i], &s->gg_patches[i + 1],
                        (size_t)(s->gg_count - i - 1) * sizeof(NesGgPatch));
                s->gg_count--;
                printf("gamegenie: removed %s\n", arg);
                return;
            }
        }
        printf("gamegenie: code '%s' not found\n", arg);
        return;
    }

    printf("gamegenie add <code> | gamegenie list [code] | gamegenie delete <code>\n");
}

static uint8_t nes_cpu_read(uint16_t addr, void *ud) {
    NesState *s = ud;

    if (addr < 0x2000) return s->ram[addr & 0x07FF];

    if (addr < 0x4000) {
        nes_sync_ppu_to_cpu(s, s->cpu.cycle_count);
        return rp2c02_read(&s->ppu, (uint8_t)(addr & 7));
    }

    /* FDS I/O registers ($4020-$408F covers disk regs + wavetable + sound) */
    if (s->fds_enabled && addr >= 0x4020 && addr <= 0x408Fu) {
        uint8_t v = fds_reg_read(&s->fds, addr);
        s->cpu.irq = fds_tick(&s->fds);  /* recalculate IRQ after flag clear */
        return v;
    }

    if (addr == 0x4015) return apu2a03_read(&s->apu, 0x4015);

    if (addr == 0x4016) {
        if (s->cfg->ports[0] != NES_DEVICE_CONTROLLER) return 0;
        if (s->ctrl_strobe) return (s->ctrl_state[0] & NES_BTN_A) ? 1u : 0u;
        uint8_t bit = s->ctrl_shift[0] & 1;
        s->ctrl_shift[0] = (s->ctrl_shift[0] >> 1) | 0x80u;
        return bit;
    }
    if (addr == 0x4017) {
        if (s->cfg->ports[1] == NES_DEVICE_ZAPPER) {
            /* Bit 4: trigger (1 = half-pulled); bit 3: light sense (0 = detected) */
            uint8_t trigger = (s->zapper_trigger_ttl > 0) ? 0x10u : 0u;
            uint8_t light   = 0x08u; /* default: not detected */
            int sl = s->ppu.scanline;
            if (sl >= 0 && sl < 240 &&
                s->zapper_x >= 0 && s->zapper_x < 256 &&
                s->zapper_y >= 0 && s->zapper_y < 240 &&
                abs(sl - s->zapper_y) <= 8) {
                uint32_t argb = s->ppu.pixels_argb[s->zapper_y * RP2C02_WIDTH + s->zapper_x];
                uint8_t r = (uint8_t)(argb >> 16);
                uint8_t g = (uint8_t)(argb >>  8);
                uint8_t b = (uint8_t)(argb);
                uint8_t luma = (uint8_t)((r * 77u + g * 150u + b * 29u) >> 8);
                if (luma >= 85) light = 0; /* bright pixel → light detected */
            }
            return trigger | light;
        }
        if (s->cfg->ports[1] != NES_DEVICE_CONTROLLER) return 0;
        if (s->ctrl_strobe) return (s->ctrl_state[1] & NES_BTN_A) ? 1u : 0u;
        uint8_t bit = s->ctrl_shift[1] & 1;
        s->ctrl_shift[1] = (s->ctrl_shift[1] >> 1) | 0x80u;
        return bit;
    }

    if (addr >= 0x6000 && addr < 0x8000) {
        if (s->fds_enabled) return s->fds.ram[addr - 0x6000u];
        if ((s->cart.mapper == 1 && !(s->mmc1_prg & 0x10)) ||
            s->cart.mapper == 4 || s->cart.mapper == 178)
            return s->prg_ram[addr & 0x1FFF];
        return 0;
    }

    if (addr >= 0x8000) {
        if (s->fds_enabled) {
            if (addr >= 0xE000)
                return s->fds.bios[addr - 0xE000u];          /* BIOS ROM */
            return s->fds.ram[addr - 0x6000u];               /* $8000–$DFFF: upper FDS RAM */
        }
        if (!s->prg) return 0;
        uint8_t rom;
        if (s->cart.mapper == 4) {
            uint8_t slot = (uint8_t)((addr - 0x8000u) >> 13);
            rom = s->prg[s->mmc3_prg_offsets[slot] + (addr & 0x1FFFu)];
        } else if (s->cart.mapper >= 1) {
            uint8_t slot = addr >= 0xC000 ? 1 : 0;
            rom = s->prg[s->prg_offsets[slot] + (addr & 0x3FFF)];
        } else {
            /* NROM: 1 bank → mirrored; 2 banks → direct 32 KB */
            uint32_t off = addr - 0x8000u;
            if (s->cart.prg_banks == 1) off &= 0x3FFF;
            rom = s->prg[off];
        }
        for (int i = 0; i < s->gg_count; i++) {
            if (s->gg_patches[i].addr == addr &&
                (!s->gg_patches[i].has_cmp || rom == s->gg_patches[i].cmp))
                return s->gg_patches[i].val;
        }
        return rom;
    }

    return 0;  /* open bus */
}

static void nes_cpu_write(uint16_t addr, uint8_t val, void *ud) {
    NesState *s = ud;

    if (addr < 0x2000) { s->ram[addr & 0x07FF] = val; return; }

    if (addr < 0x4000) {
        nes_sync_ppu_to_cpu(s, s->cpu.cycle_count);
        rp2c02_write(&s->ppu, (uint8_t)(addr & 7), val);
        return;
    }

    /* FDS I/O registers ($4020-$408A covers disk regs, wavetable and sound) */
    if (s->fds_enabled && addr >= 0x4020 && addr <= 0x408Au) {
        fds_reg_write(&s->fds, addr, val);
        /* $4025 bit 3 controls mirroring (0=vertical, 1=horizontal) */
        if (addr == 0x4025)
            s->ppu.mirror = (val & 0x08) ? RP2C02_MIRROR_HORIZONTAL : RP2C02_MIRROR_VERTICAL;
        s->cpu.irq = fds_tick(&s->fds);
        return;
    }

    if (addr == 0x4014) {
        /* OAM DMA: stall CPU 513 cycles, copy 256 bytes to OAM */
        nes_sync_ppu_to_cpu(s, s->cpu.cycle_count);
        uint8_t page[256];
        uint16_t base = (uint16_t)val << 8;
        for (int i = 0; i < 256; i++)
            page[i] = nes_cpu_read((uint16_t)(base + i), s);
        rp2c02_oam_dma(&s->ppu, page);
        s->cpu.cycle_count += 513;
        nes_sync_ppu_to_cpu(s, s->cpu.cycle_count);
        return;
    }

    if (addr == 0x4016) {
        bool new_strobe = (val & 1) != 0;
        if (s->ctrl_strobe && !new_strobe) {
            s->ctrl_shift[0] = s->ctrl_state[0];
            s->ctrl_shift[1] = s->ctrl_state[1];
        }
        s->ctrl_strobe = new_strobe;
        return;
    }

    /* APU registers */
    if ((addr >= 0x4000 && addr <= 0x4013) || addr == 0x4015 || addr == 0x4017) {
        apu2a03_write(&s->apu, addr, val);
        return;
    }

    if (s->cart.mapper == 178 && addr >= 0x4800 && addr <= 0x4803) {
        switch (addr) {
        case 0x4800: s->m178_mode   = val; break;
        case 0x4801: s->m178_prg_lo = val; break;
        case 0x4802: s->m178_prg_hi = val; break;
        default: break; /* $4803: PRG-RAM bank — stored implicitly, single 8KB bank only */
        }
        m178_update_banks(s);
        return;
    }

    if (addr >= 0x6000 && addr < 0x8000) {
        if (s->fds_enabled) { s->fds.ram[addr - 0x6000u] = val; return; }
        if ((s->cart.mapper == 1 && !(s->mmc1_prg & 0x10)) ||
            s->cart.mapper == 4 || s->cart.mapper == 178)
            s->prg_ram[addr & 0x1FFF] = val;
        return;
    }

    /* FDS: $8000–$DFFF is writable RAM */
    if (s->fds_enabled && addr >= 0x8000 && addr < 0xE000) {
        s->fds.ram[addr - 0x6000u] = val;
        return;
    }

    if (addr >= 0x8000) {
        if      (s->cart.mapper == 1) mmc1_serial_write(s, addr, val);
        else if (s->cart.mapper == 2) s->prg_offsets[0] = ((uint32_t)(val % s->cart.prg_banks)) * 0x4000u;
        else if (s->cart.mapper == 66) {
            /* GxROM has bus conflicts: the CPU write value is ANDed with
             * the ROM byte currently driving the data bus. */
            val &= nes_prg_direct(s, addr);
            uint32_t prg_size = (uint32_t)s->cart.prg_banks * 0x4000u;
            uint32_t chr_size = (uint32_t)s->cart.chr_banks * 0x2000u;
            s->prg_offsets[0] = (((uint32_t)(val >> 4) & 3u) * 0x8000u) % prg_size;
            s->prg_offsets[1] = s->prg_offsets[0] + 0x4000u;
            if (chr_size > 0) {
                s->chr_offsets[0] = (((uint32_t)val & 3u) * 0x2000u) % chr_size;
                s->chr_offsets[1] = s->chr_offsets[0] + 0x1000u;
            }
        }
        else if (s->cart.mapper == 4) mmc3_cpu_write(s, addr, val);
        else if (s->cart.mapper == 228) {
            /* Action 52 — NESdev register layout (address + data bus):
             *   FEDCBA98 76543210  (full 16-bit address)
             *   1.MHHPPP PPS.CCCC
             * D(13)=mirror  C:B(12:11)=chip-sel  A→6(10:6)=page(5-bit)
             * 5=PRG-mode  3:0=chr_hi   data[1:0]=chr_lo
             * Three 512KB chips at chip-sel 0,1,3; chip-sel 2 = open bus. */
            uint8_t chip     = (uint8_t)((addr >> 11) & 3u);
            uint8_t page     = (uint8_t)((addr >> 6) & 0x1Fu);
            uint8_t prg_mode = (uint8_t)((addr >> 5) & 1u);
            uint8_t chr_hi   = (uint8_t)(addr & 0xFu);
            uint8_t mirror   = (uint8_t)((addr >> 13) & 1u);
            uint8_t chr_bank = (uint8_t)((chr_hi << 2) | (val & 3u));

            if (chip == 2u) return; /* open bus — no PRG chip, ignore */

            uint32_t global_bank = (chip == 3u) ? (64u + page)
                                                 : ((uint32_t)chip * 32u + page);
            uint32_t prg_size = (uint32_t)s->cart.prg_banks * 0x4000u;
            if (!prg_mode) {
                /* 32KB split: round to even/odd pair */
                uint32_t base = (global_bank & ~1u) * 0x4000u;
                s->prg_offsets[0] = base % prg_size;
                s->prg_offsets[1] = (base + 0x4000u) % prg_size;
            } else {
                /* 16KB: same bank mirrored in both $8000 and $C000 windows */
                uint32_t base = (global_bank * 0x4000u) % prg_size;
                s->prg_offsets[0] = base;
                s->prg_offsets[1] = base;
            }

            uint32_t chr_size = (uint32_t)s->cart.chr_banks * 0x2000u;
            if (chr_size > 0) {
                s->chr_offsets[0] = ((uint32_t)chr_bank * 0x2000u) % chr_size;
                s->chr_offsets[1] = s->chr_offsets[0] + 0x1000u;
            }

            s->ppu.mirror = mirror ? RP2C02_MIRROR_HORIZONTAL : RP2C02_MIRROR_VERTICAL;
        }
    }
}

/* ── FDS media device ────────────────────────────────────────────────────── */

static GemuMediaResult fds_media_change(void *ud, const char *arg,
                                         char *err, size_t err_len) {
    NesState *s = ud;
    if (!arg || !arg[0]) { snprintf(err, err_len, "missing disk path"); return GEMU_MEDIA_ERR; }
    fds_disk_eject(&s->fds);
    if (!fds_disk_load(&s->fds, arg)) {
        snprintf(err, err_len, "failed to load '%s'", arg);
        return GEMU_MEDIA_ERR;
    }
    if (s->fds.hle_mode)
        fds_hle_boot(&s->fds, s->chr);
    return GEMU_MEDIA_OK;
}

static GemuMediaResult fds_media_eject(void *ud, char *err, size_t err_len) {
    (void)err; (void)err_len;
    fds_disk_eject(&((NesState *)ud)->fds);
    return GEMU_MEDIA_OK;
}

static void fds_media_status(void *ud, char *buf, size_t buf_len) {
    const NesState *s = ud;
    snprintf(buf, buf_len, "%s", s->fds.disk_inserted ? "disk inserted" : "no disk");
}

/* ── Cartridge media device ──────────────────────────────────────────────── */

static GemuMediaResult nes_media_change(void *ud, const char *arg,
                                         char *err, size_t err_len) {
    NesState *s = ud;
    if (!arg || !arg[0]) {
        snprintf(err, err_len, "missing cartridge path");
        return GEMU_MEDIA_ERR;
    }
    nes_sav_save(s);
    s->battery_autosave = false;
    s->sav_path[0] = '\0';
    free(s->prg); s->prg = NULL;
    free(s->chr); s->chr = NULL;
    if (!ines_load(s, arg)) {
        snprintf(err, err_len, "failed to load '%s'", arg);
        return GEMU_MEDIA_ERR;
    }
    nes_battery_setup(s);
    return GEMU_MEDIA_OK_RESET;
}

static GemuMediaResult nes_media_eject(void *ud, char *err, size_t err_len) {
    (void)err; (void)err_len;
    NesState *s = ud;
    nes_sav_save(s);
    s->battery_autosave = false;
    s->sav_path[0] = '\0';
    free(s->prg); s->prg = NULL;
    free(s->chr); s->chr = NULL;
    s->cart_path_buf[0] = '\0';
    printf("cartridge: ejected\n");
    return GEMU_MEDIA_OK;
}

static void nes_media_status(void *ud, char *buf, size_t buf_len) {
    const NesState *s = ud;
    snprintf(buf, buf_len, "%s",
             s->cart_path_buf[0] ? s->cart_path_buf : "no cartridge");
}

/* ── VNC key → controller ─────────────────────────────────────────────────── */

/* X11 keysym constants used by VNC */
#define XK_Left   0xFF51u
#define XK_Up     0xFF52u
#define XK_Right  0xFF53u
#define XK_Down   0xFF54u
#define XK_Return 0xFF0Du
#define XK_ShiftR 0xFFE2u

static void nes_handle_keys(NesState *s) {
    /* Headless mode: clear state each frame (no display to assign it) */
    if (!s->display && !s->vnc) {
        s->ctrl_state[0] = 0;
        s->ctrl_state[1] = 0;
    }

    /* SDL display: poll events and read back full button state */
    if (s->display) {
        nes_display_poll(s->display);
        if (s->cfg->ports[0] == NES_DEVICE_CONTROLLER)
            s->ctrl_state[0] = nes_display_ctrl1(s->display);
        if (s->cfg->ports[1] == NES_DEVICE_ZAPPER) {
            bool btn;
            nes_display_zapper(s->display, &s->zapper_x, &s->zapper_y, &btn);
            if (btn && s->zapper_trigger_ttl == 0)
                s->zapper_trigger_ttl = 10; /* 10-frame trigger pulse */
            if (s->zapper_trigger_ttl > 0)
                s->zapper_trigger_ttl--;
        }
    }

    /* Monitor key injection */
    if (s->ctrl_inject_frames > 0) {
        s->ctrl_state[0] |= s->ctrl_inject_mask;
        s->ctrl_inject_frames--;
    }

    /* VNC: drain the key queue; translate to controller buttons if port 0 has one */
    if (s->vnc) {
        GemuVncKeyEvent ev;
        while (gemu_vnc_pop_key_event(s->vnc, &ev)) {
            if (s->cfg->ports[0] != NES_DEVICE_CONTROLLER) continue;
            uint8_t btn = 0;
            switch (ev.keysym) {
            case 'z': case 'Z':   btn = NES_BTN_A;      break;
            case 'x': case 'X':   btn = NES_BTN_B;      break;
            case XK_Return:        btn = NES_BTN_START;  break;
            case XK_ShiftR:        btn = NES_BTN_SELECT; break;
            case XK_Up:            btn = NES_BTN_UP;     break;
            case XK_Down:          btn = NES_BTN_DOWN;   break;
            case XK_Left:          btn = NES_BTN_LEFT;   break;
            case XK_Right:         btn = NES_BTN_RIGHT;  break;
            default: break;
            }
            if (btn) {
                if (ev.down) s->ctrl_state[0] |=  btn;
                else         s->ctrl_state[0] &= ~btn;
            }
        }
    }
}

/* ── Screendump ──────────────────────────────────────────────────────────── */

static bool nes_screendump(void *ud, const char *path) {
    NesState *s = ud;
    int w = RP2C02_WIDTH, h = RP2C02_HEIGHT;
    uint8_t *rgb = malloc((size_t)w * (size_t)h * 3);
    if (!rgb) return false;
    for (int i = 0; i < w * h; i++) {
        uint32_t c    = rp2c02_palette_rgb[s->ppu.pixels[i] & 0x3F];
        rgb[i*3+0]    = (uint8_t)(c >> 16);
        rgb[i*3+1]    = (uint8_t)(c >>  8);
        rgb[i*3+2]    = (uint8_t)(c      );
    }
    bool ok = gemu_screendump(path, rgb, w, h);
    free(rgb);
    return ok;
}

/* ── Machine lifecycle ───────────────────────────────────────────────────── */

static void nes_reset(NesState *s) {
    rp2c02_reset(&s->ppu);
    if (s->fds_enabled) {
        /* FDS: horizontal mirroring until $4025 says otherwise */
        s->ppu.mirror = RP2C02_MIRROR_HORIZONTAL;
        s->ppu_synced_cpu_cycle = s->cpu.cycle_count;
        mos6502_reset(&s->cpu);
        apu2a03_reset(&s->apu);
        return;
    }
    s->ppu.mirror = s->cart.mirror;
    s->ppu_synced_cpu_cycle = s->cpu.cycle_count;

    /* Mapper bank state must be set before mos6502_reset reads the reset
       vector, otherwise prg_offsets[1] is 0 and the vector is fetched from
       the wrong physical bank. */
    if (s->cart.mapper == 1) {
        s->mmc1_shift       = 0;
        s->mmc1_shift_count = 0;
        s->mmc1_ctrl        = 0x0C;   /* PRG mode 3: fix last bank at $C000 */
        s->mmc1_chr0        = 0;
        s->mmc1_chr1        = 0;
        s->mmc1_prg         = 0;
        mmc1_update_banks(s);
    } else if (s->cart.mapper == 2) {
        s->prg_offsets[0] = 0;
        s->prg_offsets[1] = (uint32_t)(s->cart.prg_banks - 1) * 0x4000u;
        s->chr_offsets[0] = 0;
        s->chr_offsets[1] = 0x1000u;
    } else if (s->cart.mapper == 66) {
        s->prg_offsets[0] = 0;
        s->prg_offsets[1] = 0x4000u;
        s->chr_offsets[0] = 0;
        s->chr_offsets[1] = 0x1000u;
    } else if (s->cart.mapper == 228) {
        /* Reset maps the last two 16KB banks so the reset vector is readable */
        uint32_t last = (uint32_t)(s->cart.prg_banks - 1) * 0x4000u;
        s->prg_offsets[0] = last >= 0x4000u ? last - 0x4000u : 0;
        s->prg_offsets[1] = last;
        s->chr_offsets[0] = 0;
        s->chr_offsets[1] = 0x1000u;
    } else if (s->cart.mapper == 178) {
        s->m178_mode   = 0;
        s->m178_prg_lo = 0;
        s->m178_prg_hi = 0;
        m178_update_banks(s);
        s->chr_offsets[0] = 0;
        s->chr_offsets[1] = 0x1000u;
    } else if (s->cart.mapper == 4) {
        memset(s->mmc3_regs, 0, sizeof(s->mmc3_regs));
        s->mmc3_bank_sel    = 0;
        s->mmc3_irq_latch   = 0;
        s->mmc3_irq_counter = 0;
        s->mmc3_irq_reload  = false;
        s->mmc3_irq_enabled = false;
        s->cpu.irq          = false;
        mmc3_update_banks(s);
    }

    mos6502_reset(&s->cpu);
    apu2a03_reset(&s->apu);
}

NesState *nes_create(const MosConfig *cfg) {
    NesState *s = calloc(1, sizeof(*s));
    if (!s) return NULL;

    s->cfg         = cfg;
    s->fds_enabled = cfg->fds_enabled;

    if (s->fds_enabled) {
        /* 8 KB CHR RAM for PPU (needed before HLE boot so CHR files can load) */
        s->chr = calloc(1, FDS_CHR_SIZE);
        if (!s->chr) { free(s); return NULL; }
        s->chr_is_ram     = true;
        s->cart.chr_banks = 1;
        s->cart.mapper    = 0;

        /* BIOS: use real ROM if provided, otherwise install HLE stub */
        const char *bios_path = NULL;
        for (int i = 0; i < cfg->n_roms; i++) {
            if (cfg->roms[i].addr == 0xE000u) { bios_path = cfg->roms[i].path; break; }
        }
        if (!bios_path && cfg->n_roms > 0)
            bios_path = cfg->roms[0].path;

        if (bios_path) {
            if (!fds_bios_load(&s->fds, bios_path)) { free(s->chr); free(s); return NULL; }
        } else {
            fds_hle_build_rom(&s->fds);
            s->fds.hle_mode = true;
        }

        if (cfg->fda_path) {
            if (!fds_disk_load(&s->fds, cfg->fda_path)) { free(s->chr); free(s); return NULL; }
            if (s->fds.hle_mode)
                fds_hle_boot(&s->fds, s->chr);
        }
    } else {
        if (!cfg->cart_path) {
            fprintf(stderr, "nes: no cartridge specified — use -cartridge FILE.nes\n");
            free(s); return NULL;
        }
        if (!ines_load(s, cfg->cart_path)) { free(s); return NULL; }
    }

    /* Wire up PPU CHR bus */
    rp2c02_init(&s->ppu);
    s->ppu.chr_read  = nes_chr_read;
    s->ppu.chr_write = nes_chr_write;
    s->ppu.chr_ud    = s;
    s->ppu.mirror    = s->cart.mirror;

    mos6502_init(&s->cpu);
    s->cpu.mem_read        = nes_cpu_read;
    s->cpu.mem_write       = nes_cpu_write;
    s->cpu.mem_ud          = s;
    s->cpu.decimal_disable = (cfg->cpu != MOS_CPU_6502);

    /* APU — only initialise when sound is enabled */
    if (cfg->sound == MOS_SOUND_2A03) {
        if (!apu2a03_init(&s->apu))
            fprintf(stderr, "nes: APU audio init failed (continuing silently)\n");
        s->apu.mem_read = nes_cpu_read;
        s->apu.mem_ud   = s;
#ifdef HAVE_ALSA
    } else if (cfg->sound == MOS_SOUND_2A03_MIDI) {
        s->apu.mem_read    = nes_cpu_read;
        s->apu.mem_ud      = s;
        apu_midi_open(&s->apu_midi);
        s->apu.write_tap    = apu_midi_tap;
        s->apu.write_tap_ud = &s->apu_midi;
#endif
    }

    s->monitor = gemu_monitor_create();
    gemu_monitor_set_screendump_cb(s->monitor, nes_screendump, s);
    if (s->fds_enabled) {
        gemu_monitor_register_media(s->monitor, &(GemuMediaDevice){
            .name   = "floppy",
            .kind   = "floppy",
            .ud     = s,
            .change = fds_media_change,
            .eject  = fds_media_eject,
            .status = fds_media_status,
        });
    } else {
        gemu_monitor_register_media(s->monitor, &(GemuMediaDevice){
            .name   = "cartridge",
            .kind   = "cartridge",
            .ud     = s,
            .change = nes_media_change,
            .eject  = nes_media_eject,
            .status = nes_media_status,
        });
    }

    if (cfg->display_type == GEMU_DISPLAY_SDL ||
        cfg->display_type == GEMU_DISPLAY_GTK) {
        s->display = nes_display_create(cfg->display_type, "GEMU",
                                        rp2c02_palette_rgb,
                                        cfg->display_scale,
                                        cfg->display_renderer,
                                        s->monitor);
        if (!s->display)
            fprintf(stderr, "nes: failed to create display window\n");
    }

    nes_battery_setup(s);

    if (s->cart.mapper == 4) {
        s->ppu.irq_scanline = mmc3_irq_scanline;
        s->ppu.irq_ud       = s;
    }

    if (cfg->vnc_addr) {
        s->vnc = gemu_vnc_create(cfg->vnc_addr, RP2C02_WIDTH, RP2C02_HEIGHT);
        if (s->vnc)
            gemu_vnc_set_palette(s->vnc, rp2c02_palette_rgb, 64);
        else
            fprintf(stderr, "nes: failed to start VNC at %s\n", cfg->vnc_addr);
    }

    nes_reset(s);
    return s;
}

void nes_destroy(NesState *s) {
    if (!s->fds_enabled) nes_sav_save(s);
    if (s->fds_enabled) { free(s->fds.disk); free(s->fds.fwd_mask); free(s->fds.raw_disk); }
#ifdef HAVE_ALSA
    apu_midi_close(&s->apu_midi);
#endif
    apu2a03_destroy(&s->apu);
    gemu_monitor_destroy(s->monitor);
    nes_display_destroy(s->display);
    gemu_vnc_destroy(s->vnc);
    free(s->prg);
    free(s->chr);
    free(s);
}

/* ── Run loop ────────────────────────────────────────────────────────────── */

/* Fallback frame duration when audio is off (headless / -soundhw none). */
#define NES_FRAME_MS   17u   /* 1000/60 ≈ 16.67 ms; round up to avoid running fast */

/* Audio queue threshold for sync: allow up to 3 frames of latency (~50 ms).
 * ~735 samples/frame * 3 frames * 4 bytes/float = 8820 bytes */
#define AUDIO_QUEUE_MAX  (3u * 735u * (unsigned)sizeof(float))

void nes_run(NesState *s, const MosConfig *cfg) {
    gemu_monitor_start(s->monitor);

    bool quit = false;
    while (!quit) {
        Uint32 t0 = SDL_GetTicks();

        /* SDL event pump (headless/VNC path — SDL display polls in handle_keys) */
        if (!s->display) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev))
                if (ev.type == SDL_QUIT) quit = true;
        }

        /* Monitor commands */
        GemuMonCmd cmd;
        while ((cmd = gemu_monitor_poll(s->monitor)) != GEMU_MON_NONE) {
            if      (cmd == GEMU_MON_QUIT)   { quit = true; break; }
            else if (cmd == GEMU_MON_RESET)  { nes_sav_save(s); nes_reset(s); }
            else if (cmd == GEMU_MON_CUSTOM) {
                const char *text = gemu_monitor_command_text(s->monitor);
                while (*text == ' ' || *text == '\t') text++;
                if (strncasecmp(text, "gamegenie", 9) == 0 &&
                    (text[9] == '\0' || text[9] == ' ' || text[9] == '\t'))
                    nes_gamegenie_cmd(s, text);
                else if (strncasecmp(text, "sendkey", 7) == 0 &&
                         (text[7] == '\0' || text[7] == ' ' || text[7] == '\t')) {
                    const char *p = text + 7;
                    while (*p == ' ' || *p == '\t') p++;
                    uint8_t btn = 0;
                    if      (strncasecmp(p, "a",      1) == 0 && (p[1] < 'a' || p[1] > 'z')) btn = NES_BTN_A;
                    else if (strncasecmp(p, "b",      1) == 0 && (p[1] < 'a' || p[1] > 'z')) btn = NES_BTN_B;
                    else if (strncasecmp(p, "start",  5) == 0) btn = NES_BTN_START;
                    else if (strncasecmp(p, "select", 6) == 0) btn = NES_BTN_SELECT;
                    else if (strncasecmp(p, "up",     2) == 0) btn = NES_BTN_UP;
                    else if (strncasecmp(p, "down",   4) == 0) btn = NES_BTN_DOWN;
                    else if (strncasecmp(p, "left",   4) == 0) btn = NES_BTN_LEFT;
                    else if (strncasecmp(p, "right",  5) == 0) btn = NES_BTN_RIGHT;
                    if (btn) {
                        while (*p && *p != ' ' && *p != '\t') p++;
                        while (*p == ' ' || *p == '\t') p++;
                        int frames = (*p >= '1' && *p <= '9') ? (int)strtol(p, NULL, 10) : 5;
                        s->ctrl_inject_mask   = btn;
                        s->ctrl_inject_frames = frames;
                        printf("sendkey: holding 0x%02X for %d frame(s)\n", btn, frames);
                    } else {
                        printf("sendkey: unknown button (a b start select up down left right)\n");
                    }
                } else if (strncasecmp(text, "ppudump", 7) == 0 &&
                           (text[7] == '\0' || text[7] == ' ' || text[7] == '\t')) {
                    Rp2c02 *ppu = &s->ppu;
                    printf("PPU: ctrl=%02X mask=%02X status=%02X v=%04X t=%04X x=%d w=%d\n",
                           ppu->ppuctrl, ppu->ppumask, ppu->ppustatus,
                           ppu->v, ppu->t, ppu->x, ppu->w);
                    printf("Palette BG:  ");
                    for (int i = 0; i < 16; i++) printf("%02X ", ppu->palette[i]);
                    printf("\nPalette SPR: ");
                    for (int i = 0; i < 16; i++) printf("%02X ", ppu->palette[i + 16]);
                    printf("\nSL0 pixels[0..31]: ");
                    for (int i = 0; i < 32; i++) printf("%02X ", ppu->pixels[i]);
                    printf("\nSL8 pixels[0..31]: ");
                    for (int i = 0; i < 32; i++) printf("%02X ", ppu->pixels[256*8 + i]);
                    printf("\nNT row0 tiles: ");
                    for (int i = 0; i < 32; i++) printf("%02X ", ppu->vram[i]);
                    printf("\nNT row1 tiles: ");
                    for (int i = 0; i < 32; i++) printf("%02X ", ppu->vram[32 + i]);
                    printf("\nNT attr[0..7]: ");
                    for (int i = 0; i < 8; i++) printf("%02X ", ppu->vram[0x3C0 + i]);
                    printf("\nOAM[0..3]:     ");
                    for (int i = 0; i < 16; i++) printf("%02X ", ppu->oam[i]);
                    printf("\nOAM visible:   ");
                    int shown = 0;
                    for (int i = 0; i < 64; i++) {
                        uint8_t y = ppu->oam[i * 4 + 0];
                        if (y >= 0xEF) continue;
                        printf("#%02d:%02X %02X %02X %02X ",
                               i, y, ppu->oam[i * 4 + 1],
                               ppu->oam[i * 4 + 2], ppu->oam[i * 4 + 3]);
                        if (++shown == 12) break;
                    }
                    if (!shown) printf("(none)");
                    printf("\n");
                } else
                    gemu_monitor_unknown_command(s->monitor);
            }
        }
        if (quit) break;

        /* Input: SDL display keys + VNC events */
        nes_handle_keys(s);

        /* SDL window closed */
        if (s->display && nes_display_should_quit(s->display)) break;

        if (!gemu_monitor_is_paused(s->monitor)) {
            /* Run one full frame (until PPU marks frame complete) */
            s->ppu.dirty = false;
            while (!s->ppu.dirty) {
                uint64_t prev = s->cpu.cycle_count;
                mos6502_step(&s->cpu);
                uint64_t delta = s->cpu.cycle_count - prev;

                /* APU: one tick per CPU cycle */
                if (s->apu.audio_dev)
                    for (uint64_t i = 0; i < delta; i++) {
                        if (s->fds_enabled)
                            s->apu.fds_in = fds_audio_tick(&s->fds);
                        apu2a03_tick(&s->apu);
                    }

                nes_sync_ppu_to_cpu(s, s->cpu.cycle_count);
            }

            /* Flush APU samples to SDL audio */
            apu2a03_flush(&s->apu);

            /* Render completed frame */
            if (nes_display_has_argb(s->display))
                nes_display_render_argb(s->display, s->ppu.pixels_argb,
                                        RP2C02_WIDTH, RP2C02_HEIGHT);
            else if (s->display)
                nes_display_render(s->display, s->ppu.pixels,
                                   RP2C02_WIDTH, RP2C02_HEIGHT);
            if (s->vnc)
                gemu_vnc_update(s->vnc, s->ppu.pixels,
                                RP2C02_WIDTH, RP2C02_HEIGHT);
        }

        /* Frame sync:
         * - Audio enabled: block until SDL has consumed enough samples so the
         *   queue stays at ~3 frames. This locks emulation to the audio clock
         *   (exactly 60.099 fps) and eliminates drift entirely.
         * - Audio off / headless: fall back to a software timer. */
        if (s->apu.audio_dev) {
            while (SDL_GetQueuedAudioSize(s->apu.audio_dev) > AUDIO_QUEUE_MAX)
                SDL_Delay(1);
        } else {
            Uint32 elapsed = SDL_GetTicks() - t0;
            if (elapsed < NES_FRAME_MS)
                SDL_Delay(NES_FRAME_MS - elapsed);
        }
    }

    printf("nes: %llu frames, %llu cpu cycles\n",
           (unsigned long long)s->ppu.frame,
           (unsigned long long)s->cpu.cycle_count);

    gemu_monitor_stop(s->monitor);
    (void)cfg;
}
