/* Netplay sessions - see gemu/netplay.h.  This part is the same for every
 * mode: the session's state, the lists and log the windows show, the queue
 * of frames the machine plays, and the desync check.  The protocol is the
 * mode's (netplay_kaillera.c). */
#include "netplay_priv.h"
#include "gemu/util.h"
#include "gemu/monitor.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  include <windows.h>
#else
#  include <time.h>
#endif

#define SYNC_PREFIX "~gemu-sync "

uint32_t np_now_ms(void) {
#ifdef _WIN32
    return (uint32_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
#endif
}

const char *netplay_conn_name(int conn) {
    static const char *const names[] = { "?", "LAN", "Excellent", "Good", "Average", "Low", "Bad" };
    return conn >= NETPLAY_CONN_MIN && conn <= NETPLAY_CONN_MAX ? names[conn] : names[0];
}

const char *netplay_mode_name(NetplayMode mode) {
    return mode == NETPLAY_MODE_KAILLERA ? "Kaillera" : "?";
}

static const NetplayBackend *backend(NetplayMode mode) {
    return mode == NETPLAY_MODE_KAILLERA ? &netplay_kaillera : NULL;
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

Netplay *netplay_create(const char *emulator, const char *game) {
    Netplay *np = calloc(1, sizeof(*np));
    if (!np) return NULL;
    pthread_mutex_init(&np->lock, NULL);
    snprintf(np->emulator, sizeof(np->emulator), "%s", emulator ? emulator : "GEMU");
    snprintf(np->game, sizeof(np->game), "%s", game ? game : "");
    np->sock = NP_BAD_SOCK;
    np->info.state = NETPLAY_OFFLINE;
    return np;
}

void netplay_destroy(Netplay *np) {
    if (!np) return;
    netplay_disconnect(np);
    np->quit_lists = true;
    if (np->list_running) pthread_join(np->list_thread, NULL);
    if (np->ping_running) pthread_join(np->ping_thread, NULL);
    free(np->servers);
    free(np->proto);
    free(np->users);
    free(np->games);
    pthread_mutex_destroy(&np->lock);
    free(np);
}

void netplay_set_game(Netplay *np, const char *game) {
    if (!np) return;
    pthread_mutex_lock(&np->lock);
    snprintf(np->game, sizeof(np->game), "%s", game ? game : "");
    np_changed(np);
    pthread_mutex_unlock(&np->lock);
}

const char *netplay_game_name(Netplay *np) { return np ? np->game : ""; }

/* ── Helpers (lock held) ─────────────────────────────────────────────────── */

void np_changed(Netplay *np) { np->version++; }

void np_logf(Netplay *np, const char *fmt, ...) {
    char line[NETPLAY_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    /* the window's font is ASCII: anything else shows as '?' */
    for (char *p = line; *p; p++)
        if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E) *p = '?';
    int i = (np->log_head + np->log_n) % NP_LOG_LINES;
    if (np->log_n == NP_LOG_LINES) np->log_head = (np->log_head + 1) % NP_LOG_LINES;
    else np->log_n++;
    snprintf(np->log[i], sizeof(np->log[i]), "%s", line);
    np_changed(np);
}

void np_set_state(Netplay *np, NetplayState st) {
    np->info.state = st;
    np_changed(np);
}

void np_clear_game(Netplay *np) {
    np->info.game_id = 0;
    np->info.owner = false;
    np->info.player = np->info.players = np->info.delay = 0;
    np->n_room = 0;
    np->start_pending = false;
    np->prefill = 0;
    np->q_r = np->q_w = 0;
    np->n_pending = 0;
    np->local_taken = false;
    np->n_mine = np->n_theirs = 0;
    np->desync_frame = 0;
    np->dropped = 0;
    np_changed(np);
}

void np_clear_lists(Netplay *np) {
    np->n_users = np->n_games = 0;
    np_clear_game(np);
}

NetplayUser *np_user_add(Netplay *np, const NetplayUser *u) {
    for (int i = 0; i < np->n_users; i++)
        if (np->users[i].id == u->id) { np->users[i] = *u; np_changed(np); return &np->users[i]; }
    if (np->n_users == np->cap_users) {
        int cap = np->cap_users ? np->cap_users * 2 : 64;
        NetplayUser *n = realloc(np->users, (size_t)cap * sizeof(*n));
        if (!n) return NULL;
        np->users = n;
        np->cap_users = cap;
    }
    np->users[np->n_users] = *u;
    np_changed(np);
    return &np->users[np->n_users++];
}

void np_user_remove(Netplay *np, uint16_t id) {
    for (int i = 0; i < np->n_users; i++)
        if (np->users[i].id == id) {
            np->users[i] = np->users[--np->n_users];
            np_changed(np);
            return;
        }
}

NetplayGame *np_game_find(Netplay *np, uint32_t id) {
    for (int i = 0; i < np->n_games; i++)
        if (np->games[i].id == id) return &np->games[i];
    return NULL;
}

NetplayGame *np_game_add(Netplay *np, const NetplayGame *g) {
    NetplayGame *have = np_game_find(np, g->id);
    if (have) { *have = *g; np_changed(np); return have; }
    if (np->n_games == np->cap_games) {
        int cap = np->cap_games ? np->cap_games * 2 : 32;
        NetplayGame *n = realloc(np->games, (size_t)cap * sizeof(*n));
        if (!n) return NULL;
        np->games = n;
        np->cap_games = cap;
    }
    np->games[np->n_games] = *g;
    np_changed(np);
    return &np->games[np->n_games++];
}

void np_game_remove(Netplay *np, uint32_t id) {
    for (int i = 0; i < np->n_games; i++)
        if (np->games[i].id == id) {
            memmove(&np->games[i], &np->games[i + 1], (size_t)(np->n_games - i - 1) * sizeof(np->games[0]));
            np->n_games--;
            np_changed(np);
            return;
        }
}

void np_room_add(Netplay *np, const NetplayUser *u) {
    for (int i = 0; i < np->n_room; i++)
        if (np->room[i].id == u->id) { np->room[i] = *u; np_changed(np); return; }
    if (np->n_room < NETPLAY_MAX_PLAYERS) np->room[np->n_room++] = *u;
    np_changed(np);
}

void np_room_remove(Netplay *np, uint16_t id) {
    for (int i = 0; i < np->n_room; i++)
        if (np->room[i].id == id) {
            memmove(&np->room[i], &np->room[i + 1], (size_t)(np->n_room - i - 1) * sizeof(np->room[0]));
            np->n_room--;
            np_changed(np);
            return;
        }
}

/* Kaillera's clients hand the emulator (delay + 1) * conn - 1 frames of no
 * input before the first that came back from the server; the server buffers
 * the rest of the delay per player, so every player's machine sees the same
 * input on the same frame. */
void np_game_started(Netplay *np, int player, int players, int delay) {
    np->info.player = player;
    np->info.players = players < 1 ? 1 : players > NETPLAY_MAX_PLAYERS ? NETPLAY_MAX_PLAYERS : players;
    np->info.delay = (delay + 1) * np->info.conn - 1;
    np->info.frame = 0;
    np->prefill = np->info.delay;
    np->q_r = np->q_w = 0;
    np->n_pending = 0;
    np->local_taken = false;
    np->n_mine = np->n_theirs = 0;
    np->desync_frame = 0;
    np->dropped = 0;
    np->start_pending = true;
    np_set_state(np, NETPLAY_STARTING);
}

void np_game_playing(Netplay *np) {
    np_set_state(np, NETPLAY_PLAYING);
    np_logf(np, "* Game on: you are player %d of %d, input delay %d frame%s",
            np->info.player, np->info.players, np->info.delay, np->info.delay == 1 ? "" : "s");
}

void np_push_frames(Netplay *np, const uint8_t *data, size_t len) {
    size_t fb = (size_t)np->info.players * NETPLAY_INPUT_BYTES;
    if (!fb) return;
    for (size_t off = 0; off + fb <= len; off += fb) {
        if (np->q_w - np->q_r >= NP_QUEUE) np->q_r++;   /* can't happen in lockstep; keep the newest */
        memcpy(np->queue[np->q_w % NP_QUEUE], data + off, fb);
        np->q_w++;
    }
}

/* ── Desync check ────────────────────────────────────────────────────────── */

/* Both sides post "frame hash" to the game's chat every few seconds; a
 * pair that differs means the machines have drifted apart. */
static void compare_hashes(Netplay *np) {
    for (int i = 0; i < np->n_mine; i++)
        for (int j = 0; j < np->n_theirs; j++)
            if (np->mine[i].frame == np->theirs[j].frame &&
                np->mine[i].hash != np->theirs[j].hash && !np->desync_frame) {
                np->desync_frame = np->mine[i].frame;
                np_logf(np, "*** Desync: the games differ at frame %llu - "
                            "check everyone has the same ROM and settings",
                        (unsigned long long)np->mine[i].frame);
            }
}

static void remember(void *list, int *n, uint64_t frame, uint32_t hash) {
    struct { uint64_t frame; uint32_t hash; } *l = list;
    if (*n == NP_HASHES) { memmove(&l[0], &l[1], (NP_HASHES - 1) * sizeof(l[0])); (*n)--; }
    l[*n].frame = frame;
    l[*n].hash = hash;
    (*n)++;
}

bool np_game_chat_in(Netplay *np, const char *user, const char *text) {
    (void)user;
    if (strncmp(text, SYNC_PREFIX, sizeof(SYNC_PREFIX) - 1) != 0) return false;
    unsigned long long frame;
    unsigned hash;
    if (sscanf(text + sizeof(SYNC_PREFIX) - 1, "%llu %x", &frame, &hash) == 2 &&
        strcmp(user, np->info.user) != 0) {
        remember(np->theirs, &np->n_theirs, frame, hash);
        compare_hashes(np);
    }
    return true;
}

void netplay_sync_hash(Netplay *np, uint64_t frame, uint32_t hash) {
    if (!np) return;
    pthread_mutex_lock(&np->lock);
    if (np->info.state == NETPLAY_PLAYING && np->info.players > 1) {
        remember(np->mine, &np->n_mine, frame, hash);
        char msg[64];
        snprintf(msg, sizeof(msg), SYNC_PREFIX "%llu %08x", (unsigned long long)frame, hash);
        np->be->chat(np, msg, true);
        compare_hashes(np);
    }
    pthread_mutex_unlock(&np->lock);
}

/* ── Settings (netplay.ini) ──────────────────────────────────────────────── */

#define MAX_RECENT 8

static void ini_path(char *out, size_t len) { gemu_config_path(out, len, "netplay.ini"); }

int netplay_recent_servers(char (*out)[128], int max) {
    char path[512], line[256];
    ini_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    int n = 0;
    if (!f) return 0;
    while (n < max && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "server=", 7) == 0 && line[7]) snprintf(out[n++], 128, "%s", line + 7);
    }
    fclose(f);
    return n;
}

