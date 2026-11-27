/* NES netplay: nes_frontend.h.  The session itself is core/src/netplay.c. */
#include "nes_frontend.h"
#include "gemu/netplay.h"
#include "gemu/netplay_dialog.h"
#include "gemu/sha256.h"
#include "gemu/gemu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The session, and while a game is on, where the player was before it.
 * One NES per process. */
static Netplay       *g_np;
static NetplayDialog *g_np_dialog;   /* Netplay > Connect... / Lobby... */
static bool           g_np_active;   /* this machine is playing a netplay game */
static uint64_t       g_np_frame;
static bool           g_np_reset;    /* Reset pressed: sent with our next input */
static uint8_t       *g_poweron;     /* the machine as created: every player's start */
static size_t         g_poweron_len;
static uint8_t       *g_np_before;   /* the player's own game, put back afterwards */
static size_t         g_np_before_len;

/* ── Netplay ───────────────────────────────────────────────────────────────
 *
 * Every player's NES has to run the same frames with the same input, so a
 * netplay game starts every machine from the same state: the snapshot taken
 * when this one was created (before any battery save was loaded, with
 * PRG-RAM cleared), which is the same for everyone with the same ROM and
 * settings.  The player's own game is kept aside and put back when the
 * netplay game ends, and nothing is saved to their battery file meanwhile.
 * Controller 1 is ours; the session hands back each player's, and players 1
 * and 2 drive the two ports.  Reset goes through the session too, so every
 * machine resets on the same frame. */

#define NES_NP_SYNC_FRAMES 300   /* compare state every 5 s */

/* The game's name in the lobby: the ROM's name, and a hash of what has to
 * match for the games to stay in step (ROM contents, region, controllers). */
static void nes_np_game_name(const NesState *s, char *out, size_t len) {
    char base[256];
    nes_ui_game_name(s->cart_path_buf, base, sizeof(base));
    GemuSha256Ctx ctx;
    uint8_t d[GEMU_SHA256_DIGEST_LEN];
    gemu_sha256_init(&ctx);
    if (s->cart.prg) gemu_sha256_update(&ctx, s->cart.prg, nes_cart_prg_size(&s->cart));
    if (s->cart.chr && !s->cart.chr_is_ram) gemu_sha256_update(&ctx, s->cart.chr, nes_cart_chr_size(&s->cart));
    uint8_t cfg[4] = { (uint8_t)s->cfg->is_pal, (uint8_t)s->cfg->n_ports,
                       (uint8_t)s->cfg->ports[0], (uint8_t)s->cfg->ports[1] };
    gemu_sha256_update(&ctx, cfg, sizeof(cfg));
    gemu_sha256_final(&ctx, d);
    snprintf(out, len, "%.100s [%02x%02x%02x%02x]", base, d[0], d[1], d[2], d[3]);
}

/* The state compared between players: RAM, PRG-RAM and the CPU. */
static uint32_t nes_np_state_hash(const NesState *s) {
    uint32_t h = 2166136261u;
    const uint8_t *parts[] = { s->ram, s->cart.prg_ram };
    const size_t sizes[] = { sizeof(s->ram), sizeof(s->cart.prg_ram) };
    for (int p = 0; p < 2; p++)
        for (size_t i = 0; i < sizes[p]; i++) h = (h ^ parts[p][i]) * 16777619u;
    uint8_t cpu[6] = { s->cpu.A, s->cpu.X, s->cpu.Y, s->cpu.SP, (uint8_t)s->cpu.PC, (uint8_t)(s->cpu.PC >> 8) };
    for (int i = 0; i < 6; i++) h = (h ^ cpu[i]) * 16777619u;
    return h;
}

static void nes_np_begin(NesState *s, int player, int players) {
    const char *why = NULL;
    if (s->cfg->is_arcade)    why = "Netplay is for NES/Famicom cartridges, not the VS. System";
    else if (s->fds_enabled)  why = "Netplay doesn't support the Famicom Disk System yet";
    else if (!g_poweron)      why = "This game was loaded with Change...: restart GEMU with -cartridge "
                                    "for netplay, so it starts from a fresh power-on";
    if (!why && s->exec_hook) why = "Netplay can't run with a Lua script loaded";
    uint8_t *before = NULL;
    size_t before_len = 0;
    if (!why && !nes_snapshot_save(s, &before, &before_len)) why = "couldn't save your game state";
    if (why) {
        netplay_note(g_np, "%s - leaving the game", why);
        netplay_leave_game(g_np);
        return;
    }
    nes_ui_save_persistent(s);       /* the player's battery save, as it is now */
    g_np_before = before;
    g_np_before_len = before_len;
    g_np_active = true;              /* (so the load applies no Game Genie codes) */
    nes_ui_load_state(s, g_poweron, g_poweron_len);
    memset(s->cart.prg_ram, 0, sizeof(s->cart.prg_ram));
    /* A netplay game has a player 2 even when this NES has one controller
     * (the default): plug one into the empty port for the game. */
    s->port2_controller = players > 1 && s->cfg->ports[1] == NES_DEVICE_NONE;
    s->gg_count = 0;
    nes_gg_changed(s);
    g_np_frame = 0;
    g_np_reset = false;
    netplay_note(g_np, "Console powered on - player %d of %d%s", player, players,
                 players > 2 ? " (players 3 and up have no controller port)" : "");
    netplay_ready(g_np);
}