void netplay_last_settings(char *user, size_t user_len, int *conn) {
    char path[512], line[256];
    if (user && user_len) snprintf(user, user_len, "Player");
    if (conn) *conn = NETPLAY_CONN_DEFAULT;
    ini_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strncmp(line, "user=", 5) == 0 && line[5] && user) snprintf(user, user_len, "%s", line + 5);
        else if (strncmp(line, "conn=", 5) == 0 && conn) {
            int c = atoi(line + 5);
            if (c >= NETPLAY_CONN_MIN && c <= NETPLAY_CONN_MAX) *conn = c;
        }
    }
    fclose(f);
}

void np_save_settings(const char *server, const char *user, int conn) {
    char recent[MAX_RECENT][128], path[512];
    int n = netplay_recent_servers(recent, MAX_RECENT);
    ini_path(path, sizeof(path));
    gemu_ensure_parent_dir(path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "user=%s\nconn=%d\nserver=%s\n", user, conn, server);
    for (int i = 0, kept = 1; i < n && kept < MAX_RECENT; i++)
        if (strcmp(recent[i], server) != 0) { fprintf(f, "server=%s\n", recent[i]); kept++; }
    fclose(f);
}

/* ── Session ─────────────────────────────────────────────────────────────── */

static void join_thread(Netplay *np) {
    if (!np->thread_running) return;
    pthread_join(np->thread, NULL);
    np->thread_running = false;
}

bool netplay_connect(Netplay *np, NetplayMode mode, const char *server, const char *user,
                     int conn, char *err, size_t err_len) {
    const NetplayBackend *be = backend(mode);
    if (!np || !be) { snprintf(err, err_len, "unknown netplay mode"); return false; }
    if (!server || !*server) { snprintf(err, err_len, "no server given"); return false; }
    if (!user || !*user) { snprintf(err, err_len, "no name given"); return false; }
    if (conn < NETPLAY_CONN_MIN || conn > NETPLAY_CONN_MAX) {
        snprintf(err, err_len, "connection type must be 1 (LAN) to 6 (Bad)");
        return false;
    }
    pthread_mutex_lock(&np->lock);
    bool busy = np->info.state != NETPLAY_OFFLINE;
    pthread_mutex_unlock(&np->lock);
    if (busy) { snprintf(err, err_len, "already connected - disconnect first"); return false; }
    join_thread(np);   /* a session that ended on its own */

    pthread_mutex_lock(&np->lock);
    np->be = be;
    np_clear_lists(np);
    np->info.mode = mode;
    snprintf(np->info.server, sizeof(np->info.server), "%s", server);
    snprintf(np->info.user, sizeof(np->info.user), "%s", user);
    np->info.conn = conn;
    np->quit = false;
    np_set_state(np, NETPLAY_CONNECTING);
    np_logf(np, "* Connecting to %s (%s) as %s...", server, netplay_mode_name(mode), user);
    bool ok = be->start(np, server, err, err_len);
    if (!ok) {
        np_logf(np, "* %s", err);
        np_set_state(np, NETPLAY_OFFLINE);
    }
    pthread_mutex_unlock(&np->lock);
    return ok;
}