static void nes_np_end(NesState *s) {
    if (!g_np_active) return;
    g_np_active = false;
    s->port2_controller = false;
    if (g_np_before) nes_ui_load_state(s, g_np_before, g_np_before_len);
    free(g_np_before);
    g_np_before = NULL;
    netplay_note(g_np, "Netplay game over - back to your own game");
}

/* Before a frame: false while the other players' input hasn't arrived
 * (run nothing this time round). */
bool nes_np_frame(NesState *s) {
    int player, players;
    if (g_np && netplay_take_start(g_np, &player, &players)) nes_np_begin(s, player, players);
    if (!g_np_active) return true;
    if (gemu_monitor_is_paused(s->monitor)) return true;   /* paused: everyone waits */
    uint8_t local[NETPLAY_INPUT_BYTES] = { s->ctrl_state[0], g_np_reset ? NETPLAY_INPUT_RESET : 0 };
    uint8_t all[NETPLAY_MAX_PLAYERS * NETPLAY_INPUT_BYTES];
    int r = netplay_frame(g_np, local, all, &players);
    if (r < 0) { nes_np_end(s); return true; }
    if (r == 0) return false;
    g_np_reset = false;
    for (int p = 0; p < players; p++)
        if (all[p * NETPLAY_INPUT_BYTES + 1] & NETPLAY_INPUT_RESET) {
            nes_reset(s);
            netplay_note(g_np, "Console reset (player %d)", p + 1);
            break;
        }
    s->ctrl_state[0] = all[0];
    s->ctrl_state[1] = players > 1 ? all[NETPLAY_INPUT_BYTES] : 0;
    g_np_frame++;
    return true;
}

void nes_np_after_frame(NesState *s) {
    if (g_np_active && g_np_frame % NES_NP_SYNC_FRAMES == 0)
        netplay_sync_hash(g_np, g_np_frame, nes_np_state_hash(s));
}

bool nes_np_take_reset(void) {
    if (!g_np_active) return false;
    g_np_reset = true;
    return true;
}

bool nes_np_active(void) { return g_np_active; }

void nes_np_cart_changed(NesState *s) {
    if (!g_np) return;
    char game[NETPLAY_GAME_LEN];
    nes_np_game_name(s, game, sizeof(game));
    netplay_set_game(g_np, game);
    free(g_poweron);
    g_poweron = NULL;
}

void nes_np_refresh(void) {
    if (g_np_dialog) g_np_dialog->refresh(g_np_dialog);
}

static GemuMonCmd nes_cmd_netplay(GemuMonitor *mon, const char *args, void *ud) {
    (void)ud;
    netplay_monitor(g_np, mon, args);
    return GEMU_MON_NONE;
}

/* The Netplay menu */
static bool nes_np_offline(void *ud) {
    (void)ud;
    NetplayInfo in;
    netplay_info(g_np, &in);
    return in.state == NETPLAY_OFFLINE;
}

static bool nes_np_online(void *ud) { return !nes_np_offline(ud); }

static void nes_np_show(void *ud) {
    (void)ud;
    if (g_np_dialog) g_np_dialog->show(g_np_dialog);
}

static void nes_np_disconnect(void *ud) {
    (void)ud;
    netplay_disconnect(g_np);
}

void nes_np_setup(NesState *s, GemuDisplay *display) {
    if (s->cfg->is_arcade || s->fds_enabled || !s->cart.prg) return;
    if (!nes_snapshot_save(s, &g_poweron, &g_poweron_len)) g_poweron = NULL;
    char game[NETPLAY_GAME_LEN];
    nes_np_game_name(s, game, sizeof(game));
    g_np = netplay_create("GEMU " GEMU_VERSION_STR, game);
    gemu_monitor_bind(s->monitor, "netplay", nes_cmd_netplay, s);
    if (!display || !g_np) return;
#ifdef GEMU_GTK
    if (s->cfg->display_type == GEMU_DISPLAY_GTK) g_np_dialog = netplay_dialog_gtk_create(g_np);
#endif
    if (s->cfg->display_type == GEMU_DISPLAY_SDL) g_np_dialog = netplay_dialog_sdl_create(g_np);
    if (!g_np_dialog) return;
    gemu_display_add_netplay_item(display, "Connect...", nes_np_show, nes_np_offline, s);
    gemu_display_add_netplay_item(display, "Lobby...", nes_np_show, nes_np_online, s);
    gemu_display_add_netplay_item(display, "Disconnect", nes_np_disconnect, nes_np_online, s);
}

void nes_np_destroy(NesState *s) {
    nes_np_end(s);              /* the player's own game back */
    if (g_np_dialog) { g_np_dialog->destroy(g_np_dialog); g_np_dialog = NULL; }
    netplay_destroy(g_np);
    g_np = NULL;
    free(g_poweron);
    g_poweron = NULL;
}