void netplay_disconnect(Netplay *np) {
    if (!np) return;
    pthread_mutex_lock(&np->lock);
    if (np->info.state != NETPLAY_OFFLINE && np->be) np->be->stop(np);
    np->quit = true;
    pthread_mutex_unlock(&np->lock);
    join_thread(np);
    pthread_mutex_lock(&np->lock);
    if (np->sock != NP_BAD_SOCK) { np_closesocket(np->sock); np->sock = NP_BAD_SOCK; }
    if (np->info.state != NETPLAY_OFFLINE) {
        np_clear_lists(np);
        np_set_state(np, NETPLAY_OFFLINE);
        np_logf(np, "* Disconnected");
    }
    pthread_mutex_unlock(&np->lock);
}

static bool connected(Netplay *np) {
    return np->info.state != NETPLAY_OFFLINE && np->info.state != NETPLAY_CONNECTING;
}

void netplay_chat(Netplay *np, const char *text) {
    if (!np || !text || !*text) return;
    pthread_mutex_lock(&np->lock);
    if (connected(np)) np->be->chat(np, text, np->info.state >= NETPLAY_ROOM);
    pthread_mutex_unlock(&np->lock);
}

void netplay_create_game(Netplay *np) {
    if (!np) return;
    pthread_mutex_lock(&np->lock);
    if (np->info.state != NETPLAY_LOBBY)
        np_logf(np, "* %s", connected(np) ? "Leave this game first" : "Not connected");
    else if (!np->game[0])
        np_logf(np, "* Load a game first");
    else
        np->be->create_game(np, np->game);
    pthread_mutex_unlock(&np->lock);
}

void netplay_join_game(Netplay *np, uint32_t id) {
    if (!np) return;
    pthread_mutex_lock(&np->lock);
    NetplayGame *g = np_game_find(np, id);
    if (np->info.state != NETPLAY_LOBBY) {
        np_logf(np, "* %s", connected(np) ? "Leave this game first" : "Not connected");
    } else if (!g) {
        np_logf(np, "* No such game");
    } else if (strcmp(g->emulator, np->emulator) != 0) {
        np_logf(np, "* That game is for %s - this is %s", g->emulator, np->emulator);
    } else if (strcmp(g->name, np->game) != 0) {
        np_logf(np, "* That game is %s - load it first (you have %s)", g->name,
                np->game[0] ? np->game : "nothing loaded");
    } else if (g->status != 0) {
        np_logf(np, "* That game has already started");
    } else {
        np->be->join_game(np, id);
    }
    pthread_mutex_unlock(&np->lock);
}

void netplay_leave_game(Netplay *np) {
    if (!np) return;
    pthread_mutex_lock(&np->lock);
    if (np->info.state >= NETPLAY_ROOM) np->be->leave_game(np);
    pthread_mutex_unlock(&np->lock);
}

void netplay_start_game(Netplay *np) {
    if (!np) return;
    pthread_mutex_lock(&np->lock);
    if (np->info.state != NETPLAY_ROOM) np_logf(np, "* Create or join a game first");
    else if (!np->info.owner) np_logf(np, "* Only the game's owner can start it");
    else np->be->start_game(np);
    pthread_mutex_unlock(&np->lock);
}

/* ── Snapshots for the windows ───────────────────────────────────────────── */

uint32_t netplay_version(Netplay *np) {
    if (!np) return 0;
    pthread_mutex_lock(&np->lock);
    uint32_t v = np->version;
    pthread_mutex_unlock(&np->lock);
    return v;
}

void netplay_info(Netplay *np, NetplayInfo *out) {
    pthread_mutex_lock(&np->lock);
    *out = np->info;
    pthread_mutex_unlock(&np->lock);
}

int netplay_users(Netplay *np, NetplayUser *out, int max) {
    pthread_mutex_lock(&np->lock);
    int n = np->n_users < max ? np->n_users : max;
    memcpy(out, np->users, (size_t)n * sizeof(*out));
    pthread_mutex_unlock(&np->lock);
    return n;
}

int netplay_games(Netplay *np, NetplayGame *out, int max) {
    pthread_mutex_lock(&np->lock);
    int n = np->n_games < max ? np->n_games : max;
    memcpy(out, np->games, (size_t)n * sizeof(*out));
    pthread_mutex_unlock(&np->lock);
    return n;
}

int netplay_room(Netplay *np, NetplayUser *out, int max) {
    pthread_mutex_lock(&np->lock);
    int n = np->n_room < max ? np->n_room : max;
    memcpy(out, np->room, (size_t)n * sizeof(*out));
    pthread_mutex_unlock(&np->lock);
    return n;
}

int netplay_log(Netplay *np, char (*out)[NETPLAY_LINE_LEN], int max) {
    pthread_mutex_lock(&np->lock);
    int n = np->log_n < max ? np->log_n : max;
    for (int i = 0; i < n; i++)
        memcpy(out[i], np->log[(np->log_head + np->log_n - n + i) % NP_LOG_LINES], NETPLAY_LINE_LEN);
    pthread_mutex_unlock(&np->lock);
    return n;
}

void netplay_note(Netplay *np, const char *fmt, ...) {
    if (!np) return;
    char line[NETPLAY_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&np->lock);
    np_logf(np, "* %s", line);
    pthread_mutex_unlock(&np->lock);
}

/* ── The game ────────────────────────────────────────────────────────────── */

bool netplay_take_start(Netplay *np, int *player, int *players) {
    if (!np) return false;
    pthread_mutex_lock(&np->lock);
    bool start = np->start_pending;
    np->start_pending = false;
    if (start) { *player = np->info.player; *players = np->info.players; }
    pthread_mutex_unlock(&np->lock);
    return start;
}

void netplay_ready(Netplay *np) {
    if (!np) return;
    pthread_mutex_lock(&np->lock);
    if (np->info.state == NETPLAY_STARTING) np->be->ready(np);
    pthread_mutex_unlock(&np->lock);
}

bool netplay_in_game(Netplay *np) {
    if (!np) return false;
    pthread_mutex_lock(&np->lock);
    bool in = np->info.state == NETPLAY_STARTING || np->info.state == NETPLAY_PLAYING;
    pthread_mutex_unlock(&np->lock);
    return in;
}

int netplay_frame(Netplay *np, const uint8_t *local, uint8_t *all, int *players) {
    if (!np) return -1;
    pthread_mutex_lock(&np->lock);
    int r = 0;
    if (np->info.state == NETPLAY_STARTING || np->start_pending) {
        r = 0;                                  /* waiting for everyone to be ready */
    } else if (np->info.state != NETPLAY_PLAYING) {
        r = -1;
    } else {
        if (!np->local_taken) {
            memcpy(np->pending + np->n_pending * NETPLAY_INPUT_BYTES, local, NETPLAY_INPUT_BYTES);
            np->local_taken = true;
            if (++np->n_pending == np->info.conn) {
                np->be->send_input(np, np->pending, np->n_pending);
                np->n_pending = 0;
            }
        }
        size_t fb = (size_t)np->info.players * NETPLAY_INPUT_BYTES;
        if (np->prefill > 0) {
            np->prefill--;
            memset(all, 0, fb);
            r = 1;
        } else if (np->q_r != np->q_w) {
            memcpy(all, np->queue[np->q_r % NP_QUEUE], fb);
            np->q_r++;
            r = 1;
        }
        if (r == 1) {
            np->local_taken = false;
            np->info.frame++;
            *players = np->info.players;
        }
    }
    pthread_mutex_unlock(&np->lock);
    return r;
}

/* ── The "netplay" monitor command ───────────────────────────────────────── */

static const char *state_name(NetplayState st) {
    static const char *const names[] = { "offline", "connecting", "in the lobby", "in a game (waiting)",
                                         "starting", "playing" };
    return st <= NETPLAY_PLAYING ? names[st] : "?";
}

void netplay_monitor(Netplay *np, GemuMonitor *mon, const char *args) {
    char sub[16] = "", a[128] = "", b[64] = "", c[16] = "";
    while (*args == ' ') args++;
    sscanf(args, "%15s %127s %63s %15s", sub, a, b, c);
    const char *rest = args + strlen(sub);
    while (*rest == ' ') rest++;
    NetplayInfo in;
    netplay_info(np, &in);

    if (!sub[0] || !strcmp(sub, "info")) {
        gemu_monitor_printf(mon, "netplay: %s", state_name(in.state));
        if (in.state != NETPLAY_OFFLINE)
            gemu_monitor_printf(mon, " - %s on %s (%s), %s connection", in.user, in.server,
                                netplay_mode_name(in.mode), netplay_conn_name(in.conn));
        gemu_monitor_printf(mon, "\n");
        if (in.state >= NETPLAY_ROOM)
            gemu_monitor_printf(mon, "  game %u%s\n", in.game_id, in.owner ? " (yours)" : "");
        if (in.state >= NETPLAY_STARTING)
            gemu_monitor_printf(mon, "  player %d of %d, delay %d, frame %llu\n", in.player, in.players,
                                in.delay, (unsigned long long)in.frame);
        gemu_monitor_printf(mon, "  this game: %s\n", np->game[0] ? np->game : "(none)");
    } else if (!strcmp(sub, "connect")) {
        char user[NETPLAY_NAME_LEN];
        int conn;
        netplay_last_settings(user, sizeof(user), &conn);
        if (!a[0]) {
            gemu_monitor_printf(mon, "usage: netplay connect <host[:port]> [name] [connection 1-6]\n");
            return;
        }
        if (b[0]) snprintf(user, sizeof(user), "%s", b);
        if (c[0]) conn = atoi(c);
        char err[160];
        if (!netplay_connect(np, NETPLAY_MODE_KAILLERA, a, user, conn, err, sizeof(err)))
            gemu_monitor_printf(mon, "netplay: %s\n", err);
    } else if (!strcmp(sub, "disconnect")) {
        netplay_disconnect(np);
    } else if (!strcmp(sub, "chat") || !strcmp(sub, "say")) {
        netplay_chat(np, rest);
    } else if (!strcmp(sub, "create")) {
        netplay_create_game(np);
    } else if (!strcmp(sub, "join")) {
        netplay_join_game(np, (uint32_t)strtoul(a, NULL, 0));
    } else if (!strcmp(sub, "leave") || !strcmp(sub, "drop")) {
        netplay_leave_game(np);
    } else if (!strcmp(sub, "start")) {
        netplay_start_game(np);
    } else if (!strcmp(sub, "users") || !strcmp(sub, "room")) {
        NetplayUser u[256];
        int n = !strcmp(sub, "users") ? netplay_users(np, u, 256) : netplay_room(np, u, 256);
        for (int i = 0; i < n; i++)
            gemu_monitor_printf(mon, "  %-5u %-24s %4u ms  %s\n", u[i].id, u[i].name, u[i].ping,
                                netplay_conn_name(u[i].conn));
        if (!n) gemu_monitor_printf(mon, "  (nobody)\n");
    } else if (!strcmp(sub, "games")) {
        NetplayGame g[256];
        int n = netplay_games(np, g, 256);
        static const char *const st[] = { "waiting", "playing", "syncing" };
        for (int i = 0; i < n; i++)
            gemu_monitor_printf(mon, "  %-5u %-40s %-5s %-8s %s (%s)\n", g[i].id, g[i].name, g[i].players,
                                g[i].status < 3 ? st[g[i].status] : "?", g[i].owner, g[i].emulator);
        if (!n) gemu_monitor_printf(mon, "  (no games)\n");
    } else if (!strcmp(sub, "servers")) {
        static NetplayServer sv[400];
        int n = netplay_servers(np, sv, 400);
        if (!strcmp(a, "refresh") || (!n && !netplay_servers_busy(np))) {
            netplay_refresh_servers(np);
            gemu_monitor_printf(mon, "netplay: fetching the server lists - try 'netplay servers' again in a moment\n");
            return;
        }
        if (netplay_servers_busy(np)) gemu_monitor_printf(mon, "netplay: still fetching...\n");
        for (int i = 0; i < n; i++)
            gemu_monitor_printf(mon, "  %-28.28s %-22s %8s users %3d games  %s\n", sv[i].name, sv[i].address,
                                sv[i].users, sv[i].games, sv[i].location);
    } else if (!strcmp(sub, "log")) {
        static char lines[NP_LOG_LINES][NETPLAY_LINE_LEN];
        int want = a[0] ? atoi(a) : 20;
        int n = netplay_log(np, lines, want > 0 && want <= NP_LOG_LINES ? want : 20);
        for (int i = 0; i < n; i++) gemu_monitor_printf(mon, "%s\n", lines[i]);
    } else {
        gemu_monitor_printf(mon, "usage: netplay [info | connect <host[:port]> [name] [conn] | disconnect |\n"
                                 "        servers [refresh] | games | users | room | create | join <id> | start | leave |\n"
                                 "        chat <text> | log [n]]\n");
    }
}
